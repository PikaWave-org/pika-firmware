#pragma once

#include <pika/RingBuffer.h>
#include <pika/audio/audio_io.h>
#include <pika/audio/resample.h>

#include <ch.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::audio {

// Receives the encoded stream, one frame at a time, on the codec thread.
class MELPeSink {
public:
    virtual void on_melpe_frame(std::span<const uint8_t> frame) = 0;

protected:
    ~MELPeSink() = default;
};

// Supplies a stream to replay, one frame at a time, on the codec thread;
// false once there are no more frames.
class MELPeSource {
public:
    virtual bool fill_melpe_frame(std::span<uint8_t> frame) = 0;

protected:
    ~MELPeSource() = default;
};

// Encodes the microphone into a MELPe 2400 stream for a MELPeSink, and plays
// one from a MELPeSource. The interrupts only resample and queue; the codec,
// which is not reentrant, runs on this object's thread. Shared state is
// single-writer.
class MELPeRecorder : public chibios_rt::BaseStaticThread<8192>, public AudioConsumer, public AudioSource {
public:
    // One 22.5ms frame: 180 samples at 8kHz, 54 bits packed into 7 bytes.
    static constexpr size_t frame_len = 180U;
    static constexpr size_t frame_bytes = 7U;

    chibios_rt::ThreadReference start(tprio_t prio) override;

    // Requests, carried out by the codec thread.
    void start_record(MELPeSink &sink);
    void stop_record();
    void replay(AudioSink &sink, MELPeSource &source);

    // True once everything queued is encoded or played.
    [[nodiscard]] bool idle() const { return mode_ == mode_idle; }

    // Blocks until idle(); false on timeout. One waiting thread at a time.
    bool wait_idle(sysinterval_t timeout);

    [[nodiscard]] uint32_t frames() const { return frames_; }

    [[nodiscard]] uint32_t overruns() const { return overruns_; }

    [[nodiscard]] uint32_t underruns() const { return underruns_; }

    // Recording length in tenths of a second.
    [[nodiscard]] uint32_t tenths() const { return frames_ * 225U / 1000U; }

    [[nodiscard]] uint32_t bytes() const { return frames_ * frame_bytes; }

    // Runs in the ADC interrupt.
    void on_audio_buffer(std::span<const int16_t> buf) override;

    // Runs in the DAC interrupt.
    size_t fill_audio_buffer(std::span<int16_t> buf) override;

private:
    // Wakes the codec thread, which then checks everything.
    static constexpr eventmask_t evt_wake = EVENT_MASK(0);

    // 360ms of 8kHz PCM either way. A whole number of frames, so a codec
    // frame is never split by the wrap.
    static constexpr size_t ring_len = 16U * frame_len;

    enum : uint8_t { mode_idle, mode_record, mode_drain, mode_replay };

    enum : uint8_t { req_none, req_record, req_stop, req_replay };

    void main() override;
    void encode_queued();
    void decode_ahead();
    void set_idle();

    chibios_rt::ThreadReference thread_;

    volatile uint8_t req_{req_none};
    volatile uint8_t mode_{mode_idle};

    thread_reference_t idle_waiter_ = nullptr;

    MELPeSink *melpe_sink_{};
    MELPeSource *melpe_source_{};
    AudioSink *sink_{};
    volatile bool drain_{};
    volatile uint32_t frames_{};
    volatile uint32_t overruns_{};
    volatile uint32_t underruns_{};

    // PCM at 8kHz between the speaker/microphone and the codec, in either direction.
    RingBuffer<int16_t, ring_len> pcm_buf_;

    Decimator4 decim_;
    Interpolator4 interp_;
};

}// namespace pika::audio
