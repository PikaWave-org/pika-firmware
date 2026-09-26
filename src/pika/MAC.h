#pragma once

#include "ch.hpp"
#include "log.h"


#include <pika/Time.h>

#include <cstdint>
#include <span>

namespace pika {

using Address = uint32_t;
using MessageID = uint32_t;

struct Frame {
    static constexpr size_t HEADER_SIZE = 8;

    Address src_addr{};
    Address dst_addr{};
    std::array<uint8_t, 128> payload{};
    size_t payload_size{};

    size_t size() { return HEADER_SIZE + payload_size; }
};

class FrameHandler {
public:
    virtual void on_tx_done(Address addr) = 0;
    virtual void on_rx_frame(Address addr, const Frame &msg) = 0;
};

template<class RADIO>
class MAC : public chibios_rt::BaseStaticThread<1024> {
public:
    MAC(RADIO *radio) : radio_(radio) {}

    void main() override {
        while (true) {
            for (auto &peer: peers_) {
                if (peer.tx_frame.payload_size != 0) {
                    LOG("tx -> %i: %d", peer.addr, peer.tx_frame.payload_size);
                    radio_->tx(std::span(reinterpret_cast<uint8_t *>(&peer.tx_frame), peer.tx_frame.size()));
                    peer.tx_frame.payload_size = 0;
                }
            }
            Time t = now();
            auto lsn_win = listen_window(*this, t);
            while (is_inside_window(t, lsn_win)) {
                systime_t rx_finish_time;
                std::span<const uint8_t> rx_buf = radio_->rx(rx_finish_time, TIME_US2I(lsn_win.second));
                LOG("rx <- %i: %d", 0, rx_buf.size());
                t = now();
            }
            chThdSleepMilliseconds(10);
        }
    }

    void set_address(Address addr) { addr_ = addr; }

    void set_frame_handler(FrameHandler *handler) { handler_ = handler; }

    bool send_frame(Address dst_addr, std::span<const uint8_t> msg) {
        Peer &peer = get_peer(dst_addr);
        if (peer.tx_frame.payload_size != 0) {
            return false;
        }
        peer.tx_frame.src_addr = addr_;
        peer.tx_frame.dst_addr = dst_addr;
        std::copy(msg.begin(), msg.end(), peer.tx_frame.payload.data());
        peer.tx_frame.payload_size = msg.size();
        return true;
    }

private:
    static constexpr size_t PEERS_MAX = 8;

    struct Peer {
        Address addr{};
        Time listen_time{};
        Time listen_interval{};
        Time listen_duration{};
        Time last_seen{};
        Frame tx_frame{};
        Frame rx_frame{};
    };

    Address addr_ = 0;
    Time listen_time{};
    Time listen_interval{1000 * MILLISECOND};
    Time listen_duration{500 * MILLISECOND};
    RADIO *radio_ = nullptr;
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

    template<class PEER>
    std::pair<Time, Time> listen_window(PEER &peer, const Time &t) {
        int64_t n = t / peer.listen_interval;
        Time s = n * peer.listen_interval;
        if (s + peer.listen_duration > t) {
            // now is inside current listen window
            return {s, s + peer.listen_duration};
        }
        // this listen window ended, return the next one
        s += peer.listen_interval;
        return {s, s + peer.listen_duration};
    }

    static bool is_inside_window(Time &t, const std::pair<Time, Time> &win) {
        return t >= win.first and t < win.second;
    }
};

}// namespace pika
