#pragma once

#include <ch.h>

namespace pika {

using Time = int64_t;///< Time interval or time point in microseconds

static constexpr size_t MICROSECOND = 1;
static constexpr size_t MILLISECOND = 1000 * MICROSECOND;
static constexpr size_t SECOND = 1000 * MILLISECOND;

inline Time now() { return (int64_t) chVTGetSystemTime() * (1000000 / CH_CFG_ST_FREQUENCY); }

}// namespace pika