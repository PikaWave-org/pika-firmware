#pragma once

#include <ch.hpp>
#undef HASH// HASH macro interferes with messgen HASH

#include <pika/FixedVector.h>
#include <pika/Time.h>
#include <pika/log.h>
#include <pika/proto/mac.h>

#include <cstdint>
#include <span>

namespace pika {

using Address = uint64_t;
using MessageID = uint32_t;

class FrameHandler {
public:
    virtual void on_data_frame_sent(Address addr) = 0;
    virtual void on_data_frame_received(Address addr, std::span<const uint8_t> frame) = 0;
};

struct ListenWindow {
    Time time;
    Time interval;
    Time duration;
};

template<class RADIO>
class MAC : public chibios_rt::BaseStaticThread<1024> {
public:
    MAC(RADIO *radio) : radio_(radio) {}

    void main() override {
        while (true) {
            // TX
            Time t = now();
            if (t > last_sync_sent_ + SYNC_INTERVAL) {
                // Broadcast sync packet
                tx_buf_[0] = proto::mac::sync::MESSAGE_ID;
                types::mac::sync sync_frame{
                        .src_addr = addr_,
                        .time = t,
                        .stratum = 1,
                        .listen_freq = radio_->freq() / 1000,
                        .listen_time = listen_window_.time - t,
                        .listen_interval = listen_window_.interval,
                        .listen_duration = listen_window_.duration,
                };
                tx_buf_.resize(1 + sync_frame.serialize(&tx_buf_[1]));
                LOG("tx sync");
                radio_->tx(tx_buf_);
            }
            t = now();
            auto lsn_win = get_listen_window(listen_window_, t);
            for (auto &peer: peers_) {
                if (t + radio_->tx_duration(peer.tx_buf.size()) >= lsn_win.first) {
                    // No enough time to transmit the packet, try another peer
                    continue;
                }

                if (peer.is_synced) {
                    // Peer is synchronized, send data frame if needed
                    if (peer.tx_buf.size() != 0) {
                        LOG("tx -> %08X: %i", peer.addr, peer.tx_buf.size());
                        types::mac::data_frame data_frame{
                                .src_addr = addr_,
                                .dst_addr = peer.addr,
                                .payload = messgen::bytes(peer.tx_buf.data(), peer.tx_buf.size()),
                        };
                        tx_buf_[0] = proto::mac::data_frame::MESSAGE_ID;
                        tx_buf_.resize(data_frame.serialize(&tx_buf_[1]) + 1);
                        radio_->tx(tx_buf_);
                        peer.tx_buf.clear();
                    }
                }
            }

            // RX
            while (is_inside_window(t, lsn_win)) {
                systime_t rx_finish_time;
                std::span<const uint8_t> rx_buf = radio_->rx(rx_finish_time, TIME_US2I(lsn_win.second));
                if (rx_buf.size() > 0) {
                    uint8_t frame_type = rx_buf[0];
                    LOG("rx type=%i size=%i", 0, frame_type, rx_buf.size());
                    proto::mac::dispatch_message(
                            rx_buf[0], messgen::bytes(&rx_buf[1], rx_buf.size() - 1), [&](const auto &recv_msg) {
                                using RecvMsgType = std::decay_t<decltype(recv_msg)>;
                                typename RecvMsgType::data_type actual_data;
                                auto res = recv_msg.deserialize(actual_data, messgen::empty_alloc, messgen::NoCopy{});
                                if (res >= 0) {
                                    handle_frame(actual_data);
                                }
                            });
                }
                t = now();
            }

            chThdSleepMilliseconds(10);
        }
    }

    void set_address(Address addr) { addr_ = addr; }

    void set_frame_handler(FrameHandler *handler) { handler_ = handler; }

    bool is_busy(Address dst_addr) {
        Peer &peer = get_peer(dst_addr);
        return peer.tx_buf.size() != 0;
    }

    template<class MSG>
    bool send_data_frame(Address dst_addr, MSG &&msg) {
        Peer &peer = get_peer(dst_addr);
        if (peer.tx_buf.size() != 0) {
            return false;
        }
        size_t sz = 1 + msg.serialized_size();
        if (peer.tx_buf.cap() >= sz) {
            peer.tx_buf.resize(sz);
            using ActualType = std::decay_t<decltype(msg)>;
            peer.tx_buf[0] = ActualType::message_type::MESSAGE_ID;
            msg.serialize(peer.tx_buf.data() + 1);
            return true;
        }
        return false;
    }

private:
    static constexpr size_t PEERS_MAX = 8;
    static constexpr Time SYNC_INTERVAL = 5 * SECOND;

    struct Peer {
        Address addr{};
        ListenWindow listen_window;
        Time last_seen{};
        bool is_synced{};
        FixedVector<uint8_t, 256> tx_buf{};
    };

    Address addr_ = 0;
    ListenWindow listen_window_{
            .time = 0,
            .interval = 1000 * MILLISECOND,
            .duration = 500 * MILLISECOND,
    };
    RADIO *radio_ = nullptr;
    FrameHandler *handler_ = nullptr;
    FixedVector<Peer, PEERS_MAX> peers_;
    Time last_sync_sent_{};

    FixedVector<uint8_t, 256> tx_buf_;

    Peer &get_peer(Address addr) {
        Peer *oldest = nullptr;
        for (auto &peer: peers_) {
            if (peer.addr == addr) {
                return peer;
            }
            if (oldest == nullptr or peer.last_seen < oldest->last_seen) {
                oldest = &peer;
            }
        }
        if (peers_.full()) {
            // Replace the oldest peer
            *oldest = {};
            oldest->addr = addr;
            return *oldest;
        }
        // Add new peer
        Peer *new_peer = peers_.add();
        *new_peer = {};
        new_peer->addr = addr;
        return *new_peer;
    }

    // Get current or next listen window
    std::pair<Time, Time> get_listen_window(const ListenWindow &win, const Time &t) {
        int64_t n = t / win.interval;
        Time s = n * win.interval;
        if (s + win.duration > t) {
            // now is inside current listen window
            return {s, s + win.duration};
        }
        // this listen window ended, return the next one
        s += win.interval;
        return {s, s + win.duration};
    }

    static bool is_inside_window(Time &t, const std::pair<Time, Time> &win) {
        return t >= win.first and t < win.second;
    }

    void handle_frame(const types::mac::sync &sync) {
        LOG("rx <- %08X: sync", sync.src_addr);
        Peer &peer = get_peer(sync.src_addr);
        peer.listen_window.time = sync.listen_time;
        peer.listen_window.interval = sync.listen_interval;
        peer.listen_window.duration = sync.listen_duration;
        peer.last_seen = now();
        peer.is_synced = true;
        if (peer.tx_buf.empty() and handler_ != nullptr) {
            handler_->on_data_frame_sent(peer.addr);
        }
    }

    void handle_frame(const types::mac::data_frame &data) {
        LOG("rx <- %08X: data_frame", data.src_addr);
        if (data.dst_addr == addr_ and handler_ != nullptr) {
            handler_->on_data_frame_received(data.src_addr, data.payload);
        }
    }
};

}// namespace pika
