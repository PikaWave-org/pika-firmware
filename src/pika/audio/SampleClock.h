/*
 * The 32kHz sample clock: one timer's TRGO paces both converters, the DAC on
 * the way out and the ADC on the way in.
 *
 * There is one of these on the board rather than one per converter, because
 * the speaker and the microphone never run at the same time - the device
 * either talks or listens. That is a hardware-level decision, so this class
 * enforces it rather than trusting callers: start() claims the clock for an
 * owner and refuses anyone else until it is released.
 *
 * Usage:
 *
 *   static pika::audio::SampleClock sample_clock{&BOARD_SAMPLE_TIMER};
 *   static const pika::audio::DACSpeaker::Config spk_cfg = {
 *     &BOARD_SPK_DAC, &sample_clock, LINE_SPK_EN, BOARD_SPK_SETTLE_MS
 *   };
 *
 * The converters take it in their Config and never name a timer themselves,
 * which is what keeps them free of the board header.
 */

#pragma once

#include "hal.h"

#include <cstdint>

namespace pika::audio {

class SampleClock {
public:
    /** @brief  Samples per second, for both directions. */
    static constexpr uint32_t rate = 32000U;

    explicit SampleClock(GPTDriver *tim) : tim_{tim} {}

    /**
     * @brief   Starts the timer, stopped, ready for start() to run it.
     * @details Idempotent: both converters call it from their own init() and
     *          neither knows nor cares which got there first.
     * @return  false if the timer could not be started.
     */
    bool init();

    /**
     * @brief   Claims the clock for @p owner and starts the TRGO.
     * @param   owner  any stable pointer identifying the caller, normally
     *                 `this`. Only the same pointer can stop it again.
     * @return  false if another owner already holds the clock, which is a
     *          speaker and a microphone asked to run at once.
     */
    bool start(const void *owner);

    /** @brief  Stops the clock and releases it. A non-owner is ignored. */
    void stop(const void *owner);

    /**
     * @brief   I-class form of stop(), for use from a DMA callback.
     * @note    Must be called with the kernel locked.
     */
    void stop_i(const void *owner);

    /** @brief  True while some converter holds the clock. */
    [[nodiscard]] bool held() const { return owner_ != nullptr; }

private:
    /*
     * The timer counts at 4MHz and the TRGO comes from its update event, so
     * the sample rate is the counter rate divided by the interval below. 4MHz
     * is high enough that 32kHz divides it exactly with room for other rates,
     * and low enough that the prescaler fits.
     */
    static constexpr uint32_t frequency = 4000000U;
    static constexpr uint32_t interval = frequency / rate;

    static_assert(frequency % rate == 0U, "sample rate must divide the timer frequency exactly");
    static_assert(interval > 1U, "interval 0 and 1 produce no update events");
    static_assert(STM32_TIMCLK1 % frequency == 0U, "timer prescaler must be exact, or the sample rate drifts");
    static_assert((STM32_TIMCLK1 / frequency) - 1U <= 0xFFFFU, "prescaler does not fit the 16 bit register");

    /* MMS = 010, so the update event drives TRGO. No callback and no
       interrupt: nothing here runs in software. */
    static constexpr GPTConfig tim_cfg = {//
            .frequency = frequency,
            .callback = nullptr,
            .cr2 = TIM_CR2_MMS_1,
            .dier = 0U};

    GPTDriver *tim_;

    /* The claim. Written by the claiming thread and cleared from a DMA
       callback, hence volatile. */
    const void *volatile owner_ = nullptr;
};

}// namespace pika::audio
