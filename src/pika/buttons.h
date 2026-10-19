// Press and release events from a set of active low GPIO lines, which arrive
// as a Config so that no board header is needed here.
//
// Debouncing waits for the line to go quiet: every edge restarts that button's
// DEBOUNCE wait, and its level is read once the wait passes with no further
// edge. A press reports DEBOUNCE after the contact settles; in exchange no
// bounce or spike, however long, can leave the state wrong. Measured on a PTT
// switch: up to 66ms of bounce on release and sub-100us spikes while held,
// which a lockout of the first edge read as presses and releases reversed.
//
// Both edges are broadcast, each button with a press and a release flag.
//
// @note  Two lines on pads with the same number cannot both be interrupt
//        driven: an EXTI channel is picked by pad number and maps to one port
//        at a time. The Config resolves it by marking one polled, costing that
//        one up to a poll period of latency. That path exists only for this
//        collision and goes with the next pinout - do not build on it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "ch.h"
#include "hal.h"

namespace pika::input {

/**
 * @brief   The buttons, in the order the Config lists them.
 * @note    The enumerator value is the press flag's bit position, so it is
 *          also the index into Config::lines.
 */
enum class Button : uint8_t { pwr, up, down, right, left, ptt, lsn, count };

class Buttons {
public:
    /** @brief  How many lines the driver tracks. */
    static constexpr size_t count = (size_t) Button::count;

    /** @brief  One button's wiring. */
    struct Entry {
        ioline_t line; /**< Line the switch pulls low.                       */
        bool polled;   /**< true if no EXTI channel is available for it.     */
    };

    /** @brief  How the buttons are wired up, taken from the board header. */
    struct Config {
        Entry lines[count]{};        /**< Indexed by Button.                   */
        eventid_t event_offset{};
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

    /** @brief  Event source carrying press_flag() and release_flag(). */
    event_source_t &events() { return events_; }

    /** @brief  Event flag standing for a press of @p b. */
    eventflags_t press_flag(Button b) { return (eventflags_t) 1U << (cfg_.event_offset + (eventid_t) b); }

    /** @brief  Event flag standing for a release of @p b. */
    eventflags_t release_flag(Button b) { return (eventflags_t) 1U << (cfg_.event_offset + count + (eventid_t) b); }

    /** @brief  Current debounced level of @p b, true while it is held. */
    bool pressed(Button b) const;

    /** @brief  Human readable name of @p b, for logging. */
    static const char *name(Button b);

private:
    static constexpr sysinterval_t DEBOUNCE = TIME_MS2I(20);
    static constexpr sysinterval_t POLL_PERIOD = TIME_MS2I(10);

    // One button's state. An edge callback is handed the record for the line
    // that raised it, so it reaches its own state without searching.
    struct State {
        Button button;     // Which Button this record stands for.
        systime_t edge;    // The latest edge; the level is read DEBOUNCE after it.
        bool pending;      // An edge since the level was last read.
        bool sampled_low;  // Polled lines only: the last sample, to see edges in.
        volatile bool down;// Debounced level, true while pressed.
    };

    // The callback's one void* carries the State, so the driver is reached
    // through here instead. One driver per board; init() asserts on a second.
    static Buttons *instance_;

    static void edge_cb(void *arg);
    static void poll_cb(virtual_timer_t *vtp, void *arg);
    static void settle_cb(virtual_timer_t *vtp, void *arg);

    void poll_i();
    void edge_i(State &s);
    void settle_i();

    Config cfg_;
    event_source_t events_;
    virtual_timer_t poll_vt_;
    // One shot, armed by an edge and re-armed while a line is still settling.
    virtual_timer_t settle_vt_;
    State state_[count];
};

}// namespace pika::input
