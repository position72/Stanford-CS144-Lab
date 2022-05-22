#include "stream_reassembler.hh"

// Dummy implementation of a stream reassembler.

// For Lab 1, please replace with a real implementation that passes the
// automated checks run by `make check_lab1`.

// You will need to add private members to the class declaration in `stream_reassembler.hh`

template<typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

StreamReassembler::StreamReassembler(const size_t capacity) : _output(capacity), _capacity(capacity) {
    _first_unacceptable = capacity;
}

//! \details This function accepts a substring (aka a segment) of bytes,
//! possibly out-of-order, from the logical stream, and assembles any newly
//! contiguous substrings and writes them into the output stream in order.
void StreamReassembler::push_substring(const string &data, const size_t index, const bool eof) {
    size_t length = data.size();
    if (!length) {
        _eof |= eof;
    } else {
        _first_unacceptable = _first_unread + _output.remaining_capacity();
        SetNode node = {data, index, index + length - 1};
        if (!check_out_of_bound(node)) return;
        cut_string(node);
        if (!overlap_insert(node)) return;
        // 只有eof=1并且data的最后一个字符进入StreamReassembler，才能认为StreamReassembler接收到eof数据
        if (node.right_point == index + length - 1) _eof |= eof;
        node.data = node.data.substr(node.left_point - index, node.right_point - node.left_point + 1);
        SegSet.insert(node);
        _unassembled_bytes += node.right_point - node.left_point + 1;
        push_output();
    }
    if (_eof && empty()) _output.end_input();
}

bool StreamReassembler::check_out_of_bound(const SetNode &node) const {
    if (node.right_point < _first_unread || node.left_point >= _first_unacceptable) return false;
    return true;
}

void StreamReassembler::cut_string(SetNode &node) {
    if (node.left_point < _first_unread) {
        node.left_point = _first_unread;
    }
    if (node.right_point >= _first_unacceptable) {
        node.right_point = _first_unacceptable - 1;
    }
}

void StreamReassembler::push_output() {
    for (auto it = SegSet.begin(); it != SegSet.end();) {
        if (it->left_point == _first_unread) {
            size_t length = it->right_point - it->left_point + 1;
            _first_unread += length;
            _unassembled_bytes -= length;
            _output.write(it->data);
            auto tmp = it;
            it++;
            SegSet.erase(tmp);
        } else break;
    }
}
bool StreamReassembler::overlap_insert(SetNode &node) {
    for (auto it = SegSet.begin(); it != SegSet.end();) {
        size_t l1 = it->left_point, r1 = it->right_point;
        size_t &l2 = node.left_point, &r2 = node.right_point;
        if (r1 < l2 || l1 > r2) {
            it++;
            continue;
        }
        if (l1 <= l2) {
            if (r1 >= r2) return false;
            l2 = r1 + 1;
            it++;
        } else {
            if (r1 <= r2) {
                auto tmp = it;
                it++;
                _unassembled_bytes -= tmp->right_point - tmp->left_point + 1;
                SegSet.erase(tmp);
            } else {
                r2 = l1 - 1;
                it++;
            }
        }
    }
    return true;
}
size_t StreamReassembler::unassembled_bytes() const { return _unassembled_bytes; }

bool StreamReassembler::empty() const { return _unassembled_bytes == 0; }

size_t StreamReassembler::first_unassembled() const { return _first_unread; }

size_t StreamReassembler::first_unacceptable() const { return _first_unacceptable; }
