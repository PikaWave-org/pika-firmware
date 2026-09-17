#include "MicLevel.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace pika::audio {

/* Runs in the DMA interrupt: arithmetic only, no logging and no blocking. */
void MicLevel::take_audio_buffer(std::span<const int16_t> buf) {
    peak_ -= peak_ * DECAY_FACTOR * buf.size();
    for (int16_t s: buf) { peak_ = std::fmaxf(peak_, float(std::abs(s))); }
}

/* A magnitude of a signed 16 bit sample as dB, where full scale is 0dB. */
float MicLevel::to_db(uint32_t mag) {
    const float ratio = (float) mag / (float) std::numeric_limits<int16_t>::max();

    if (ratio <= FLOOR_MAG) {
        return FLOOR_DB;
    }

    return fminf(20.0f * log10f(ratio), 0.0f);
}

}// namespace pika::audio
