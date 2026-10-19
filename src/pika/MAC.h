#pragma once

#include <ch.hpp>
#undef HASH// HASH macro interferes with messgen HASH

#include "radio/Radio.h"


#include <pika/FixedVector.h>
#include <pika/Time.h>
#include <pika/log.h>
#include <pika/proto/mac.h>

#include <hal.h>

#include <algorithm>
#include <cstdint>
#include <span>

namespace pika {

using Address = uint64_t;

// LOG an Address as 16 hex digits: LOG("peer " ADDR_FMT, ADDR_ARG(addr)).
// chprintf has no 64-bit conversions, so it goes as two 32-bit halves.
#define ADDR_FMT "%08lX%08lX"
#define ADDR_ARG(addr) (unsigned long) ((uint64_t) (addr) >> 32U), (unsigned long) ((uint64_t) (addr) & 0xFFFFFFFFU)
using MessageID = uint32_t;

struct PeerInfo {
    bool is_online{};
    radio::RadioStats radio_stats{};
};

// Called on the MAC thread, so implementations should only hand the work off.
class FrameHandler {
public:
    virtual void on_data_frame_sent(Address addr, const PeerInfo &peer_info) = 0;
    virtual void on_data_frame_received(Address addr, std::span<const uint8_t> frame, const PeerInfo &peer_info) = 0;
    virtual void on_peer_updated(Address addr, const PeerInfo &peer_info) = 0;
};

struct ListenWindow {
    Time time;
    Time interval;
    Time duration;
};

// What the radio is transmitting, see tx_state().
enum class RadioState : uint8_t { idle, rx, tx };

template<class RADIO>
class MAC : public chibios_rt::BaseStaticThread<1024> {
public:
    // Flags broadcast on events(). Flags raised before the listener reads them
    // arrive together, in no particular order.
    static constexpr eventflags_t EVT_TX_START = 1U << 0;
    static constexpr eventflags_t EVT_TX_END = 1U << 1;
    static constexpr eventflags_t EVT_RX_START = 1U << 2;
    static constexpr eventflags_t EVT_RX_END = 1U << 3;

    MAC(RADIO *radio) : radio_(radio) { chEvtObjectInit(&events_); }

    // Status changes, for listeners in other threads.
    event_source_t &events() { return events_; }

    RadioState tx_state() const { return radio_state_; }

    void main() override {
        listen_window_.time = listen_window_.duration * addr_;
        while (true) {
            if (tx_power_changed_) {
                // Cleared first, so a request landing meanwhile is applied next pass.
                tx_power_changed_ = false;
                radio_->set_power(tx_power_req_);
            }
            Time t = now();
            expire_peers(t);
            auto lsn_win = get_listen_window(listen_window_, t);

            // TX data
            // Use time slot before our listen window starts
            // Peers are only added or replaced, never removed, so the size can be read unlocked.
            for (size_t i = 0; i < peers_.size(); i++) {
                Peer &peer = peers_[i];
                // peer.is_synced is managed from this thread
                // peer.tx_buf may be added from another thread, but cleared only from this thread,
                // so it's safe to check if it is empty without mutex
                if (not peer.info.is_online or peer.tx_buf.empty()) {
                    continue;
                }

                t = now();

                // Calculate nearest listen window of the peer
                auto peer_win = get_listen_window(peer.listen_window, t);
                if (t + TX_GAP < peer_win.first or peer_win.first > lsn_win.second) {
                    // Too early to start or peer will listen after us, skip for now
                    continue;
                }

                // Peer is synchronized and has a data frame in tx buffer
                // LOG("mac tx -> " ADDR_FMT ": %u", ADDR_ARG(peer.addr), (unsigned) peer.tx_buf.size());
                types::mac::data_frame data_frame{
                        .src_addr = addr_,
                        .dst_addr = peer.addr,
                        .payload = messgen::bytes(peer.tx_buf.data(), peer.tx_buf.size()),
                };

                Time est_tx_duration = radio_->tx_duration(data_frame.serialized_size() + 1);
                // if (peer_win.first + est_tx_duration + TX_GAP >= lsn_win.first) {
                //     // No enough time to transmit the packet, try another peer
                //     continue;
                // }

                tx_buf_[0] = proto::mac::data_frame::MESSAGE_ID;
                tx_buf_.resize(data_frame.serialize(&tx_buf_[1]) + 1);

                // Unlocked, so senders are not blocked for the air time; the
                // peer stays busy until its frame is cleared below.
                transmit(std::max(peer_win.first, t + TX_GAP / 2));
                peer.tx_buf.clear();
            }

            // TX sync
            if (t + TX_GAP > next_sync_) {
                if (t + TX_GAP / 4 > next_sync_) {
                    next_sync_ = t + TX_GAP / 2;
                }
                // Broadcast sync packet
                tx_buf_[0] = proto::mac::sync::MESSAGE_ID;
                types::mac::sync sync_frame{
                        .src_addr = addr_,
                        .time = next_sync_,
                        .stratum = 1,
                        .listen_freq = radio_->freq() / 1000,
                        .listen_time = get_listen_window(listen_window_, next_sync_).first - next_sync_,
                        .listen_interval = listen_window_.interval,
                        .listen_duration = listen_window_.duration,
                };
                tx_buf_.resize(1 + sync_frame.serialize(&tx_buf_[1]));
                // LOG("mac tx -> sync");
                transmit(next_sync_);

                rng_ = rng_ * 1664525U + 1013904223U;// LCG
                next_sync_ = std::max(t, next_sync_) + SYNC_TX_INTERVAL - SYNC_JITTER + rng_ % (2 * SYNC_JITTER);
            }

            // RX
            while (is_inside_window(t, lsn_win)) {
                Time rx_end_time;
                radio_state_ = RadioState::rx;
                chEvtBroadcastFlags(&events_, EVT_RX_START);
                std::span<const uint8_t> rx_buf = radio_->rx(rx_end_time, lsn_win.second);
                radio_state_ = RadioState::idle;
                chEvtBroadcastFlags(&events_, EVT_RX_END);
                if (rx_buf.size() > 0) {
                    Time rx_time = rx_end_time - radio_->tx_duration(rx_buf.size());
                    uint8_t frame_type = rx_buf[0];
                    proto::mac::dispatch_message(
                            frame_type, messgen::bytes(&rx_buf[1], rx_buf.size() - 1), [&](const auto &recv_msg) {
                                using RecvMsgType = std::decay_t<decltype(recv_msg)>;
                                typename RecvMsgType::data_type actual_data;
                                auto res = recv_msg.deserialize(actual_data, messgen::empty_alloc, messgen::NoCopy{});
                                if (res >= 0) {
                                    handle_frame(rx_time, actual_data);
                                }
                            });
                }
                t = now();
            }

            chThdSleepMilliseconds(1);
        }
    }

