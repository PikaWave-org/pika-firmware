#include "ch.h"
#include "hal.h"

#include "buttons.h"

#include <algorithm>

namespace pika::input {
namespace {

const char *const button_names[] = {"PWR", "UP", "DOWN", "RIGHT", "LEFT", "PTT", "LSN"};

static_assert(sizeof(button_names) / sizeof(button_names[0]) == Buttons::count, "button_names must cover every Button");

}// namespace

Buttons *Buttons::instance_ = nullptr;

Buttons::Buttons(const Config &cfg) : cfg_(cfg) {
    chEvtObjectInit(&events_);
    chVTObjectInit(&poll_vt_);
    chVTObjectInit(&settle_vt_);

    for (size_t i = 0; i < count; i++) {
        state_[i].button = (Button) i;
        state_[i].edge = (systime_t) 0;
        state_[i].pending = false;
        state_[i].sampled_low = false;
        state_[i].down = false;
    }
}

// The resting state is not sampled; every button is assumed up.
void Buttons::init() {
    chDbgAssert(instance_ == nullptr, "buttons: already initialised");

    instance_ = this;

    bool any_polled = false;

    for (size_t i = 0; i < count; i++) {
        if (cfg_.lines[i].polled) {
            any_polled = true;
            continue;
        }
        // The callback has to be in place before the channel can fire.
        palSetLineCallback(cfg_.lines[i].line, edge_cb, &state_[i]);
        palEnableLineEvent(cfg_.lines[i].line, PAL_EVENT_MODE_BOTH_EDGES);
    }

    // No timer once every line has an EXTI channel of its own.
    if (any_polled) {
        chSysLock();
        chVTSetContinuousI(&poll_vt_, POLL_PERIOD, poll_cb, this);
        chSysUnlock();
    }
}

bool Buttons::pressed(Button b) const { return state_[(unsigned) b].down; }

const char *Buttons::name(Button b) {
    const unsigned i = (unsigned) b;
    return i < count ? button_names[i] : "?";
}

void Buttons::edge_i(State &s) {
    s.edge = chVTGetSystemTimeX();
    s.pending = true;
    if (not chVTIsArmedI(&settle_vt_)) {
        chVTSetI(&settle_vt_, DEBOUNCE, settle_cb, this);
    }
}

// Reads every line quiet for DEBOUNCE, and re-arms for the earliest still
// settling.
void Buttons::settle_i() {
    sysinterval_t next = DEBOUNCE + 1U;

    for (State &s: state_) {
        if (not s.pending) {
            continue;
        }
        const sysinterval_t quiet = chVTTimeElapsedSinceX(s.edge);
        if (quiet < DEBOUNCE) {
            next = std::min(next, (sysinterval_t) (DEBOUNCE - quiet));
            continue;
        }
        s.pending = false;
        const bool down = palReadLine(cfg_.lines[(size_t) s.button].line) == PAL_LOW;
        if (down != s.down) {
            s.down = down;
            chEvtBroadcastFlagsI(&events_, down ? press_flag(s.button) : release_flag(s.button));
        }
    }
    if (next <= DEBOUNCE) {
        chVTSetI(&settle_vt_, next, settle_cb, this);
    }
}

// A polled line's edges are the changes between two samples.
void Buttons::poll_i() {
    for (State &s: state_) {
        if (not cfg_.lines[(size_t) s.button].polled) {
            continue;
        }
        const bool low = palReadLine(cfg_.lines[(size_t) s.button].line) == PAL_LOW;
        if (low != s.sampled_low) {
            s.sampled_low = low;
            edge_i(s);
        }
    }
}

// The entry points arrive with the kernel unlocked.
void Buttons::edge_cb(void *arg) {
    chSysLockFromISR();
    instance_->edge_i(*(State *) arg);
    chSysUnlockFromISR();
}

void Buttons::poll_cb(virtual_timer_t *vtp, void *arg) {
    (void) vtp;

    chSysLockFromISR();
    ((Buttons *) arg)->poll_i();
    chSysUnlockFromISR();
}

void Buttons::settle_cb(virtual_timer_t *vtp, void *arg) {
    (void) vtp;

    chSysLockFromISR();
    ((Buttons *) arg)->settle_i();
    chSysUnlockFromISR();
}

}// namespace pika::input
