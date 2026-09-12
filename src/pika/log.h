/*
 * Debug logging over the board's console UART (BOARD_DBG_SERIAL, 115200 8N1
 * by default), for bring-up and monitoring.
 *
 * Every line is stamped with the time since boot and terminated with CRLF:
 *
 *   [     0.000] pika_go_0 starting
 *   [     1.234] lcd: flush took 47ms
 *
 * Usage: call pika::log_init() once after chSysInit(), then LOG() from any
 * thread.
 *
 *   LOG("lcd: init failed, i2c error 0x%08x", pika::lcd::last_error());
 *
 * Formatting is chprintf()'s, which is a subset of printf(): no %f unless
 * CHPRINTF_USE_FLOAT is enabled, no length modifiers beyond l/L.
 */

#ifndef PIKA_LOG_H
#define PIKA_LOG_H

namespace pika {

/**
 * @brief   Starts the debug serial driver.
 * @note    Call once from main(), after chSysInit(). Lines logged before
 *          this are dropped.
 */
void log_init(void);

/**
 * @brief   Writes one timestamped, chprintf() formatted message.
 * @note    Thread context only: it takes a mutex and blocks on the serial
 *          output queue, so it must not be called from an ISR or with the
 *          kernel locked.
 * @note    Prefer the LOG() macro, which appends the line terminator.
 */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} /* namespace pika */

/**
 * @brief   Logs one line, no trailing "\r\n" needed in the format string.
 */
#define LOG(fmt, ...)                                                       \
  pika::log_printf(fmt "\r\n" __VA_OPT__(, ) __VA_ARGS__)

#endif /* PIKA_LOG_H */