    // The address also seeds the sync jitter, so boards do not jitter alike.
    // Applied by the MAC thread, which owns the radio, before its next transmission.
    void set_tx_power(float dbm) {
        tx_power_req_ = dbm;
        tx_power_changed_ = true;
    }

    float tx_power() const { return tx_power_changed_ ? tx_power_req_ : radio_->power(); }

    void set_address(Address addr) {
        addr_ = addr;
        rng_ = (uint32_t) addr;
    }

    void set_frame_handler(FrameHandler *handler) { handler_ = handler; }

    bool is_busy(Address dst_addr) {
        chibios_rt::MutexLocker lock(peers_mtx_);
        Peer &peer = get_peer(dst_addr);
        return peer.tx_buf.size() != 0;
    }

    template<class MSG>
    bool send_data_frame(Address dst_addr, MSG &&msg) {
        chibios_rt::MutexLocker lock(peers_mtx_);// TODO remove?
        Peer &peer = get_peer(dst_addr);
        if (not peer.tx_buf.empty()) {
            return false;
        }
        size_t sz = 1 + msg.serialized_size();
        if (peer.tx_buf.cap() >= sz) {
            using ActualType = std::decay_t<decltype(msg)>;
            peer.tx_buf[0] = ActualType::message_type::MESSAGE_ID;
            msg.serialize(peer.tx_buf.data() + 1);

            // Resize tx buffer _after_ serialization, it can be accessed concurrently from MAC thread
            peer.tx_buf.resize(sz);
            return true;
        }
        return false;
    }

    float get_min_snr() const { return radio_->get_min_snr(); }

    Time retransmit_delay() const { return listen_window_.interval; }

private:
    static constexpr size_t PEERS_MAX = 8;
    // Sync every SYNC_INTERVAL ± SYNC_JITTER, so boards that collide once drift apart.
    static constexpr Time SYNC_TX_INTERVAL = 2000 * MILLISECOND;
    static constexpr Time SYNC_JITTER = 500 * MILLISECOND;
    // Only syncs landing in our listen window are heard, about half of them,
    // so this spans enough of them that a peer in range does not flap.
    static constexpr Time PEER_TIMEOUT = 30 * SECOND;
    static constexpr Time TX_GAP = 2 * MILLISECOND;

    struct Peer {
        Address addr{};
        ListenWindow listen_window;
        Time last_seen{};
        PeerInfo info{};
        FixedVector<uint8_t, 256> tx_buf{};
    };

    Address addr_ = 0;
    ListenWindow listen_window_{
            .time = 0,
            .interval = 1000 * MILLISECOND,
            .duration = 500 * MILLISECOND,
    };
    RADIO *radio_ = nullptr;
    volatile float tx_power_req_ = 0.0f;
    volatile bool tx_power_changed_ = false;
    FrameHandler *handler_ = nullptr;
    FixedVector<Peer, PEERS_MAX> peers_;
    // Guards peers_: senders use it from other threads.
    chibios_rt::Mutex peers_mtx_;
    Time next_sync_{};
    uint32_t rng_{};

