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

/*
 * The capture-side mirror of AudioSource: where a source is pulled from by
 * whatever plays it, a consumer is pushed to by whatever records it.
 *
 * The direction is not a style choice. Each converter is paced by its own
 * hardware clock, so the clocked side drives and the other side is passive:
 * playback pulls from an AudioSource, capture pushes to an AudioConsumer. A
 * microphone made to look like an AudioSource would have to buffer and
 * reconcile two clocks for no gain, since nothing here plays and records at
 * once.
 *
 * There is deliberately no return value. A source ends a stream by returning
 * short, but a recording has no natural end - the device decides when to stop,
 * because a button was released.
 */
class AudioConsumer {
public:
    /**
     * @brief   Takes the next captured samples, signed 16 bit at full scale.
     *          Undoing the converter's offset and range is the device's.
     * @details Called from the capture device's DMA interrupt, one buffer half
     *          at a time, so it must not block or log. The buffer is valid for
     *          the call and no longer.
     */
    virtual void take_audio_buffer(std::span<const int16_t> buf) = 0;
};

}// namespace pika::audio
