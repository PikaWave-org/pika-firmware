#pragma once

#include <ch.hpp>
#undef HASH// HASH macro interferes with messgen HASH

#include <pika/Time.h>
#include <pika/log.h>
#include <pika/proto/mac.h>

#include <cstdint>
#include <span>

namespace pika {

using Address = uint32_t;
using MessageID = uint32_t;

template<class T, size_t SIZE>
class FixedVector {
public:
    T *data() { return storage_.data(); }

    size_t size() { return size_; }

    T *add(const T &value) {
        if (size_ < storage_.size()) {
            T *ret = &storage_[size_];
            *ret = value;
            ++size_;
            return ret;
        }
        return nullptr;
    }

    T *add() {
        if (size_ < storage_.size()) {
            T *ret = &storage_[size_];
            ++size_;
            return ret;
        }
        return nullptr;
    }

    void size(size_t sz) { size_ = sz; }

    void clear() { size_ = 0; }

    size_t cap() { return storage_.size(); }

    operator std::span<T>() { return {storage_.data(), size_}; }

    T *begin() { return storage_.data(); }

    T *end() { return storage_.data() + size_; }

    bool is_full() const { return size_ >= storage_.size(); }

private:
    std::array<T, SIZE> storage_{};
    size_t size_{};
};

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
                        radio_->tx(peer.tx_buf);
                        peer.tx_buf.clear();
                    }
                } else {
                    // Peer not synced, try to sync
                    std::array<uint8_t, types::mac::sync::FIXED_SIZE + 1> buf;
                    buf[0] = proto::mac::sync::MESSAGE_ID;
                    types::mac::sync sync_frame{
                            .src_addr = addr_,
                            .time = t,
                            .stratum = 1,
                            .listen_freq = radio_->freq() / 1000,
                            .listen_time = listen_window_.time - t,
                            .listen_interval = listen_window_.interval,
                            .listen_duration = listen_window_.duration,
                    };
                    sync_frame.serialize(&buf[1]);
                    LOG("tx -> %08X: sync", peer.addr);
                    radio_->tx(buf);
                }
            }

            // RX
            while (is_inside_window(t, lsn_win)) {
                systime_t rx_finish_time;
                std::span<const uint8_t> rx_buf = radio_->rx(rx_finish_time, TIME_US2I(lsn_win.second));
                if (rx_buf.size() > 0) {
                    uint8_t frame_type = rx_buf[0];
                    LOG("rx type=%i size=%i", 0, frame_type, rx_buf.size());
                    proto::mac::dispatch_message(rx_buf[0], messgen::bytes(&rx_buf[1], rx_buf.size() - 1),
                                                 [&](const auto &recv_msg) {
                                                     using RecvMsgType = std::decay_t<decltype(recv_msg)>;
                                                     typename RecvMsgType::data_type_strg actual_data;
                                                     auto res = recv_msg.deserialize(actual_data);
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

    bool send_data_frame(Address dst_addr, std::span<const uint8_t> frame) {
        Peer &peer = get_peer(dst_addr);
        if (peer.tx_buf.size() != 0) {
            return false;
        }

        types::mac::data_frame data_frame{
                .src_addr = addr_,
                .dst_addr = dst_addr,
                .seq = 0,//TODO
                .ack = 0,//TODO
                .payload = messgen::bytes(&frame),
        };

        peer.tx_buf.data()[0] = proto::mac::data_frame::MESSAGE_ID;
        peer.tx_buf.size(data_frame.serialize(&peer.tx_buf.data()[1]) + 1);
        return true;
    }

private:
    static constexpr size_t PEERS_MAX = 8;

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
        if (peers_.is_full()) {
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

    /**
     * Get current or next listen window
     */
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

    void handle_frame(const types::mac::strg::sync &sync) { LOG("rx <- %08X: sync", sync.src_addr); }

    void handle_frame(const types::mac::strg::data_frame &data) { LOG("rx <- %08X: data_frame", data.src_addr); }

    void handle_frame(const types::mac::strg::data_ack &ack) { LOG("rx <- %08X: data_ack", ack.src_addr); }
};

}// namespace pika
