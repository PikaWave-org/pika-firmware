/*
 * Press events from a set of active low GPIO lines, which arrive as a Config
 * so that no board header is needed here.
 *
 * Debouncing is a lockout, not a resample: the first edge is believed at once
 * and the line ignored for the next debounce period. A press reports
 * immediately; the trade is that the level is read while it may still be
 * bouncing, and a wrong read costs one missed press until the next edge.
 *
 * Only presses are broadcast. Releases move the state so the next press is
 * seen, so ask pressed() about a held button.
 *
 * @note  Two lines on pads with the same number cannot both be interrupt
 *        driven: an EXTI channel is picked by pad number and maps to one port
 *        at a time. The Config resolves it by marking one polled, costing that
 *        one up to a poll period of latency. That path exists only for this
 *        collision and goes with the next pinout - do not build on it.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "ch.h"
#include "hal.h"

namespace pika::input {

/**
 * @brief   The buttons, in the order the Config lists them.
 * @note    The enumerator value is the event flag's bit position, so it is
 *          also the index into Config::lines.
 */
enum class Button : uint8_t { pwr, up, down, right, left, ptt, lsn, count };

class Buttons {
public:

    /** @brief  How many lines the driver tracks. */
    static constexpr size_t count = (size_t) Button::count;

    /** @brief  One button's wiring. */
    struct Entry {
        ioline_t line;  /**< Line the switch pulls low.                       */
        bool polled;    /**< true if no EXTI channel is available for it.     */
    };

    /** @brief  How the buttons are wired up, taken from the board header. */
    struct Config {
        Entry lines[count];         /**< Indexed by Button.                   */
        sysinterval_t debounce;     /**< Lockout after an accepted edge.      */
        sysinterval_t poll_period;  /**< Sampling period for polled lines.    */
    };

    explicit Buttons(const Config &cfg);

    /**
     * @brief   Starts watching the lines.
     * @note    Call once, from a thread, after chSysInit(). Every button is
     *          assumed up, so a polled line held across init reports a press
     *          on its first sample. Asserts if called twice or on a second
     *          instance.
     */
    void init();

    /** @brief  Event source carrying one flag per button, see flag(). */
    event_source_t &events() { return events_; }

    /** @brief  Event flag standing for a press of @p b. */
    static eventflags_t flag(Button b) { return (eventflags_t) 1U << (unsigned) b; }

    /** @brief  Current debounced level of @p b, true while it is held. */
    bool pressed(Button b) const;

    /** @brief  Human readable name of @p b, for logging. */
    static const char *name(Button b);

private:

    /* One button's state. An edge callback is handed the record for the line
       that raised it, so it reaches its own state without searching. */
    struct State {
        uint8_t index;       /**< Which Button this record stands for.      */
        systime_t changed;   /**< When it last changed, for the lockout.    */
        volatile bool down;  /**< Debounced level, true while pressed.      */
    };

    /* The callback's one void* carries the State, so the driver is reached
       through here instead. One driver per board; init() asserts on a second. */
    static Buttons *instance_;

    static void edge_cb(void *arg);
    static void poll_cb(virtual_timer_t *vtp, void *arg);

    void poll_i();
    void update_i(State &s);

    Config cfg_;
    event_source_t events_;
    virtual_timer_t poll_vt_;
    State state_[count];
};

}  // namespace pika::input
