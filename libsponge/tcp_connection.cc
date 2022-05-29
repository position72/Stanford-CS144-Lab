#include "tcp_connection.hh"

#include <iostream>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;
using namespace TCPReceiverStateSummary;
using namespace TCPSenderStateSummary;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _ms_since_last_received; }

void TCPConnection::send_sender_segment() {
    while (!_sender.segments_out().empty()) {
        TCPSegment &segment = _sender.segments_out().front();
        // 如果receiver的ackno存在，则发送的报文要附带ACK相关信息
        if (_receiver.ackno().has_value()) {
            segment.header().ack = true;
            segment.header().ackno = _receiver.ackno().value();
            segment.header().win = _receiver.window_size();
        }
        if (_sender.stream_in().error() || _receiver.stream_out().error()) {
            segment.header().rst = true;
            _segments_out.push(segment);
            return;
        }
        _segments_out.push(segment);
        _sender.segments_out().pop();
    }
}
/** receiver端逻辑： 1.只接受第一次的SYN和FIN报文
 *                 2.一旦接收SYN报文，即可产生ackno，以后sender发送报文时ACK必须置1
 *                 3.ackno来源：receiver的reassembler组装完最后一个字节的编号加1，由于FIN报文不参与组装，当接收完FIN报文后需额外加1
 */

/** sender端逻辑：   1.通过fill_window或send_empty_segment发送报文，再交给外部TCPConnection发送
 *                 2.若receiver能产生ackno，TCPConnection就把发送的报文添加ACK后再发送
 *                 3.第一次调用fill_window会发送SYN报文
 *                 4.当发送SYN第一次报文后，必须接受到相应的ACK报文，才能继续发送其他报文
 *                 5.stream进入eof状态，有窗口剩余空间时发送FIN报文
 *                 6.发送完FIN报文，就不再发送其他报文
 */
void TCPConnection::segment_received(const TCPSegment &seg) {
    if (!_active)
        return;

    _ms_since_last_received = 0;

    if (seg.header().rst) {
        unclean_shutdown(false);
        return;
    }

    // LISTEN
    if (_sender.next_seqno_absolute() == 0) {
        if (seg.header().syn) {
            // 进行被动连接, LISTEN -> SYN_RCVD
            _receiver.segment_received(seg);
            connect();
        }
        return;
    }

    // SYN_SENT
    if (_sender.next_seqno_absolute() == bytes_in_flight() && !_receiver.ackno().has_value()) {
        if (seg.header().syn) {
            _receiver.segment_received(seg);
            if (seg.header().ack) {
                // 对方发送ACK报文，进行第三次握手发送空ACK报文
                // SYN_SENT -> ESTABLISHED
                _sender.ack_received(seg.header().ackno, seg.header().win);
                _sender.send_empty_segment();
                send_sender_segment();
            } else {
                // 双方同时进行主动连接请求会出现这种情况，己方作为主动方转变为被动方
                // 由于之前发送一次SYN报文，这里只需发送空ACK报文
                // SYN_SENT -> SYN_RCVD
                _sender.send_empty_segment();
                send_sender_segment();
            }
        }
        return;
    }

    // SYN_RCVD
    if (_sender.next_seqno_absolute() == bytes_in_flight() && _receiver.ackno().has_value()) {
        if (seg.header().ack) {
            // SYN_RCVD -> ESTABLISHED
            _receiver.segment_received(seg);
            _sender.ack_received(seg.header().ackno, seg.header().win);
        }
        return;
    }

    //    if (_receiver.ackno().has_value() && (seg.length_in_sequence_space() == 0) &&
    //        seg.header().seqno == _receiver.ackno().value() - 1) {
    //        _sender.send_empty_segment();
    //    }
    // ESTABLISHED
    if (_sender.next_seqno_absolute() > bytes_in_flight() && !_sender.stream_in().eof()) {
        _receiver.segment_received(seg);
        _sender.ack_received(seg.header().ackno, seg.header().win);
        // 被动断开连接，ESTABLISHED -> CLOSE_WAIT
        if (seg.header().fin) {
            // 发送空ACK报文，第二次挥手
            _sender.send_empty_segment();
            send_sender_segment();
            _linger_after_streams_finish = false;
        } else if (seg.length_in_sequence_space()) {
            _sender.fill_window();
            if (_sender.segments_out().empty())
                _sender.send_empty_segment();
            send_sender_segment();
        }
        return;
    }

    // 断开连接方发送FIN报文后
    if (_sender.stream_in().eof()) {
        _receiver.segment_received(seg);
        _sender.ack_received(seg.header().ackno, seg.header().win);

        if (_sender.next_seqno_absolute() == _sender.stream_in().bytes_written() + 2) {
            if (seg.header().fin) {
                // FIN_WAIT2 -> TIME_WAIT
                // 发送空ACK报文，第四次挥手
                _sender.send_empty_segment();
                send_sender_segment();

            } else if (!_linger_after_streams_finish){
                // 被动方进入CLOSED状态
                try_clean_shutdown();
            }
        }
        return;
    }

    // CLOSE_WAIT
}

bool TCPConnection::active() const { return _active; }

size_t TCPConnection::write(const string &data) {
    size_t len = _sender.stream_in().write(data);
    _sender.fill_window();
    send_sender_segment();
    return len;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) {
    _ms_since_last_received += ms_since_last_tick;
    _sender.tick(ms_since_last_tick);  // tell the TCPSender about the passage of time.

    // abort the connection, and send a reset segment to the peer (an empty segment with
    // the rst flag set), if the number of consecutive retransmissions is more than an upper
    // limit TCPConfig::MAX RETX ATTEMPTS.
    if (_sender.consecutive_retransmissions() > TCPConfig::MAX_RETX_ATTEMPTS) {
        unclean_shutdown(true);
        return;
    }
    send_sender_segment();
    // end the connection cleanly if necessary.
    try_clean_shutdown();
}

void TCPConnection::try_clean_shutdown() {
    if (_sender.stream_in().eof() &&  _sender.bytes_in_flight() == 0 &&
        _receiver.stream_out().eof()) {
        if (!_linger_after_streams_finish || _ms_since_last_received >= 10 * _cfg.rt_timeout) {
            _active = false;
        }
    }
}

void TCPConnection::unclean_shutdown(bool need_send_rst) {
    _active = false;
    // 下面是unclean_shutdown成立的条件
    _receiver.stream_out().set_error();
    _sender.stream_in().set_error();
    // 主动结束TCP需要保证发送一个RST报文
    if (need_send_rst && _sender.segments_out().empty()) {
        _sender.send_empty_segment();
    }
    send_sender_segment();
}

void TCPConnection::end_input_stream() {
    // 主动断开方第一次挥手，发送FIN报文
    // 被动断开方第三次挥手，发送FIN报文
    _sender.stream_in().end_input();
    _sender.fill_window();
    send_sender_segment();
}

void TCPConnection::connect() {
    // 主动方第一次握手，被动方第二次握手
    _sender.fill_window();
    send_sender_segment();
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";
            //  need to send a RST segment to the peer
            unclean_shutdown(true);
            _sender.send_empty_segment();
            send_sender_segment();
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