    FixedVector<uint8_t, 256> tx_buf_;

    event_source_t events_;
    volatile RadioState radio_state_ = RadioState::idle;

    // Sends tx_buf_, broadcasting its start and end.
    void transmit(const Time &send_at) {
        radio_state_ = RadioState::tx;
        chEvtBroadcastFlags(&events_, EVT_TX_START);
        Time t0 = now();
        radio_->tx(tx_buf_, send_at);
        Time t1 = now();
        LOG("tx time %i bytes %ims", tx_buf_.size(), int((t1 - t0) / 1000));
        radio_state_ = RadioState::idle;
        chEvtBroadcastFlags(&events_, EVT_TX_END);
    }

    // Peers not heard from for PEER_TIMEOUT go offline until their next sync.
    void expire_peers(Time t) {
        FixedVector<Address, PEERS_MAX> expired;
        {
            chibios_rt::MutexLocker lock(peers_mtx_);
            for (auto &peer: peers_) {
                if (peer.info.is_online and t - peer.last_seen > PEER_TIMEOUT) {
                    peer.info.is_online = false;
                    expired.add(peer.addr);
                }
            }
        }
        for (Address addr: expired) {
            // LOG("mac peer " ADDR_FMT " offline", ADDR_ARG(addr));
            if (handler_ != nullptr) {
                PeerInfo peer_info{};
                handler_->on_peer_updated(addr, peer_info);
            }
        }
    }

    Peer *find_peer(Address addr) {
        for (auto &peer: peers_) {
            if (peer.addr == addr) {
                return &peer;
            }
        }
        return nullptr;
    }

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
        int64_t n = (t - win.time) / win.interval;
        Time s = win.time + n * win.interval;
        if (s + win.duration > t) {
            // now is inside current listen window
            return {s, s + win.duration};
        }
        // this listen window ended, return the next one
        s += win.interval;
        return {s, s + win.duration};
    }

    // Logs data in hex, 32 bytes a line, each line prefixed with dir and its offset.
    static void log_hex(const char *dir, std::span<const uint8_t> data) {
        constexpr size_t PER_LINE = 32;
        for (size_t off = 0; off < data.size(); off += PER_LINE) {
            char line[PER_LINE * 3];
            char *p = line;
            for (size_t i = off; i < std::min(off + PER_LINE, data.size()); i++) {
                *p++ = "0123456789ABCDEF"[data[i] >> 4];
                *p++ = "0123456789ABCDEF"[data[i] & 0xFU];
                *p++ = ' ';
            }
            p[-1] = '\0';
            LOG("%s %03u: %s", dir, (unsigned) off, line);
        }
    }

    static bool is_inside_window(Time &t, const std::pair<Time, Time> &win) {
        return t >= win.first and t < win.second;
    }

    void handle_frame(Time &rx_time, const types::mac::sync &sync) {
        bool idle;
        radio::RadioStats radio_stats = radio_->get_stats();
        // LOG("mac rx <- " ADDR_FMT ": sync local_time=%u remote_time=%u", ADDR_ARG(sync.src_addr), uint32_t(rx_time),
        // uint32_t(sync.time));
        Peer *peer;
        {
            chibios_rt::MutexLocker lock(peers_mtx_);
            peer = &get_peer(sync.src_addr);
        }
        peer->listen_window.time = sync.listen_time + rx_time;
        peer->listen_window.interval = sync.listen_interval;
        peer->listen_window.duration = sync.listen_duration;
        peer->last_seen = now();
        peer->info.is_online = true;
        peer->info.radio_stats = radio_stats;
        idle = peer->tx_buf.empty();

        Time t = now();
        Time win_diff =
                get_listen_window(peer->listen_window, t).first - get_listen_window(listen_window_, rx_time).second;
        // LOG("win_diff %i clock_diff=%i rssi=%.1fdBm", int32_t(win_diff), int32_t(rx_time - sync.time), peer.rssi);
        listen_window_.time += win_diff / 10;
        if (handler_ != nullptr) {
            handler_->on_peer_updated(sync.src_addr, peer->info);
            if (idle) {
                handler_->on_data_frame_sent(sync.src_addr, peer->info);
            }
        }
    }

    void handle_frame(Time &rx_time, const types::mac::data_frame &data) {
        // LOG("mac rx <- " ADDR_FMT ": data_frame, size=%i", ADDR_ARG(data.src_addr), data.payload.size());
        if (data.dst_addr == addr_ and handler_ != nullptr) {
            Peer &peer = get_peer(data.src_addr);
            radio::RadioStats radio_stats = radio_->get_stats();
            peer.info.radio_stats = radio_stats;
            handler_->on_data_frame_received(data.src_addr, data.payload, peer.info);
        }
    }
};

}// namespace pika
