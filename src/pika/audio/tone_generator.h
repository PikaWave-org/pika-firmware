/*
 * A sine of any frequency, from a phase accumulator, so no per-frequency
 * table is needed. Samples are signed full scale: the converter's width and
 * the volume belong to the sink.
 */

#pragma once

#include "audio_io.h"

#include <algorithm>
#include <cmath>

namespace pika::audio {

class ToneGenerator : public AudioSource {
public:
    void tone(AudioSink &sink, float freq, float gain, float duration) {
        phase_inc_ = 2.0f * M_PI * freq / sink.sample_rate();
        phase_ = 0.0f;
        gain_ = gain;
        remaining_ = duration * sink.sample_rate();
        active_ = true;
        sink.start_stream(*this);
    }

    size_t fill_audio_buffer(std::span<int16_t> buf) override {
        size_t n = std::min(buf.size(), remaining_);

        if (n == 0) {
            active_ = false;
            return 0;
        }

        for (size_t i = 0; i < n; i++) {
            buf[i] = int16_t(sinf(phase_) * gain_ * 32767.0f);
            phase_ += phase_inc_;
            if (phase_ >= 2 * M_PI) {
                phase_ -= 2 * M_PI;
            }
        }

        remaining_ -= n;
        return n;
    }

    bool is_active_() { return active_; }

private:
    bool active_ = false;
    float phase_inc_ = 0;
    float phase_ = 0;
    float gain_ = 0;
    size_t remaining_ = 0;
};

}// namespace pika::audio
