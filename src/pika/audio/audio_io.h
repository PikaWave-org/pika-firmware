#pragma once

#include <cstdint>
#include <span>

namespace pika::audio {

class AudioSource;

class AudioSink {
public:
    virtual bool start_stream(AudioSource &src) = 0;
    virtual void stop_stream() = 0;

    /* No bit depth: sources deal in signed 16 bit at full scale, and what the
       converter underneath is has stopped being their business. */
    [[nodiscard]] virtual uint32_t sample_rate() const = 0;
};

class AudioSource {
public:
    /**
     * @brief   Writes the next samples of the stream, signed 16 bit at full
     *          scale. Scaling to the hardware and the volume are the sink's.
     * @details The buffer comes from the sink because only the sink knows how
     *          much it needs and when. It is valid for the call and no longer.
     * @return  samples written; short of buf.size(), including 0, ends the
     *          stream.
     */
    virtual size_t fill_audio_buffer(std::span<int16_t> buf) = 0;
};

}// namespace pika::audio
