#pragma once

#include <pika/MAC.h>
#include <pika/RingBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <pika/types/app/message_type.h>

namespace pika::app {

static constexpr size_t PEERS_MAX_NUM = 64;
// Messages kept per peer each way, the oldest dropped for a new one.
static constexpr size_t MSGS_MAX_NUM = 32;

struct RxMessage {
    Time created{};// When it was announced.
    Time last_req_time{};
    uint32_t id{};
    uint32_t size{};
    uint32_t size_received{};
    types::app::message_type msg_type{};

    // Still arriving.
    bool pending() const { return size_received < size; }
    // All here, and something to play.
    bool complete() const { return size > 0 and size_received >= size; }
};

struct TxMessage {
    enum : uint8_t {
        notified = (1 << 0),
        acked = (1 << 1),
    };

    Time created{};// When it was recorded.
    Time last_req_time{};
    uint32_t id{};
    uint32_t size{};
    uint32_t size_sent{};// The furthest the peer has asked for, so what it has.
    uint8_t flags{};

    // The peer has it all.
    bool delivered() const { return (flags & acked) != 0; }
};

// This board's own entry, from /id.txt.
struct Identity {
    Address addr{};
    std::array<uint8_t, 32> key{};
    std::array<char, 64> name{};
};

struct Peer {
    Address addr{};
    std::array<uint8_t, 32> key{};
    std::array<char, 64> name{};
    RingBuffer<RxMessage, MSGS_MAX_NUM> rx_msgs{};
    RingBuffer<TxMessage, MSGS_MAX_NUM> tx_msgs{};
    PeerInfo info{};
};

}// namespace pika::app
