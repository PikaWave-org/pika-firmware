#pragma once

#include <pika/MAC.h>
#include <pika/RingBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace pika::app {

static constexpr size_t PEERS_MAX_NUM = 64;

struct RxMessage {
    uint32_t id{};
    uint32_t size{};
};

struct TxMessage {
    enum : uint8_t {
        notified = (1 << 0),
        acked = (1 << 1),
    };

    uint32_t id{};
    uint32_t size{};
    uint8_t flags{};
};

struct Peer {
    Address addr{};
    std::array<uint8_t, 32> key{};
    std::array<char, 64> name{};
    RingBuffer<RxMessage, 32> rx_msgs{};
    RingBuffer<TxMessage, 32> tx_msgs{};
};

}// namespace pika::app
