#pragma once

#include <cstdint>
#include <span>

namespace pika {

using Address = uint32_t;
using Time = int64_t;
using MessageID = uint32_t;

struct Frame {
    Address src_addr{};
    Address dst_addr{};
    uint32_t seq{};
    uint32_t size{};
    std::array<uint8_t, 256> payload{};
};

class FrameHandler {
public:
    void on_tx_done(Address addr) = 0;
    void on_rx_frame(Address addr, const Frame &msg) = 0;
};

class MAC {
public:
    void set_address(Address addr) { addr_ = addr; }

    void set_frame_handler(FrameHandler *handler) { handler_ = handler; }

    void send_frame(Address addr, const Frame &msg) {
        Peer &peer = get_peer(addr);
        peer.tx_frame.src_addr = addr;
    }

    void update(Time &ts) {}

private:
    static constexpr size_t PEERS_MAX = 8;

    struct Peer {
        Address addr{};
        Time listen_time{};
        Time listen_interval{};
        Time last_seen{};
        Frame tx_frame{};
        Frame rx_frame{};
    };

    Address addr_ = 0;
    FrameHandler *handler_ = nullptr;
    std::array<Peer, PEERS_MAX> peers_;
    size_t peers_num_ = 0;

    Peer &get_peer(Address addr) {
        Peer *oldest = nullptr;
        for (size_t i = 0; i < peers_num_; ++i) {
            auto &peer = peers_[i];
            if (peer.addr == addr) {
                return peer;
            }
            if (oldest == nullptr or peer.last_seen < oldest->last_seen) {
                oldest = &peer;
            }
        }
        if (peers_num_ >= PEERS_MAX) {
            // Replace the oldest peer
            *oldest = {};
            oldest->addr = addr;
            return *oldest;
        }
        // Add new peer
        Peer &new_peer = peers_[peers_num_++];
        new_peer.addr = addr;
        return new_peer;
    }
};

}// namespace pika
