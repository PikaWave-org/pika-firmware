#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include "log.h"

#include <stdarg.h>

namespace pika {
namespace {

/*
 * Serializes whole lines: the timestamp and the message go out under one
 * lock, so two threads logging at the same time cannot interleave.
 */
mutex_t lock;

bool started = false;

BaseSequentialStream *const stream =
    (BaseSequentialStream *)&BOARD_DBG_SERIAL;

/*
 * Milliseconds since chSysInit(). The tick counter is 32 bits
 * (CH_CFG_ST_RESOLUTION), so at CH_CFG_ST_FREQUENCY = 10kHz the timestamp
 * wraps back to zero after ~4.9 days of uptime.
 */
uint32_t uptime_ms() {

  return (uint32_t)chTimeI2MS((sysinterval_t)chVTGetSystemTimeX());
}

} /* anonymous namespace */

void log_init() {

  chMtxObjectInit(&lock);
  sdStart(&BOARD_DBG_SERIAL, nullptr);
  started = true;
}

void log_printf(const char *fmt, ...) {

  if (!started) {
    return;
  }

  uint32_t ms = uptime_ms();

  va_list ap;
  va_start(ap, fmt);

  chMtxLock(&lock);
  chprintf(stream, "[%6u.%03u] ", (unsigned)(ms / 1000U),
           (unsigned)(ms % 1000U));
  chvprintf(stream, fmt, ap);
  chMtxUnlock(&lock);

  va_end(ap);
}

} /* namespace pika */
