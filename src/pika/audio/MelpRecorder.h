#pragma once

#include <pika/audio/audio_io.h>
#include <pika/audio/resample.h>

#include "ch.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::audio {

/*
 * Records the microphone straight into a MELPe 2400 bitstream, and replays it
 * by decoding.
 *
 * The recording is 311 bytes a second rather than 64KB, so a minute costs
 * 18.7KB where four seconds of PCM used to cost 250KB. Nothing stores raw
 * samples: what is captured is encoded as it arrives, and what is replayed is
 * decoded on the way out. The replay therefore sounds vocoded, because it is.
 *
 * One class for both directions, rather than a recorder and a player, because
 * the codec underneath is a single global instance with no reentrancy: every
 * one of its buffers, filter memories and quantizer tables is a file scope
 * static. Splitting it across two owners would make it possible to have two,
 * and that has to stay impossible. Two rules keep it honest:
 *
 *   1. Only the codec thread ever calls into src/melpe/, asserted at the call.
 *      Not the ISRs, not the heartbeat.
 *   2. melpe_i24() runs on entering each direction, which is what drops the
 *      previous replay's synthesis tail.
 *
 * The board already enforces the rest: SampleClock refuses to let the speaker
 * and the microphone run at once, so record and replay cannot overlap.
 *
 * Where the work happens matters. analysis() is milliseconds, so it cannot run
 * in the ADC interrupt - the interrupt decimates 32kHz to 8kHz (63us of an 8ms
 * callback) and hands whole 180 sample frames to the codec thread through a
 * ring. Decimating on the interrupt side rather than shipping raw 32kHz across
 * makes the ring four times smaller and leaves the thread doing nothing but
 * the codec, which is the thing whose timing has to be trusted.
 *
 * Threading contract, which is not optional reading: this thread runs at
 * NORMALPRIO + 1, above the heartbeat, because it is the only one here with a
 * deadline. That means it preempts, so nothing shared with the heartbeat may
 * be a multi-word update - every field below that crosses that boundary is a
 * single volatile word with exactly one writer. Same for the two rings: one
 * writer and one reader each, free running indices whose modulus divides
 * 65536, so a torn index is not representable.
 */
class MelpRecorder : public AudioConsumer, public AudioSource {
public:
    /** @brief  Samples in one codec frame, 22.5ms at 8kHz. */
    static constexpr size_t frame_len = 180U;

    /** @brief  Bytes one frame encodes to: 54 bits, packed 8 to the byte. */
    static constexpr size_t frame_bytes = 7U;

    /** @brief  Codec sample rate. The converters run at four times this. */
    static constexpr uint32_t codec_rate = 8000U;

    /* 60s of speech. 60 / 0.0225 is 2666 frames and 59.985s, which is the
       length the panel rounds to 60.0. At 7 bytes a frame the whole thing is
       18662 bytes - small enough that the limit is the feature's, not the
       memory's, and one line to change. */
    static constexpr size_t max_frames = 2666U;

    void start(tprio_t prio, void *wa, size_t wa_size);

    /* Asked for by the heartbeat, acted on by the codec thread. None of these
       touch the codec themselves. */
    void start_record();
    void stop_record();
    bool replay(AudioSink &sink);

    /** @brief  True once the thread has encoded everything the interrupt
     *          queued, i.e. the recording is complete and safe to replay. */
    [[nodiscard]] bool idle() const { return mode_ == mode_idle; }

    [[nodiscard]] bool playing() const { return playing_; }

    [[nodiscard]] bool full() const { return full_; }

    [[nodiscard]] uint32_t frames() const { return frames_; }

    [[nodiscard]] uint32_t overruns() const { return overruns_; }

    [[nodiscard]] uint32_t underruns() const { return underruns_; }

    /** @brief  Recording length in tenths of a second. */
    [[nodiscard]] uint32_t tenths() const { return frames_ * 225U / 1000U; }

    /** @brief  Encoded bytes, for the log line. */
    [[nodiscard]] uint32_t bytes() const { return frames_ * frame_bytes; }

    /** @brief  Worst and mean encode time in microseconds, against a 22500us
     *          budget. The whole feature stands on these two numbers. */
    [[nodiscard]] uint32_t encode_max_us() const;
    [[nodiscard]] uint32_t encode_mean_us() const;

    /** @brief  Unused bytes of the codec thread's working area, from the fill
     *          CH_DBG_FILL_THREADS leaves. Zero means it was never measured. */
    [[nodiscard]] size_t stack_free() const;

    /* Runs in the ADC interrupt. */
    void take_audio_buffer(std::span<const int16_t> buf) override;

    /* Runs in the DAC interrupt. */
    size_t fill_audio_buffer(std::span<int16_t> buf) override;

private:
    /* Slots of 22.5ms each, so eight is 180ms of slack before a frame is lost.
       A full panel flush is 47ms, so the thread can lose four of those and
       still drop nothing. 65536 is a multiple of 8, so the free running
       indices wrap without a modulo. */
    static constexpr size_t q_slots = 8U;

    /* 128ms at 32kHz: enough for one decode's 720 samples, the 256 sample pull
       that consumes them, and the thread being preempted in between. A power
       of two that divides 65536, for the same reason. */
    static constexpr size_t ring_len = 4096U;

    enum : uint8_t { mode_idle, mode_record, mode_drain, mode_replay };

    enum : uint8_t { req_none, req_record, req_stop, req_replay };

    static void thread_trampoline(void *self);
    void run();
    void encode_queued();
    void decode_ahead();

    thread_t *thread_{};
    void *wa_{};
    size_t wa_size_{};

    /* Written by the heartbeat, read and cleared by the thread. */
    volatile uint8_t req_{req_none};

    /* Written by the thread only. The interrupt reads it to decide whether to
       append, so it is what arms and disarms capture. */
    volatile uint8_t mode_{mode_idle};

    AudioSink *sink_{};
    volatile bool playing_{};
    volatile bool drain_{};
    volatile bool full_{};

    volatile uint32_t frames_{};
    volatile uint32_t overruns_{};
    volatile uint32_t underruns_{};

    /* Encode timing, thread only. */
    uint32_t enc_max_{};
    uint64_t enc_sum_{};
    uint32_t enc_n_{};

    /* Capture: interrupt writes q_w_, thread writes q_r_. */
    Decimator4 decim_;
    int16_t q_[q_slots][frame_len]{};
    volatile uint16_t q_w_{};
    volatile uint16_t q_r_{};
    size_t q_fill_{}; /* Samples in the slot being written; interrupt only. */

    /* Replay: thread writes ring_w_, interrupt writes ring_r_. */
    Interpolator4 interp_;
    int16_t ring_[ring_len]{};
    volatile uint16_t ring_w_{};
    volatile uint16_t ring_r_{};
    uint32_t play_frame_{}; /* Next frame to decode; thread only. */
    int16_t play_pcm_[frame_len]{};
    int16_t play_up_[4U * frame_len]{};

    /* The recording. Plain .bss: no DMA ever sees it, the interrupt never
       touches it, and it would not fit the 16K .nocache region anyway. */
    uint8_t bits_[max_frames * frame_bytes]{};
};

}// namespace pika::audio
