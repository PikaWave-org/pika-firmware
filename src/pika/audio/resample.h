#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::audio {

/*
 * Rate conversion between the board's 32kHz converters and the codec's 8kHz.
 *
 * Both converters are locked to SampleClock::rate, and MELPe is defined at
 * 8kHz, so something has to bridge the two. One prototype filter serves both
 * directions: a 256 tap linear phase FIR, Kaiser beta 6.6, -6dB at 3700Hz at
 * fs 32000, about 70dB of stopband. See resample.cpp for where the numbers
 * come from and the line that generated the table.
 *
 * The anti-aliasing is the whole point and not a detail. There is no analog
 * low pass in front of the microphone - the preamp goes straight into PA6 -
 * so this filter is the only thing between the 4-16kHz band and the codec.
 * Decimating without it folds that band onto 0-4kHz at full amplitude: 5kHz
 * lands on 3kHz, and the LCD backlight PWM at 10kHz lands on exactly 2kHz,
 * the middle of the voice band. It does not sound like hiss - it detunes
 * MELPe's pitch tracker and comes out as a metallic warble - and once folded
 * it is arithmetically indistinguishable from speech. The measured response
 * is -69dB at the 4kHz fold point and -107dB at 10kHz.
 *
 * Float rather than a Q format. The FPU is hard float fpv5-d16 and the ISR
 * already does per sample float work (MicLevel's fmaxf, DACSpeaker's volume
 * scale), so this follows the precedent and drops every saturation question.
 * Cost is about 0.8% of the CPU for both directions together.
 *
 * Group delay is 4.0ms each way. Irrelevant to record-and-replay; it would
 * matter to a duplex link, so it is written down here rather than discovered
 * there.
 *
 * Neither of these is DMA visible, so both live in plain .bss and none of the
 * .nocache reasoning that applies to the converter buffers applies here.
 */

/** @brief  Taps in the prototype filter. */
inline constexpr size_t fir_taps = 256U;

/** @brief  32000 -> 8000. */
class Decimator4 {
public:
    /** @brief  Largest input block, i.e. one microphone DMA half. */
    static constexpr size_t max_in = 256U;

    static_assert(max_in % 4U == 0U, "a block must be a whole number of outputs");

    void reset();

    /**
     * @brief   Filters and decimates by four.
     * @details in.size() must be a multiple of 4 and at most max_in; out must
     *          hold in.size() / 4.
     * @return  samples written.
     */
    size_t process(std::span<const int16_t> in, std::span<int16_t> out);

private:
    /* The delay line and the block, contiguous, so a tap is one load and one
       multiply with no index wrapping in the inner loop. */
    float work_[fir_taps - 1U + max_in]{};
};

/** @brief  8000 -> 32000, polyphase: four phases of 64 taps. */
class Interpolator4 {
public:
    /** @brief  Largest input block, i.e. one codec frame. */
    static constexpr size_t max_in = 180U;

    /** @brief  Taps per phase. */
    static constexpr size_t phase_taps = fir_taps / 4U;

    void reset();

    /**
     * @brief   Interpolates by four.
     * @details in.size() must be at most max_in; out must hold 4 * in.size().
     * @return  samples written.
     */
    size_t process(std::span<const int16_t> in, std::span<int16_t> out);

private:
    float work_[phase_taps - 1U + max_in]{};
};

}// namespace pika::audio
