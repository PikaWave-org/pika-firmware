#pragma once

#include <pika/RingBuffer.h>

#include <array>
#include <cstdint>

namespace pika::app {

struct Message {
    uint32_t id{};
    uint32_t size{};
};

struct Peer {
    uint64_t id{};
    std::array<uint8_t, 32> key{};
    std::array<char, 64> name{};
    RingBuffer<Message, 32> rx_msgs{};
    RingBuffer<Message, 32> tx_msgs{};
};

}// namespace pika::app
