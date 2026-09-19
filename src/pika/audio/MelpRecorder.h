#pragma once

#include <pika/audio/audio_io.h>
#include <pika/audio/resample.h>

#include "ch.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::audio {

/*
 * Records the microphone into a MELPe 2400 bitstream and replays it by
 * decoding. 311 bytes a second, so a minute costs 18.7KB. Nothing stores raw
 * samples, and the replay sounds vocoded because it is.
 *
 * One class for both directions because the codec is a single global instance
 * with no reentrancy, and a second one has to stay inexpressible. Only the
 * codec thread calls into src/melpe/, and melpe_i24() runs on entering each
 * direction, which drops the previous replay's synthesis tail. SampleClock
 * already stops record and replay overlapping.
 *
 * analysis() is milliseconds, so the ADC interrupt only decimates 32kHz to
 * 8kHz and queues whole frames; the codec runs on its own thread.
 *
 * That thread is at NORMALPRIO + 1 and preempts the heartbeat, so everything
 * shared with it is a single volatile word with one writer, and both rings
 * have one reader and one writer with free running indices whose modulus
 * divides 65536.
 */
class MelpRecorder : public AudioConsumer, public AudioSource {
public:
    /** @brief  Samples in one codec frame, 22.5ms at 8kHz. */
    static constexpr size_t frame_len = 180U;

    /** @brief  Bytes per frame: 54 bits, packed 8 to the byte. */
    static constexpr size_t frame_bytes = 7U;

    /** @brief  60s at 22.5ms a frame, i.e. 18662 bytes. */
    static constexpr size_t max_frames = 2666U;

    void start(tprio_t prio, void *wa, size_t wa_size);

    /* Asked for by the heartbeat, acted on by the codec thread. */
    void start_record();
    void stop_record();
    bool replay(AudioSink &sink);

    /** @brief  True once the thread has encoded everything the interrupt
     *          queued, so the recording is complete. */
    [[nodiscard]] bool idle() const { return mode_ == mode_idle; }

    [[nodiscard]] bool playing() const { return playing_; }

    [[nodiscard]] bool full() const { return full_; }

    [[nodiscard]] uint32_t frames() const { return frames_; }

    [[nodiscard]] uint32_t overruns() const { return overruns_; }

    [[nodiscard]] uint32_t underruns() const { return underruns_; }

    /** @brief  Recording length in tenths of a second. */
    [[nodiscard]] uint32_t tenths() const { return frames_ * 225U / 1000U; }

    [[nodiscard]] uint32_t bytes() const { return frames_ * frame_bytes; }

    /* Runs in the ADC interrupt. */
    void take_audio_buffer(std::span<const int16_t> buf) override;

    /* Runs in the DAC interrupt. */
    size_t fill_audio_buffer(std::span<int16_t> buf) override;

private:
    /* 180ms of slack, against a full panel flush of 47ms. */
    static constexpr size_t q_slots = 8U;

    /* 128ms at 32kHz: one decode's 720 samples, the pull that takes them, and
       the thread being preempted in between. */
    static constexpr size_t ring_len = 4096U;

    enum : uint8_t { mode_idle, mode_record, mode_drain, mode_replay };

    enum : uint8_t { req_none, req_record, req_stop, req_replay };

    static void thread_trampoline(void *self);
    void run();
    void encode_queued();
    void decode_ahead();

    thread_t *thread_{};

    volatile uint8_t req_{req_none};   /* Heartbeat writes, thread clears.     */
    volatile uint8_t mode_{mode_idle}; /* Thread writes; arms the interrupt.  */

    AudioSink *sink_{};
    volatile bool playing_{};
    volatile bool drain_{};
    volatile bool full_{};

    volatile uint32_t frames_{};
    volatile uint32_t overruns_{};
    volatile uint32_t underruns_{};

    /* Capture: interrupt writes q_w_, thread writes q_r_. */
    Decimator4 decim_;
    int16_t q_[q_slots][frame_len]{};
    volatile uint16_t q_w_{};
    volatile uint16_t q_r_{};
    size_t q_fill_{}; /* Interrupt only. */

    /* Replay: thread writes ring_w_, interrupt writes ring_r_. */
    Interpolator4 interp_;
    int16_t ring_[ring_len]{};
    volatile uint16_t ring_w_{};
    volatile uint16_t ring_r_{};
    uint32_t play_frame_{}; /* Thread only. */
    int16_t play_pcm_[frame_len]{};
    int16_t play_up_[4U * frame_len]{};

    uint8_t bits_[max_frames * frame_bytes]{};
};

}// namespace pika::audio
