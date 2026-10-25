#pragma once

#include <ch.hpp>
#undef HASH// HASH macro interferes with messgen HASH

#include "radio/Radio.h"
#include "radio/SX1262.h"


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
using Channel = uint8_t;

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
        own_channel_ = 1 + own_addr_ & 0xFF;
        discovery_header_duration_ = radio_->tx_duration(types::mac::heartbeat::FIXED_SIZE + 1);
        transfer_header_duration_ = discovery_header_duration_;
        set_state_idle();

        while (true) {
            if (tx_power_changed_) {
                // Cleared first, so a request landing meanwhile is applied next pass.
                tx_power_changed_ = false;
                radio_->set_power(tx_power_req_);
            }

            Time t = now();
            update_peers(t);

            switch (state_) {
                case State::idle:
                    update_idle(t);
                    break;
                case State::negotiation:
                    update_negotiation(t);
                    break;
                case State::transfer_master:
                    update_transfer_master(t);
                    break;
                case State::transfer_slave:
                    update_transfer_slave(t);
                    break;
                default:
                    break;
            }
            //
            // // TX data
            // // Use time slot before our listen window starts
            // // Peers are only added or replaced, never removed, so the size can be read unlocked.
            // for (size_t i = 0; i < peers_.size(); i++) {
            //     Peer &peer = peers_[i];
            //     // peer.is_synced is managed from this thread
            //     // peer.tx_buf may be added from another thread, but cleared only from this thread,
            //     // so it's safe to check if it is empty without mutex
            //     if (not peer.info.is_online or peer.tx_buf.empty()) {
            //         continue;
            //     }
            //
            //     t = now();
            //
            //     // Calculate nearest listen window of the peer
            //     auto peer_win = get_listen_window(peer.listen_window, t);
            //     if (t + TX_GAP < peer_win.first or peer_win.first > lsn_win.second) {
            //         // Too early to start or peer will listen after us, skip for now
            //         continue;
            //     }
            //
            //     // Peer is synchronized and has a data frame in tx buffer
            //     // LOG("mac tx -> " ADDR_FMT ": %u", ADDR_ARG(peer.addr), (unsigned) peer.tx_buf.size());
            //     types::mac::data_frame data_frame{
            //             .src_addr = addr_,
            //             .dst_addr = peer.addr,
            //             .payload = messgen::bytes(peer.tx_buf.data(), peer.tx_buf.size()),
            //     };
            //
            //     Time est_tx_duration = radio_->tx_duration(data_frame.serialized_size() + 1);
            //     // if (peer_win.first + est_tx_duration + TX_GAP >= lsn_win.first) {
            //     //     // No enough time to transmit the packet, try another peer
            //     //     continue;
            //     // }
            //
            //     tx_buf_[0] = proto::mac::data_frame::MESSAGE_ID;
            //     tx_buf_.resize(data_frame.serialize(&tx_buf_[1]) + 1);
            //
            //     // Unlocked, so senders are not blocked for the air time; the
            //     // peer stays busy until its frame is cleared below.
            //     transmit(std::max(peer_win.first, t + TX_GAP / 2));
            //     peer.tx_buf.clear();
            // }
            //
            // // RX
            // while (is_inside_window(t, lsn_win)) {
            //     Time rx_end_time;
            //     radio_state_ = RadioState::rx;
            //     chEvtBroadcastFlags(&events_, EVT_RX_START);
            //     std::span<const uint8_t> rx_buf = radio_->rx(rx_end_time, lsn_win.second);
            //     radio_state_ = RadioState::idle;
            //     chEvtBroadcastFlags(&events_, EVT_RX_END);
            //     if (rx_buf.size() > 0) {
            //         Time rx_time = rx_end_time - radio_->tx_duration(rx_buf.size());
            //         uint8_t frame_type = rx_buf[0];
            //         proto::mac::dispatch_message(
            //                 frame_type, messgen::bytes(&rx_buf[1], rx_buf.size() - 1), [&](const auto &recv_msg) {
            //                     using RecvMsgType = std::decay_t<decltype(recv_msg)>;
            //                     typename RecvMsgType::data_type actual_data;
            //                     auto res = recv_msg.deserialize(actual_data, messgen::empty_alloc, messgen::NoCopy{});
            //                     if (res >= 0) {
            //                         handle_frame(rx_time, actual_data);
            //                     }
            //                 });
            //     }
            //     t = now();
            // }

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
        own_addr_ = addr;
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
        chibios_rt::MutexLocker lock(peers_mtx_);
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

    Time retransmit_delay() const { return 1 * SECOND; }

private:
    static constexpr size_t PEERS_MAX = 8;
    // Sync every SYNC_INTERVAL ± SYNC_JITTER, so boards that collide once drift apart.
    static constexpr Time HEARTBEAT_INTERVAL = 2000 * MILLISECOND;
    static constexpr Time HEARTBEAT_JITTER = 1000 * MILLISECOND;
    static constexpr Time TRANSFER_REQUEST_INTERVAL = 200 * MILLISECOND;
    static constexpr Time TRANSFER_TIMEOUT = 10 * SECOND;
    static constexpr Time TRANSFER_INTERVAL = 500 * MILLISECOND;

    // Only syncs landing in our listen window are heard, about half of them,
    // so this spans enough of them that a peer in range does not flap.
    static constexpr Time PEER_TIMEOUT = 30 * SECOND;
    static constexpr Time TX_GAP = 2 * MILLISECOND;
    static constexpr uint64_t FREQ_BASE = 868100000;// [Hz] Channel 0 frequency
    static constexpr uint64_t FREQ_STEP = 200000;   // [Hz] Channel spacing

    struct Peer {
        Address addr{};
        Channel channel{};
        Time last_seen{};
        PeerInfo info{};
        FixedVector<uint8_t, 256> tx_buf{};
    };

    enum class State : uint8_t {
        idle = 1,           // Listening discovery channel, sendinding heartbeats
        negotiation = 2,    // Negotiation about dedicated channel
        transfer_master = 3,// Active data transfer, master
        transfer_slave = 4, // Active data transfer, slave
    };

    Address own_addr_ = 0;
    Channel own_channel_ = 1;
    Channel transfer_channel_ = 1;
    State state_ = State::idle;
    Peer *transfer_peer_ = nullptr;
    Time discovery_header_duration_{}; // Header duration on discovery channel
    Time transfer_header_duration_{};  // Header duration on discovery channel
    Time last_transfer_request_sent_{};// On discovery channel
    Time last_transfer_packet_recv_{}; //On dedicated channel
    Time last_transfer_packet_sent_{}; //On dedicated channel
    Time next_transfer_rx_time_{};
    RADIO *radio_ = nullptr;
    volatile float tx_power_req_ = 0.0f;
    volatile bool tx_power_changed_ = false;
    FrameHandler *handler_ = nullptr;
    FixedVector<Peer, PEERS_MAX> peers_;
    // Guards peers_: senders use it from other threads.
    chibios_rt::Mutex peers_mtx_;
    Time last_heartbeat_{};
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
    void update_peers(Time t) {
        FixedVector<Address, PEERS_MAX> expired;
        {
            chibios_rt::MutexLocker lock(peers_mtx_);
            for (auto &peer: peers_) {
                if (peer.info.is_online and t - peer.last_seen > PEER_TIMEOUT) {
                    peer.info.is_online = false;
                    expired.add(peer.addr);
                } else if (state_ == State::idle and not peer.tx_buf.empty()) {
                    set_state_negotiation(&peer, own_channel_);
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

    void set_channel(Channel channel) { radio_->set_freq(FREQ_BASE + channel * FREQ_STEP); }

    void set_state_idle() {
        if (state_ != State::idle) {
            LOG("mac: state IDLE");
            state_ = State::idle;
            set_channel(0);
        }
    }

    void set_state_negotiation(Peer *peer, Channel channel) {
        if (state_ != State::negotiation) {
            LOG("mac: state NEGOTIATION peer=" ADDR_FMT " channel=%u", ADDR_ARG(peer->addr), int(channel));
            state_ = State::negotiation;
            transfer_peer_ = peer;
            transfer_channel_ = channel;
        }
    }

    void set_state_transfer_master(Peer *peer, Channel channel) {
        if (state_ != State::negotiation) {
            LOG("mac: state TRANSFER_MASTER peer=" ADDR_FMT " channel=%u", ADDR_ARG(peer->addr), int(channel));
            state_ = State::transfer_master;
            transfer_peer_ = peer;
            transfer_channel_ = channel;
            set_channel(channel);
        }
    }

    void set_state_transfer_slave(Peer *peer, Channel channel) {
        if (state_ != State::negotiation) {
            LOG("mac: state TRANSFER_SLAVE peer=" ADDR_FMT " channel=%u", ADDR_ARG(peer->addr), int(channel));
            state_ = State::transfer_slave;
            transfer_peer_ = peer;
            transfer_channel_ = channel;
            set_channel(channel);
        }
    }

    Time get_heartbeat_time(const Time &t) {
        rng_ = rng_ * 1664525U + 1013904223U;// LCG
        return std::max((Time) t, last_heartbeat_ + HEARTBEAT_INTERVAL) + rng_ % HEARTBEAT_JITTER;
    }

    template<class HANDLER>
    bool receive(Time rx_deadline, HANDLER &&handler) {
        // Receive until planned transfer_request_time
        Time rx_end_time;
        radio_state_ = RadioState::rx;
        chEvtBroadcastFlags(&events_, EVT_RX_START);
        std::span<const uint8_t> rx_buf = radio_->rx(rx_end_time, rx_deadline);
        radio_state_ = RadioState::idle;
        chEvtBroadcastFlags(&events_, EVT_RX_END);
        if (not rx_buf.empty()) {
            uint8_t frame_type = rx_buf[0];
            proto::mac::dispatch_message(
                    frame_type, messgen::bytes(&rx_buf[1], rx_buf.size() - 1), [&](const auto &recv_msg) {
                        using RecvMsgType = std::decay_t<decltype(recv_msg)>;
                        typename RecvMsgType::data_type data;
                        auto res = recv_msg.deserialize(data, messgen::empty_alloc, messgen::NoCopy{});
                        if (res >= 0) {
                            if constexpr (requires { data.dst_addr; }) {
                                LOG("mac: rx " ADDR_FMT "<- " ADDR_FMT ": %s", ADDR_ARG(data.dst_addr), ADDR_ARG(data.src_addr), RecvMsgType::data_type::NAME);
                            } else {
                                LOG("mac: rx ALL <- " ADDR_FMT ": %s", ADDR_ARG(data.src_addr), RecvMsgType::data_type::NAME);
                            }
                            handler(data);
                        }
                    });
            return true;
        }
        return false;
    }

    void update_idle(Time t) {
        Time heartbeat_time = get_heartbeat_time(t);

        Time rx_timeout = heartbeat_time - t;
        if (rx_timeout > discovery_header_duration_) {
            // Receive until planned heartbeat_time
            LOG("mac: rx for %ims", int(rx_timeout / 1000));
            if (receive(heartbeat_time, [this](const auto &frame) { handle_discovery_frame(frame); })) {
                return;
            }
        }

        t = now();
        if (t + TX_GAP > heartbeat_time) {
            heartbeat_time = t + TX_GAP / 2;
            last_heartbeat_ = heartbeat_time;
        }

        // Send heartbeat packet on discovery channel
        tx_buf_[0] = proto::mac::heartbeat::MESSAGE_ID;
        types::mac::heartbeat hb{
                .src_addr = own_addr_,
                .channel = own_channel_,
        };
        tx_buf_.resize(1 + hb.serialize(&tx_buf_[1]));
        LOG("mac: tx -> ALL: heartbeat channel=%u", int(hb.channel));
        transmit(last_heartbeat_);
    }

    void update_negotiation(Time t) {
        if (transfer_peer_ == nullptr) {
            set_state_idle();
            return;
        }
        Time transfer_request_time = std::max(t, last_transfer_request_sent_ + TRANSFER_REQUEST_INTERVAL);

        Time rx_timeout = transfer_request_time - t;
        if (rx_timeout > discovery_header_duration_) {
            // Receive until planned transfer_request_time
            LOG("mac: rx for %ims", int(rx_timeout / 1000));
            Time rx_end_time;
            radio_state_ = RadioState::rx;
            chEvtBroadcastFlags(&events_, EVT_RX_START);
            std::span<const uint8_t> rx_buf = radio_->rx(rx_end_time, transfer_request_time);
            radio_state_ = RadioState::idle;
            chEvtBroadcastFlags(&events_, EVT_RX_END);
            if (not rx_buf.empty()) {
                uint8_t frame_type = rx_buf[0];
                proto::mac::dispatch_message(
                        frame_type, messgen::bytes(&rx_buf[1], rx_buf.size() - 1), [&](const auto &recv_msg) {
                            using RecvMsgType = std::decay_t<decltype(recv_msg)>;
                            typename RecvMsgType::data_type actual_data;
                            auto res = recv_msg.deserialize(actual_data, messgen::empty_alloc, messgen::NoCopy{});
                            if (res >= 0) {
                                handle_discovery_frame(actual_data);
                            }
                        });
                return;
            }
        }

        t = now();
        if (t + TX_GAP > transfer_request_time) {
            transfer_request_time = t + TX_GAP / 2;
            last_transfer_request_sent_ = transfer_request_time;
        }

        // Send heartbeat packet on discovery channel
        tx_buf_[0] = proto::mac::transfer_request::MESSAGE_ID;
        types::mac::transfer_request tr{
                .src_addr = own_addr_,
                .dst_addr = transfer_peer_->addr,
                .channel = own_channel_,
        };
        tx_buf_.resize(1 + tr.serialize(&tx_buf_[1]));
        LOG("mac: tx -> " ADDR_FMT ": transfer request channel=%u", ADDR_ARG(transfer_peer_->addr), int(tr.channel));
        transmit(last_transfer_request_sent_);
    }

    void update_transfer_master(Time t) {
        if (t > last_transfer_packet_recv_ + TRANSFER_TIMEOUT) {
            // Transfer timeout, return to IDLE
            LOG("mac: transfer_master timeout");
            set_state_idle();
            return;
        }

        Time transfer_tx_time = last_transfer_packet_sent_ + TRANSFER_INTERVAL;
        Time rx_timeout = transfer_tx_time - t;
        if (rx_timeout > transfer_header_duration_) {
            LOG("mac: rx for %ims", int(rx_timeout / 1000));
            if (receive(transfer_tx_time, [this](const auto &frame) { handle_transfer_frame(frame); })) {
                return;
            }
        }
    }

    void update_transfer_slave(Time t) {
        if (t > last_transfer_packet_recv_ + TRANSFER_TIMEOUT) {
            // Transfer timeout, return to IDLE
            LOG("mac: transfer_slave timeout");
            set_state_idle();
            return;
        }
    }

    void handle_discovery_frame(const types::mac::heartbeat &hb) {
        radio::RadioStats radio_stats = radio_->get_stats();
        LOG("mac: rx <- " ADDR_FMT ": heartbeat channel=%u", ADDR_ARG(hb.src_addr), int(hb.channel));

        Peer *peer;
        {
            chibios_rt::MutexLocker lock(peers_mtx_);
            peer = &get_peer(hb.src_addr);
        }
        peer->channel = hb.channel;
        peer->last_seen = now();
        peer->info.is_online = true;
        peer->info.radio_stats = radio_stats;

        if (handler_ != nullptr) {
            handler_->on_peer_updated(hb.src_addr, peer->info);
        }
    }

    void handle_discovery_frame(const types::mac::transfer_request &tr) {
        radio::RadioStats radio_stats = radio_->get_stats();
        LOG("mac: rx " ADDR_FMT " <- " ADDR_FMT ": transfer_request channel=%u", ADDR_ARG(tr.dst_addr),
            ADDR_ARG(tr.src_addr), int(tr.channel));

        // Update peer info for any transfer request, even for someone else
        Peer *peer;
        {
            chibios_rt::MutexLocker lock(peers_mtx_);
            peer = &get_peer(tr.src_addr);
        }
        peer->channel = tr.channel;
        peer->last_seen = now();
        peer->info.is_online = true;
        peer->info.radio_stats = radio_stats;

        if (handler_ != nullptr) {
            handler_->on_peer_updated(tr.src_addr, peer->info);
        }

        if (tr.dst_addr == own_addr_) {
            // Handle transfer request
            set_state_transfer_slave(peer, tr.channel);

            uint8_t next_rx = 50;
            last_transfer_packet_sent_ = now() * TX_GAP;
            tx_buf_[0] = proto::mac::transfer_ack::MESSAGE_ID;
            types::mac::transfer_ack cc{
                    .src_addr = own_addr_,
                    .dst_addr = transfer_peer_->addr,
            };
            tx_buf_.resize(1 + cc.serialize(&tx_buf_[1]));
            LOG("mac: tx -> " ADDR_FMT ": transfer_ack", ADDR_ARG(transfer_peer_->addr));
            transmit(last_transfer_packet_sent_);

            next_transfer_rx_time_ =
                    last_transfer_packet_sent_ + radio_->tx_duration(tx_buf_.size()) + next_rx * 10 * MILLISECOND;
        }
    }

    void handle_discovery_frame(const auto &data) {
        LOG("mac: WARNING: unexpected message type=%s on discovery channel", std::decay_t<decltype(data)>::NAME);
    }

    void handle_transfer_frame(const types::mac::transfer_data &data) {
        // radio::RadioStats radio_stats = radio_->get_stats();
        // LOG("mac: rx <- " ADDR_FMT ": data_frame", ADDR_ARG(data.src_addr));
        //
        // if (data.dst_addr == own_addr_ and transfer_peer_ and data.src_addr == transfer_peer_->addr) {
        //     transfer_peer_->last_seen = now();
        //     transfer_peer_->info.is_online = true;
        //     transfer_peer_->info.radio_stats = radio_stats;
        // }
        //
        // if (not transfer_peer_->tx_buf.empty()) {
        //     LOG("mac tx -> " ADDR_FMT ": transfer_data", ADDR_ARG(peer.addr), (unsigned) peer.tx_buf.size());
        //     types::mac::transfer_data data_frame{
        //             .src_addr = own_addr_,
        //             .dst_addr = transfer_peer_->addr,
        //             .payload = messgen::bytes(transfer_peer_->tx_buf.data(), transfer_peer_->tx_buf.size()),
        //     };
        //     tx_buf_[0] = proto::mac::transfer_ack::MESSAGE_ID;
        //     tx_buf_.resize(data_frame.serialize(&tx_buf_[1]) + 1);
        //
        //     // Unlocked, so senders are not blocked for the air time; the
        //     // peer stays busy until its frame is cleared below.
        //     transmit(now() + TX_GAP);
        //     transfer_peer_->tx_buf.clear();
        // } else {
        // }
    }

    void handle_transfer_frame(const types::mac::transfer_ack &ack) {
        // radio::RadioStats radio_stats = radio_->get_stats();
        // LOG("mac: rx <- " ADDR_FMT ": transfer_ack", ADDR_ARG(ack.src_addr));
        //
        // if (ack.dst_addr == own_addr_ and transfer_peer_ and ack.src_addr == transfer_peer_->addr) {
        //     transfer_peer_->last_seen = now();
        //     transfer_peer_->info.is_online = true;
        //     transfer_peer_->info.radio_stats = radio_stats;
        // }
        //
        // if (not transfer_peer_->tx_buf.empty()) {
        //     // LOG("mac tx -> " ADDR_FMT ": %u", ADDR_ARG(peer.addr), (unsigned) peer.tx_buf.size());
        //     types::mac::data_frame data_frame{
        //             .src_addr = own_addr_,
        //             .dst_addr = transfer_peer_->addr,
        //             .payload = messgen::bytes(transfer_peer_->tx_buf.data(), transfer_peer_->tx_buf.size()),
        //     };
        //
        //     tx_buf_[0] = proto::mac::data_frame::MESSAGE_ID;
        //     tx_buf_.resize(data_frame.serialize(&tx_buf_[1]) + 1);
        //
        //     // Unlocked, so senders are not blocked for the air time; the
        //     // peer stays busy until its frame is cleared below.
        //     transmit(now() + TX_GAP);
        //     transfer_peer_->tx_buf.clear();
        // }
    }

    void handle_transfer_frame(const auto &data) {
        LOG("mac: WARNING: unexpected message type=%s on transfer channel", std::decay_t<decltype(data)>::NAME);
    }

    // void handle_frame(Time &rx_time, const types::mac::sync &sync) {
    //     bool idle;
    //     radio::RadioStats radio_stats = radio_->get_stats();
    //     // LOG("mac rx <- " ADDR_FMT ": sync local_time=%u remote_time=%u", ADDR_ARG(sync.src_addr), uint32_t(rx_time),
    //     // uint32_t(sync.time));
    //     Peer *peer;
    //     {
    //         chibios_rt::MutexLocker lock(peers_mtx_);
    //         peer = &get_peer(sync.src_addr);
    //     }
    //     peer->listen_window.time = sync.listen_time + rx_time;
    //     peer->listen_window.interval = sync.listen_interval;
    //     peer->listen_window.duration = sync.listen_duration;
    //     peer->last_seen = now();
    //     peer->info.is_online = true;
    //     peer->info.radio_stats = radio_stats;
    //     idle = peer->tx_buf.empty();
    //
    //     Time t = now();
    //     Time win_diff =
    //             get_listen_window(peer->listen_window, t).first - get_listen_window(listen_window_, rx_time).second;
    //     // LOG("win_diff %i clock_diff=%i rssi=%.1fdBm", int32_t(win_diff), int32_t(rx_time - sync.time), peer.rssi);
    //     listen_window_.time += win_diff / 10;
    //     if (handler_ != nullptr) {
    //         handler_->on_peer_updated(sync.src_addr, peer->info);
    //         if (idle) {
    //             handler_->on_data_frame_sent(sync.src_addr, peer->info);
    //         }
    //     }
    // }
    //
    // void handle_frame(Time &rx_time, const types::mac::data_frame &data) {
    //     // LOG("mac rx <- " ADDR_FMT ": data_frame, size=%i", ADDR_ARG(data.src_addr), data.payload.size());
    //     if (data.dst_addr == addr_ and handler_ != nullptr) {
    //         Peer &peer = get_peer(data.src_addr);
    //         radio::RadioStats radio_stats = radio_->get_stats();
    //         peer.info.radio_stats = radio_stats;
    //         handler_->on_data_frame_received(data.src_addr, data.payload, peer.info);
    //     }
    // }
};

}// namespace pika
