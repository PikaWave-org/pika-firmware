#include "MelpRecorder.h"

#include <melpe/melpe.h>
#include <pika/log.h>

#include "ch.h"
#include "hal.h" /* STM32_CORE_CK, for the encode timing. */

#include <algorithm>

namespace pika::audio {

namespace {

/* One event per thing that can give the thread work. The event carries no
   count on purpose: how much work there is comes from the ring indices and
   from nowhere else, so a count and an index cannot disagree - which is the
   usual way this shape goes wrong. */
constexpr eventmask_t evt_pcm_in = EVENT_MASK(0);  /* a frame was captured   */
constexpr eventmask_t evt_pcm_out = EVENT_MASK(1); /* the speaker took some  */
constexpr eventmask_t evt_req = EVENT_MASK(2);     /* the heartbeat asked    */

uint32_t cycles_to_us(uint32_t cycles) {
    return (uint32_t) ((uint64_t) cycles * 1000000ULL / (uint64_t) STM32_CORE_CK);
}

}// namespace

void MelpRecorder::start(tprio_t prio, void *wa, size_t wa_size) {
    wa_ = wa;
    wa_size_ = wa_size;
    thread_ = chThdCreateStatic(wa, wa_size, prio, thread_trampoline, this);
}

void MelpRecorder::thread_trampoline(void *self) { static_cast<MelpRecorder *>(self)->run(); }

void MelpRecorder::start_record() {
    req_ = req_record;
    chEvtSignal(thread_, evt_req);
}

void MelpRecorder::stop_record() {
    req_ = req_stop;
    chEvtSignal(thread_, evt_req);
}

bool MelpRecorder::replay(AudioSink &sink) {
    if (frames_ == 0U) {
        return false;
    }

    /* playing_ is raised here rather than by the thread so that the caller's
       state machine can wait on it without a window where the replay has been
       asked for but has not visibly started. The thread lowers it. */
    sink_ = &sink;
    playing_ = true;
    req_ = req_replay;
    chEvtSignal(thread_, evt_req);
    return true;
}

uint32_t MelpRecorder::encode_max_us() const { return cycles_to_us(enc_max_); }

uint32_t MelpRecorder::encode_mean_us() const { return enc_n_ ? cycles_to_us((uint32_t) (enc_sum_ / enc_n_)) : 0U; }

size_t MelpRecorder::stack_free() const {
    const uint8_t *p = (const uint8_t *) wa_;
    size_t i = 0;

    while (p != nullptr && i < wa_size_ && p[i] == CH_DBG_STACK_FILL_VALUE) { i++; }
    return i;
}

/*
 * Runs in the ADC interrupt, once per 8ms buffer half. Decimates 256 samples
 * to 64 and appends them to the frame being filled, handing whole frames to
 * the thread. Arithmetic and a copy, nothing that can block.
 */
void MelpRecorder::take_audio_buffer(std::span<const int16_t> buf) {
    if (mode_ != mode_record || full_) {
        return;
    }

    int16_t dec[Decimator4::max_in / 4U];
    const size_t n = decim_.process(buf, dec);
    size_t i = 0;

    while (i < n) {
        if ((uint16_t) (q_w_ - q_r_) >= q_slots) {
            /* The thread missed its deadline. Drop the newest frame, count it
               and keep recording: a 22.5ms hole is better than losing the
               whole take, but it has to be visible, so the panel says so. */
            overruns_++;
            return;
        }

        const size_t take = std::min(frame_len - q_fill_, n - i);

        std::copy_n(&dec[i], take, &q_[q_w_ & (q_slots - 1U)][q_fill_]);
        q_fill_ += take;
        i += take;

        if (q_fill_ == frame_len) {
            q_fill_ = 0U;
            q_w_++;

            chSysLockFromISR();
            chEvtSignalI(thread_, evt_pcm_in);
            chSysUnlockFromISR();
        }
    }
}

/*
 * Runs in the DAC interrupt. Copies out of the ring the thread is decoding
 * into.
 *
 * Returning short would end the stream (DACSpeaker.cpp:166), so an underrun
 * pads with silence and still returns the full count: one scheduling hiccup
 * must not truncate a minute of audio. The stream ends exactly one way - the
 * thread has decoded the last frame and the ring has run dry.
 */
size_t MelpRecorder::fill_audio_buffer(std::span<int16_t> buf) {
    const size_t avail = (size_t) (uint16_t) (ring_w_ - ring_r_);

    if (avail == 0U && drain_) {
        playing_ = false;
        return 0U;
    }

    const size_t take = std::min(avail, buf.size());

    for (size_t i = 0; i < take; i++) { buf[i] = ring_[(ring_r_ + i) & (ring_len - 1U)]; }
    ring_r_ = (uint16_t) (ring_r_ + take);

    if (take < buf.size()) {
        if (!drain_) {
            underruns_++;
        }
        std::fill(buf.begin() + take, buf.end(), (int16_t) 0);
    }

    chSysLockFromISR();
    chEvtSignalI(thread_, evt_pcm_out);
    chSysUnlockFromISR();

    return buf.size();
}

void MelpRecorder::encode_queued() {
    chDbgAssert(chThdGetSelfX() == thread_, "melpe: encode off the codec thread");

    while (q_r_ != q_w_) {
        if (frames_ >= max_frames) {
            /* Out of room. Swallow whatever is queued so the interrupt does
               not also start counting overruns for it. */
            full_ = true;
            q_r_ = q_w_;
            break;
        }

        const rtcnt_t t0 = chSysGetRealtimeCounterX();

        melpe_a24(&bits_[frames_ * frame_bytes], q_[q_r_ & (q_slots - 1U)]);

        const uint32_t dt = (uint32_t) (chSysGetRealtimeCounterX() - t0);

        if (dt > enc_max_) {
            enc_max_ = dt;
        }
        enc_sum_ += dt;
        enc_n_++;

        frames_++;
        q_r_++;
    }
}

void MelpRecorder::decode_ahead() {
    chDbgAssert(chThdGetSelfX() == thread_, "melpe: decode off the codec thread");

    constexpr size_t up_len = 4U * frame_len;

    while (play_frame_ < frames_) {
        const size_t used = (size_t) (uint16_t) (ring_w_ - ring_r_);

        if (ring_len - used < up_len) {
            break;
        }

        melpe_s24(play_pcm_, &bits_[play_frame_ * frame_bytes]);
        play_frame_++;

        interp_.process(play_pcm_, play_up_);

        for (size_t i = 0; i < up_len; i++) { ring_[(ring_w_ + i) & (ring_len - 1U)] = play_up_[i]; }
        ring_w_ = (uint16_t) (ring_w_ + up_len);
    }

    if (play_frame_ >= frames_) {
        drain_ = true;
    }
}

void MelpRecorder::run() {
    chRegSetThreadName("melpe");

    while (true) {
        /* Bounded on purpose. The events are the mechanism; the timeout is a
           safety net, so that a converter that stops delivering degrades into
           a slow poll rather than a thread that never wakes. */
        (void) chEvtWaitAnyTimeout(ALL_EVENTS, TIME_MS2I(100));

        const uint8_t req = req_;

        if (req != req_none) {
            req_ = req_none;

            switch (req) {
                case req_record:
                    frames_ = 0U;
                    overruns_ = 0U;
                    underruns_ = 0U;
                    full_ = false;
                    enc_max_ = 0U;
                    enc_sum_ = 0U;
                    enc_n_ = 0U;
                    q_r_ = q_w_;
                    decim_.reset();
                    melpe_i24();
                    mode_ = mode_record; /* Arms the interrupt. */
                    break;

                case req_stop:
                    /* Disarms the interrupt, but the queue may still hold up
                       to eight frames; idle() goes true when they are in. */
                    mode_ = mode_drain;
                    break;

                case req_replay:
                    play_frame_ = 0U;
                    drain_ = false;
                    ring_r_ = 0U;
                    ring_w_ = 0U;
                    interp_.reset();
                    melpe_i24();
                    mode_ = mode_replay;

                    /* Prime before handing the source to the speaker, so the
                       first pull finds audio rather than the underrun pad. */
                    decode_ahead();

                    if (!sink_->start_stream(*this)) {
                        playing_ = false;
                        mode_ = mode_idle;
                    }
                    break;

                default:
                    break;
            }
        }

        switch (mode_) {
            case mode_record:
                encode_queued();
                break;

            case mode_drain:
                encode_queued();
                if (q_r_ == q_w_) {
                    mode_ = mode_idle;
                }
                break;

            case mode_replay:
                decode_ahead();
                if (!playing_) {
                    mode_ = mode_idle;
                }
                break;

            default:
                break;
        }
    }
}

}// namespace pika::audio
