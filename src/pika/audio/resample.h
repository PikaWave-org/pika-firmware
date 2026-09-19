#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::audio {

/*
 * Between the board's 32kHz converters and the codec's 8kHz. One 256 tap
 * Kaiser prototype serves both directions; see resample.cpp for the design.
 *
 * The anti-aliasing is the point. There is no analog low pass in front of the
 * microphone, so this is the only thing between the 4-16kHz band and the
 * codec, and decimating without it folds that band onto 0-4kHz at full
 * amplitude - the 10kHz backlight PWM lands on 2kHz, and once folded it is
 * indistinguishable from speech.
 *
 * Float, following MicLevel and DACSpeaker, which already do per sample float
 * work in the interrupt. Group delay is 4.0ms each way: irrelevant to
 * record-and-replay, but it would matter to a duplex link.
 */

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
    /* Delay line and block contiguous, so a tap is a load and a multiply. */
    float work_[fir_taps - 1U + max_in]{};
};

/** @brief  8000 -> 32000, polyphase: four phases of 64 taps. */
class Interpolator4 {
public:
    /** @brief  Largest input block, i.e. one codec frame. */
    static constexpr size_t max_in = 180U;

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
