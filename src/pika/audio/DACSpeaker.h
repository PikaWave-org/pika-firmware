// Speaker: DAC fed by DMA from a circular two-half buffer, paced by the sample
// clock, into a class D amplifier. One instance only.

#pragma once

#include "SampleClock.h"
#include "audio_io.h"

#include "hal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace pika::audio {

// Samples per buffer half: 8ms.
constexpr unsigned wave_buffer_half_len = 256U;

enum : uint32_t { err_dma = 1U << 0, err_underrun = 1U << 1, err_no_dma = 1U << 2, err_no_output = 1U << 3 };

class DACSpeaker : public AudioSink {
public:
    struct Config {
        DACDriver *dac{};
        SampleClock *clock{};
        ioline_t enable{};   // Amplifier enable.
        uint32_t settle_ms{};// Output settling time after init.
    };

    explicit DACSpeaker(const Config &cfg) { cfg_ = cfg; }

    bool start_stream(AudioSource &src) override;

    void stop_stream() override;

    // Starts the DAC at mid scale, amplifier muted.
    bool init();

    // True while a stream plays, including its last buffered half.
    [[nodiscard]] bool busy() const { return source_ != nullptr; }

    // Blocks until the stream has ended; false on timeout or DMA error. One
    // waiting thread at a time.
    bool wait_idle(sysinterval_t timeout);

    uint32_t last_error() const { return error_flags_; }

    // Clears the error flags and stops any stream.
    bool recover();

    [[nodiscard]] uint32_t sample_rate() const override { return SampleClock::rate; }

    // Linear amplitude factor, 0 to 1.
    void set_volume(float v) { volume_ = std::clamp(v, 0.0f, 1.0f); }

    [[nodiscard]] float volume() const { return volume_; }

private:
    static DACSpeaker *instance_;

    static constexpr sysinterval_t AMP_ON_DELAY = TIME_MS2I(20U);

    static constexpr int DAC_BIT_DEPTH = 12;
    static constexpr uint16_t DAC_VALUE_MID = 1 << (DAC_BIT_DEPTH - 1);
    static constexpr uint16_t DAC_VALUE_MAX = (1 << DAC_BIT_DEPTH) - 1;

    // TSEL value for TIM6 TRGO.
    static constexpr uint32_t TRG_TIM6_TRGO = 5U;

    // Output buffer on. CR stays 0: the underrun interrupt is shared with
    // TIM6, which has no handler.
    const DACConfig dac_cfg = {//
                               .init = DAC_VALUE_MID,
                               .datamode = DAC_DHRM_12BIT_RIGHT,
                               .cr = 0U,
                               .mcr = 0U};

    const DACConversionGroup dac_grp_ = {1U, fill_cb, error_cb, DAC_TRG(TRG_TIM6_TRGO)};

    static void fill_cb(DACDriver *dacp);
    static void error_cb(DACDriver *dacp, dacerror_t err);

    void amp(bool on);
    void check_underrun();

    void stop_stream_i();

    // Signed samples to DAC codes, in place.
    void scale(std::span<dacsample_t> buf) const;

    Config cfg_{};

    volatile float volume_ = 1.0f;
    AudioSource *volatile source_ = nullptr;
    thread_reference_t idle_waiter_ = nullptr;
    // Callbacks left until the last half has played, 0 if not draining.
    volatile uint8_t drain_left_ = 0;
    volatile uint32_t error_flags_ = 0U;
    bool ready_ = false;
};

}// namespace pika::audio
