/*
 * Plays a block of signed 16 bit PCM sitting in flash, of the kind
 * tools/wav2cpp.py generates. That is already the form a source is asked for,
 * so the refill callback copies it out of .rodata and nothing else: no
 * decoding, no buffer of its own. Level is the sink's set_volume().
 *
 * Usage:
 *
 *   static pika::audio::PCMPlayer player;
 *   spk.set_volume(0.2f);
 *   player.play(spk, meow);
 *   while (spk.busy()) chThdSleepMilliseconds(5);
 *
 * The generated array converts to the span on its own, so no length can be
 * passed wrongly. The rate is not checked: the data is taken to be at the
 * sink's rate already, which is what wav2cpp.py's --rate is for.
 */

#pragma once

#include "audio_io.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::audio {

class PCMPlayer : public AudioSource {
public:
    /**
     * @brief   Starts playing a PCM block, already at the sink's sample rate.
     * @return  false if the sink is busy.
     */
    bool play(AudioSink &sink, std::span<const int16_t> samples) {
        samples_ = samples;
        pos_ = 0;
        return sink.start_stream(*this);
    }

    /* The position is a separate offset rather than the span being walked
       forward, so the clip is still there when the stream ends and the same
       player can play it again. */
    size_t fill_audio_buffer(std::span<int16_t> buf) override {
        size_t n = std::min(buf.size(), samples_.size() - pos_);

        std::copy_n(samples_.begin() + pos_, n, buf.begin());
        pos_ += n;
        return n;
    }

private:
    std::span<const int16_t> samples_;
    size_t pos_ = 0;
};

}// namespace pika::audio
