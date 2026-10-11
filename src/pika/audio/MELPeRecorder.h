#pragma once

#include <pika/RingBuffer.h>
#include <pika/audio/audio_io.h>
#include <pika/audio/resample.h>

#include "ch.h"
#include "ch.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::audio {

// Records the microphone into a MELPe 2400 bitstream (up to 60s) and replays
// it. The interrupts only resample and queue; the codec, which is not
// reentrant, runs on this object's thread. Shared state is single-writer.
class MELPeRecorder : public chibios_rt::BaseStaticThread<8192>, public AudioConsumer, public AudioSource {
public:
    // One 22.5ms frame: 180 samples at 8kHz, 54 bits packed into 7 bytes.
    static constexpr size_t frame_len = 180U;
    static constexpr size_t frame_bytes = 7U;
    static constexpr size_t max_frames = 2666U;// 60s

    chibios_rt::ThreadReference start(tprio_t prio) override;

    // Requests, carried out by the codec thread.
    void start_record();
    void stop_record();
    bool replay(AudioSink &sink);

    // True once everything queued is encoded or played.
    [[nodiscard]] bool idle() const { return mode_ == mode_idle; }

    [[nodiscard]] bool full() const { return full_; }

    [[nodiscard]] uint32_t frames() const { return frames_; }

    [[nodiscard]] uint32_t overruns() const { return overruns_; }

    [[nodiscard]] uint32_t underruns() const { return underruns_; }

    // Recording length in tenths of a second.
    [[nodiscard]] uint32_t tenths() const { return frames_ * 225U / 1000U; }

    [[nodiscard]] uint32_t bytes() const { return frames_ * frame_bytes; }

    // Runs in the ADC interrupt.
    void take_audio_buffer(std::span<const int16_t> buf) override;

    // Runs in the DAC interrupt.
    size_t fill_audio_buffer(std::span<int16_t> buf) override;

private:
    // 360ms of 8kHz PCM either way. A whole number of frames, so a codec
    // frame is never split by the wrap.
    static constexpr size_t ring_len = 16U * frame_len;

    enum : uint8_t { mode_idle, mode_record, mode_drain, mode_replay };

    enum : uint8_t { req_none, req_record, req_stop, req_replay };

    void main() override;
    void encode_queued();
    void decode_ahead();

    chibios_rt::ThreadReference thread_;

    volatile uint8_t req_{req_none};
    volatile uint8_t mode_{mode_idle};

    AudioSink *sink_{};
    volatile bool drain_{};
    volatile bool full_{};
    volatile uint32_t frames_{};
    volatile uint32_t overruns_{};
    volatile uint32_t underruns_{};

    // PCM at 8kHz between the speaker/microphone and the codec, in either direction.
    RingBuffer<int16_t, ring_len> pcm_buf_;

    Decimator4 decim_;
    Interpolator4 interp_;
    uint32_t play_frame_{};

    uint8_t melpe_buf_[max_frames * frame_bytes]{};
};

}// namespace pika::audio
