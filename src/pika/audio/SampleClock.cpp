#include "SampleClock.h"

namespace pika::audio {

/*
 * gptStart() asserts against being called on an already started driver, so the
 * state is checked rather than the call repeated: whichever converter inits
 * second finds the timer ready and has nothing to do.
 */
bool SampleClock::init() {
    if (tim_->state != GPT_STOP) {
        return true;
    }

    return gptStart(tim_, &tim_cfg) == HAL_RET_SUCCESS;
}

/*
 * The claim is taken under the system lock so that two threads cannot both see
 * a free clock. Everything after it is this owner's alone.
 */
bool SampleClock::start(const void *owner) {
    osalSysLock();

    if (owner_ != nullptr and owner_ != owner) {
        osalSysUnlock();
        return false;// someone else is using the converter pair
    }

    owner_ = owner;
    osalSysUnlock();

    gptStartContinuous(tim_, interval);
    return true;
}

void SampleClock::stop(const void *owner) {
    osalSysLock();
    stop_i(owner);
    osalSysUnlock();
}

/*
 * A non-owner is ignored rather than asserted on: a converter's error path may
 * run after the clock has already been handed over, and stopping the other
 * one's clock would be far worse than doing nothing.
 */
void SampleClock::stop_i(const void *owner) {
    if (owner_ != owner) {
        return;
    }

    if (tim_->state == GPT_CONTINUOUS) {
        gptStopTimerI(tim_);
    }

    owner_ = nullptr;
}

}// namespace pika::audio
