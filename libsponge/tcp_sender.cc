#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <random>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _retransmission_timeout(retx_timeout)
    , _stream(capacity) {}

uint64_t TCPSender::bytes_in_flight() const { return _bytes_in_flight; }

void TCPSender::send_segment(TCPSegment &segment) {
    segment.header().seqno = next_seqno();
    _segments_out.push(segment);
    _outstanding_segments_out.push(segment);
    // outstanding队列不为空时开始计时
    if (!_timer_running) {
        _timer_running = true;
        _timer_elapsed = 0;
    }

    _bytes_in_flight += segment.length_in_sequence_space();
    _next_seqno += segment.length_in_sequence_space();

}

void TCPSender::fill_window() {
    // 发送SYN报文
    if (!_syn_sent) {
        _syn_sent = true;
        TCPSegment segment;
        segment.header().syn = true;
        send_segment(segment);
        return;
    }

    // 在SYN报文被ack之前，禁止发送其他报文
    // FIN报文发送后禁止发送其他报文
    if (!_syn_acked || _fin_sent)
        return;

    // receiver窗口未被占用的大小, 等于receiver的窗口的字节数减去sender已发送但未被ack的字节数
    uint64_t window_remaining_size = _window_size <= _bytes_in_flight ? 0 : _window_size - _bytes_in_flight;


    // 发送FIN报文
    if (window_remaining_size && _stream.eof()) {
        _fin_sent = true;
        TCPSegment segment;
        segment.header().fin = true;
        send_segment(segment);
        return;
    }

    while ((window_remaining_size = _window_size <= _bytes_in_flight ? 0 : _window_size - _bytes_in_flight) > 0 &&
           !_stream.buffer_empty()) {
        size_t len = min(window_remaining_size, TCPConfig::MAX_PAYLOAD_SIZE);
        TCPSegment segment;
        segment.payload() = _stream.read(len);
        // 若空间足够，则在最后一个报文捎带上FIN位
        if (_stream.eof() && window_remaining_size >= 1 + segment.length_in_sequence_space()) {
            segment.header().fin = true;
            _fin_sent = true;
        }
        send_segment(segment);
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    uint64_t abs_ackno = unwrap(ackno, _isn, _next_seqno);

    uint64_t left_bound = (!_outstanding_segments_out.empty())
        ? abs_ackno >= unwrap(_outstanding_segments_out.front().header().seqno, _isn, _next_seqno) : _next_seqno;
    // ackno的范围应在已发送没被ack的报文段中最小和最大的seqno之间
    if (abs_ackno >= left_bound && abs_ackno <= _next_seqno) {
        _window_size = window_size;

        if (!_window_size) {
            // 当window_size等于0时，将其看作1，并且不进行指数退避(exponential backoff)
            _window_size = 1;
            _back_off_RTO = false;
        } else
            _back_off_RTO = true;

        bool ack_of_new_data = false;

        while (!_outstanding_segments_out.empty()) {
            TCPSegment &front_seg = _outstanding_segments_out.front();
            uint64_t seqno = unwrap(front_seg.header().seqno, _isn, _next_seqno);

            // 所有ackno之前的数据可认为被receiver接收到
            if (seqno + front_seg.length_in_sequence_space() > abs_ackno)
                break;
            if (front_seg.header().syn) {
                _syn_acked = true;
            }
            ack_of_new_data = true;
            _bytes_in_flight -= front_seg.length_in_sequence_space();
            _outstanding_segments_out.pop();
        }
        // 有新的报文段被ack时，重置RTO,计时器和连续重发次数
        if (ack_of_new_data) {
            _retransmission_timeout = _initial_retransmission_timeout;
            if (_timer_running)
                _timer_elapsed = 0;
            _consecutive_retransmissions = 0;
        }
    }

    // outstanding队列为空时停止计时器
    if (_outstanding_segments_out.empty())
        _timer_running = false;

    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    if (_timer_running) {
        _timer_elapsed += ms_since_last_tick;
        // 超时重发，重置计时器，在receiver窗口空闲将RTO翻倍，连续重发次数加1
        if (_timer_elapsed >= _retransmission_timeout) {
            _segments_out.push(_outstanding_segments_out.front());

            if (_back_off_RTO) {
                _consecutive_retransmissions++;
                _retransmission_timeout <<= 1;
            }
            _timer_elapsed = 0;
        }
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions; }

void TCPSender::send_empty_segment() {
    TCPSegment segment;
    // 空报文不进入outstanding队列
    _segments_out.push(segment);
}
