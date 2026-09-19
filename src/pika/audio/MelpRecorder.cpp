#include "MelpRecorder.h"

#include <melpe/melpe.h>

#include "ch.h"

#include <algorithm>

namespace pika::audio {

namespace {

/* The events carry no count: how much work there is comes from the ring
   indices, so a count and an index cannot disagree. */
constexpr eventmask_t evt_pcm_in = EVENT_MASK(0);
constexpr eventmask_t evt_pcm_out = EVENT_MASK(1);
constexpr eventmask_t evt_req = EVENT_MASK(2);

}// namespace

void MelpRecorder::start(tprio_t prio, void *wa, size_t wa_size) {
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

    /* Raised here, not by the thread, so the caller can wait on it without a
       window where the replay has been asked for but has not started. */
    sink_ = &sink;
    playing_ = true;
    req_ = req_replay;
    chEvtSignal(thread_, evt_req);
    return true;
}

void MelpRecorder::take_audio_buffer(std::span<const int16_t> buf) {
    if (mode_ != mode_record || full_) {
        return;
    }

    int16_t dec[Decimator4::max_in / 4U];
    const size_t n = decim_.process(buf, dec);
    size_t i = 0;

    while (i < n) {
        if ((uint16_t) (q_w_ - q_r_) >= q_slots) {
            /* The thread missed its deadline. A 22.5ms hole beats losing the
               take, but the panel says so. */
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
 * Returning short would end the stream (DACSpeaker.cpp:166), so an underrun
 * pads and still returns the full count. The stream ends one way only: the
 * last frame is decoded and the ring has run dry.
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
            /* Swallow the queue so the interrupt does not count overruns for
               audio there is no room for. */
            full_ = true;
            q_r_ = q_w_;
            break;
        }

        melpe_a24(&bits_[frames_ * frame_bytes], q_[q_r_ & (q_slots - 1U)]);
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
        /* The events are the mechanism; the timeout only keeps every wait
           bounded, so a converter that stops delivering degrades to a poll. */
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
                    q_r_ = q_w_;
                    decim_.reset();
                    melpe_i24();
                    mode_ = mode_record;
                    break;

                case req_stop:
                    /* The queue may still hold eight frames; idle() goes true
                       when they are in. */
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

                    /* Prime first, so the first pull finds audio. */
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
