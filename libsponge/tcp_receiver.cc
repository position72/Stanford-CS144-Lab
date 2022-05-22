#include "tcp_receiver.hh"

// Dummy implementation of a TCP receiver

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

void TCPReceiver::segment_received(const TCPSegment &seg) {
    TCPHeader header = seg.header();

    // 只接受第一个SYN报文段，第一个SYN报文段之前的数据全部丢弃
    if (!_syn) {
        if (!header.syn) return;
        _syn = true;
        _isn = header.seqno;
        _abs_seqno = 1;
    } else {
        if (header.syn) return;
    }

    // 只接受第一个FIN报文段
    if (header.fin) {
        if (_fin) return;
        _fin = true;
    }

    if (!header.syn) _abs_seqno = unwrap(header.seqno, _isn, _abs_seqno);

    // 这里SYN和FIN各占一个编号，payload为空，abs_sqeno=0时是SYN报文段，不进入ByteStream，FIN报文段只起到通知eof的作用
    if (_abs_seqno > 0) _reassembler.push_substring(seg.payload().copy(), _abs_seqno - 1, header.fin);

}

optional<WrappingInt32> TCPReceiver::ackno() const {
    if (!_syn) return nullopt;
    // FIN报文段占一个编号，Reassembler组装完所有数据段后，ackno要多算FIN这一位
    return wrap(_reassembler.first_unassembled() + 1 + (_reassembler.stream_out().input_ended()), _isn);
}

// 窗口大小可以看作ByteStream的剩余可用空间
size_t TCPReceiver::window_size() const { return _reassembler.stream_out().remaining_capacity(); }
