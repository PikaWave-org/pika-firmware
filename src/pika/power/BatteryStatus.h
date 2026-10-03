#pragma once

#include <cstdint>

namespace pika {

enum class Charge : uint8_t { off = 0U, precharge = 1U, fast = 2U, done = 3U };

struct BatteryStatus {
    bool input_connected;
    Charge charging;
    float voltage{};
    float charge_current{};
};

}
