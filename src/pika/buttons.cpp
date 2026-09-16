#include "ch.h"
#include "hal.h"

#include "buttons.h"

namespace pika::input {
namespace {

const char *const button_names[] = {"PWR", "UP", "DOWN", "RIGHT", "LEFT", "PTT", "LSN"};

static_assert(sizeof(button_names) / sizeof(button_names[0]) == Buttons::count,
              "button_names must cover every Button");

}  // namespace

Buttons *Buttons::instance_ = nullptr;

Buttons::Buttons(const Config &cfg) : cfg_(cfg) {
    /* Runs before halInit(), so nothing here may touch the hardware. */
    chEvtObjectInit(&events_);
    chVTObjectInit(&poll_vt_);

    for (size_t i = 0; i < count; i++) {
        state_[i].index = (uint8_t) i;
        state_[i].changed = (systime_t) 0;
        state_[i].down = false;
    }
}

/* The resting state is not sampled; every button is assumed up. */
void Buttons::init() {
    chDbgAssert(instance_ == nullptr, "buttons: already initialised");

    instance_ = this;

    bool any_polled = false;

    for (size_t i = 0; i < count; i++) {
        if (cfg_.lines[i].polled) {
            any_polled = true;
            continue;
        }
        /* The callback has to be in place before the channel can fire. */
        palSetLineCallback(cfg_.lines[i].line, edge_cb, &state_[i]);
        palEnableLineEvent(cfg_.lines[i].line, PAL_EVENT_MODE_BOTH_EDGES);
    }

    /* No timer once every line has an EXTI channel of its own. */
    if (any_polled) {
        chSysLock();
        chVTSetContinuousI(&poll_vt_, cfg_.poll_period, poll_cb, this);
        chSysUnlock();
    }
}

bool Buttons::pressed(Button b) const { return state_[(unsigned) b].down; }

const char *Buttons::name(Button b) {
    const unsigned i = (unsigned) b;
    return i < count ? button_names[i] : "?";
}

/* The lockout is the whole of the debouncing; outside it the line is read and
   believed. */
void Buttons::update_i(State &s) {
    if (chVTTimeElapsedSinceX(s.changed) < cfg_.debounce) { return; }

    const bool down = palReadLine(cfg_.lines[s.index].line) == PAL_LOW;

    if (down == s.down) { return; }

    s.changed = chVTGetSystemTimeX();
    s.down = down;

    if (down) { chEvtBroadcastFlagsI(&events_, flag((Button) s.index)); }
}

/* The lines with no EXTI channel of their own. Same lockout as the rest; the
   poll period is the only difference and it shows up as latency. */
void Buttons::poll_i() {
    for (uint8_t i = 0; i < count; i++) {
        if (cfg_.lines[i].polled) { update_i(state_[i]); }
    }
}

/* Both entry points arrive with the kernel unlocked. */
void Buttons::edge_cb(void *arg) {
    chSysLockFromISR();
    instance_->update_i(*(State *) arg);
    chSysUnlockFromISR();
}

void Buttons::poll_cb(virtual_timer_t *vtp, void *arg) {
    (void) vtp;

    chSysLockFromISR();
    ((Buttons *) arg)->poll_i();
    chSysUnlockFromISR();
}

} /* namespace pika::input */
