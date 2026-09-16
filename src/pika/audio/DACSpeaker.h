/*
 * Driver for the speaker: DAC1_OUT1 into an SSM2305 class D amplifier.
 *
 * The wiring comes in as a Config, filled from the board's BOARD_SPK_* and
 * LINE_SPK_* definitions, so the driver depends on no board header.
 *
 * Samples go out at a fixed rate: the SampleClock's TRGO paces the DAC, and
 * DMA feeds it from a two-half circular buffer that the transfer callback
 * refills from an AudioSource a half at a time.
 *
 * Usage:
 *
 *   static pika::audio::SampleClock sample_clock{&BOARD_SAMPLE_TIMER};
 *   static const pika::audio::DACSpeaker::Config spk_cfg = {
 *     &BOARD_SPK_DAC, &sample_clock,
 *     LINE_SPK_EN, BOARD_SPK_SETTLE_MS
 *   };
 *   static pika::audio::DACSpeaker spk{spk_cfg};
 *   static pika::audio::ToneGenerator tone_gen;
 *   ...
 *   spk.init();
 *   tone_gen.tone(spk, 1000.0f, 0.1f, 0.5f);
 *   while (spk.busy()) chThdSleepMilliseconds(5);
 *
 * @note  The sample buffer is shared between all instances, because where it
 *        sits in memory is the driver's business and not a caller's. One
 *        instance is therefore the supported case - see the .cpp.
 */

#pragma once

#include "SampleClock.h"
#include "audio_io.h"

#include "hal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace pika::audio {

/* Samples per buffer half, i.e. 8ms of audio and a 125Hz refill callback. */
constexpr unsigned wave_buffer_half_len = 256U;

/**
 * @brief   Error flags reported by last_error().
 */
enum : uint32_t {
    err_dma = 1U << 0,      /**< DMA transfer error, the stream was freed.   */
    err_underrun = 1U << 1, /**< DAC underrun, the hardware stopped the DMA. */
    err_no_dma = 1U << 2,   /**< verify(): the DMA never moved anything.     */
    err_no_output = 1U << 3 /**< verify(): the converter output never moved. */
};

class DACSpeaker : public AudioSink {
public:
    /**
     * @brief   How the speaker is wired up, taken from the board header.
     */
    struct Config {
        DACDriver *dac{};     /**< DAC channel driving the amplifier input.      */
        SampleClock *clock{}; /**< Sample clock, shared with the microphone.     */
        ioline_t enable{};    /**< Amplifier enable pin.                         */
        uint32_t settle_ms{}; /**< Output settling before un-muting.             */
    };

    explicit DACSpeaker(const Config &cfg) { cfg_ = cfg; }

    bool start_stream(AudioSource &src) override;

    void stop_stream() override;

    /**
     * @brief   Starts the DAC and brings the output to its idle level.
     * @details Leaves the amplifier muted and the clock stopped. The output
     *          sits at mid scale from here on, so a later tone starts without
     *          waiting for the input capacitor again.
     * @return  false if the DAC could not be started.
     */
    bool init();

    /**
     * @brief   True until the converter actually stops, which is one buffer
     *          half after the source ran out. A finished source is therefore
     *          not yet a speaker that will take a new stream.
     */
    [[nodiscard]] bool busy() const { return source_ != nullptr; }

    /** @brief  Error flags since the last init(), 0 if none. */
    uint32_t last_error() const { return error_flags_; }

    /** @brief  Clears the error flags and, if the DMA died, re-arms it. */
    bool recover();

    [[nodiscard]] uint32_t sample_rate() const override { return SampleClock::rate; }

    /**
     * @brief   Playback volume, 0 to 1.0, clamped into range.
     * @details A linear factor on amplitude, not loudness: half is about
     *          -6dB. May be changed mid-stream, taking effect within 8ms.
     */
    void set_volume(float v) { volume_ = std::clamp(v, 0.0f, 1.0f); }

    /** @brief  The volume set_volume() last accepted. */
    [[nodiscard]] float volume() const { return volume_; }


private:
    static DACSpeaker *instance_;

    /* The DMA transfer and error callbacks, and the conversion group holding
     them. The group is a member so that its initializer may name them while
     they stay private.*/
    static constexpr sysinterval_t AMP_ON_DELAY = TIME_MS2I(20U);

    static constexpr int DAC_BIT_DEPTH = 12;
    static constexpr uint16_t DAC_VALUE_MID = 1 << (DAC_BIT_DEPTH - 1);
    static constexpr uint16_t DAC_VALUE_MAX = (1 << DAC_BIT_DEPTH) - 1;

    /* TSEL value selecting TIM6's TRGO as the DAC trigger; the low level
       driver shifts it into TSEL1 itself. The SampleClock owns the timer, but
       which trigger the DAC listens for is the DAC's own wiring. */
    static constexpr uint32_t TRG_TIM6_TRGO = 5U;

    /*
     * MODE1 = 000: normal mode, output buffer on, external pin only. The
     * buffer is what makes this a low impedance source able to charge the
     * amplifier's 100nF input capacitor.
     *
     * CR stays zero on purpose, DMAUDRIE1 in particular: an underrun raises
     * the interrupt shared with TIM6, for which this port compiles no handler
     * (STM32_TIM6_SUPPRESS_ISR). See check_underrun().
     */
    const DACConfig dac_cfg = {//
                               .init = DAC_VALUE_MID,
                               .datamode = DAC_DHRM_12BIT_RIGHT,
                               .cr = 0U,
                               .mcr = 0U};

    const DACConversionGroup dac_grp_ = {
            1U,                    /* num_channels                           */
            fill_cb,               /* end_cb                                 */
            error_cb,              /* error_cb                               */
            DAC_TRG(TRG_TIM6_TRGO) /* trigger                                */
    };

    static void fill_cb(DACDriver *dacp);
    static void error_cb(DACDriver *dacp, dacerror_t err);

    void amp(bool on);
    void check_underrun();

    /* I-class twin of stop_stream(), for use from the DMA callback. */
    void stop_stream_i();

    /* Rewrites signed full scale samples in place as converter codes. */
    void scale(std::span<dacsample_t> buf) const;

    Config cfg_{};

    /* Written by set_volume(), read in the DMA callback. */
    volatile float volume_ = 1.0f;
    /* The source doubles as the driver's state: non-null exactly while the
       converter and clock run, which is what busy() reports. Written from the
       DMA callback as well as the starting thread, hence volatile. */
    AudioSource *volatile source_ = nullptr;

    /* Set when the source runs out: the padded half is still to play, so the
       callback after it is the one that stops. */
    volatile bool draining_ = false;

    volatile uint32_t error_flags_ = 0U; /**< Also set from the callback.  */
    bool ready_ = false;                 /**< init() succeeded, DAC holds mid.       */
};

}// namespace pika::audio
