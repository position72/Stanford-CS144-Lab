#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// For Lab 5, please replace with a real implementation that passes the
// automated checks run by `make check_lab5`.

// You will need to add private members to the class declaration in `network_interface.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    // convert IP address of next hop to raw 32-bit representation (used in ARP header)
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();

    EthernetFrame frame;
    frame.header().src = _ethernet_address;
    frame.header().type = EthernetHeader::TYPE_IPv4;
    frame.payload() = dgram.serialize();

    // arp表中不存在该IP地址的映射或已过期
    if (!_arp_table.count(next_hop_ip) || _arp_table[next_hop_ip].ttl <= _timer) {
        // pending不加入重复IP
        if (!_pending_arp.count(next_hop_ip))
            _pending_arp[next_hop_ip] = 0;
        _waiting_frames_out.push_back({frame, next_hop_ip});
        arp_request();
    } else {  // 否则直接发送
        frame.header().dst = _arp_table[next_hop_ip].mac;
        _frames_out.push(frame);
    }
}

void NetworkInterface::arp_request() {
    for (auto &it : _pending_arp) {
        // 间隔查询时间需要超过5秒
        if (_timer - it.second <= 5000)
            continue;

        ARPMessage message;
        message.opcode = ARPMessage::OPCODE_REQUEST;
        message.sender_ethernet_address = _ethernet_address;
        message.sender_ip_address = _ip_address.ipv4_numeric();
        message.target_ethernet_address = {0, 0, 0, 0, 0, 0};
        message.target_ip_address = it.first; // 查询的IP

        EthernetFrame frame;
        frame.header().src = _ethernet_address;
        frame.header().dst = ETHERNET_BROADCAST; //发送广播
        frame.header().type = EthernetHeader::TYPE_ARP;
        frame.payload() = message.serialize();

        it.second = _timer;

        _frames_out.push(frame);
    }
}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    if (frame.header().dst != _ethernet_address && frame.header().dst != ETHERNET_BROADCAST) {
        return nullopt;
    }
    if (frame.header().type == EthernetHeader::TYPE_IPv4) {
        // 检查无误后向网络层传输
        InternetDatagram datagram;
        if (datagram.parse(frame.payload()) != ParseResult::NoError) {
            return nullopt;
        }
        return datagram;
    }
    if (frame.header().type == EthernetHeader::TYPE_ARP){
        ARPMessage message;
        if (message.parse(frame.payload()) != ParseResult::NoError) {
            return nullopt;
        }
        uint32_t ip = message.sender_ip_address;
        // 记录数据帧的发送者的MAC地址，有效期为30秒
        _arp_table[ip] = {message.sender_ethernet_address, _timer + 30 * 1000};
        // 数据帧请求本机的MAC地址,向请求方发送该地址
        if (message.opcode == ARPMessage::OPCODE_REQUEST && message.target_ip_address == _ip_address.ipv4_numeric()) {
            ARPMessage reply_message;
            reply_message.opcode = ARPMessage::OPCODE_REPLY;
            reply_message.sender_ethernet_address = _ethernet_address;
            reply_message.sender_ip_address = _ip_address.ipv4_numeric();
            reply_message.target_ethernet_address = message.sender_ethernet_address;
            reply_message.target_ip_address = message.sender_ip_address;

            EthernetFrame reply_frame;
            reply_frame.header().src = _ethernet_address;
            reply_frame.header().dst = message.sender_ethernet_address;
            reply_frame.header().type = EthernetHeader::TYPE_ARP;
            reply_frame.payload() = reply_message.serialize();

            _frames_out.push(reply_frame);
        }
        _pending_arp.erase(ip);
        update_waiting_and_pending();
    }
    return nullopt;
}

void NetworkInterface::update_waiting_and_pending() {
    for (auto it = _waiting_frames_out.begin(); it != _waiting_frames_out.end();) {
        if (!_arp_table.count(it->ip) || _arp_table[it->ip].ttl <= _timer) {
            it++;
            continue;
        }
        EthernetFrame frame = it->frame;
        frame.header().dst = _arp_table[it->ip].mac;
        _frames_out.push(frame);
        auto tmp = it;
        it++;
        _waiting_frames_out.erase(tmp);
    }
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    _timer += ms_since_last_tick;
    arp_request();
}
