#pragma once

#include <pika/MAC.h>
#include <pika/RingBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <pika/types/app/message_type.h>

namespace pika::app {

static constexpr size_t PEERS_MAX_NUM = 64;

struct RxMessage {
    Time last_req_time{};
    uint32_t id{};
    uint32_t size{};
    uint32_t size_received{};
    types::app::message_type msg_type{};
};

struct TxMessage {
    enum : uint8_t {
        notified = (1 << 0),
        acked = (1 << 1),
    };

    Time last_req_time{};
    uint32_t id{};
    uint32_t size{};
    uint8_t flags{};
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
    RingBuffer<RxMessage, 32> rx_msgs{};
    RingBuffer<TxMessage, 32> tx_msgs{};
    float rssi{};
};

}// namespace pika::app
