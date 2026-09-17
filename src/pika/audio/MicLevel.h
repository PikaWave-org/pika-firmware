/*
 * A simple level meter on the capture path.
 */

#pragma once

#include "audio_io.h"

#include <cstdint>
#include <span>

namespace pika::audio {

class MicLevel : public AudioConsumer {
public:
    MicLevel() {}

    void take_audio_buffer(std::span<const int16_t> buf) override;

    /** @brief  Peak with slow decay, in dB. */
    [[nodiscard]] float peak_db() const { return to_db(peak_); }

private:
    static constexpr float DECAY_FACTOR = 1e-4f;
    static constexpr float FLOOR_DB = -100.0f;
    static constexpr float FLOOR_MAG = 1e-5f;

    static float to_db(uint32_t mag);

    volatile float peak_ = 0;
};

}// namespace pika::audio
