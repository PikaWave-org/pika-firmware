#include "MELPeRecorder.h"

#include <melpe/melpe.h>

#include "ch.h"

#include <algorithm>

namespace pika::audio {

namespace {

// Wakes the codec thread, which then checks everything.
constexpr eventmask_t evt_wake = EVENT_MASK(0);

}// namespace

chibios_rt::ThreadReference MELPeRecorder::start(tprio_t prio) {
    thread_ = BaseStaticThread::start(prio);
    return thread_;
}

void MELPeRecorder::start_record() {
    req_ = req_record;
    thread_.signalEvents(evt_wake);
}

void MELPeRecorder::stop_record() {
    req_ = req_stop;
    thread_.signalEvents(evt_wake);
}

bool MELPeRecorder::replay(AudioSink &sink) {
    if (frames_ == 0U) {
        return false;
    }

    sink_ = &sink;
    req_ = req_replay;
    thread_.signalEvents(evt_wake);
    return true;
}

void MELPeRecorder::take_audio_buffer(std::span<const int16_t> buf) {
    if (mode_ != mode_record or full_) {
        return;
    }

    const size_t n = buf.size() / Decimator4::factor;

    if (pcm_buf_.free() < n) {
        // The codec fell behind: drop this block.
        overruns_++;
        return;
    }

    // Split in two if the ring wraps; the decimator keeps its state between
    // the calls.
    const std::span<int16_t> first = pcm_buf_.write_span(n);
    decim_.process(buf.first(Decimator4::factor * first.size()), first);
    pcm_buf_.commit(first.size());

    if (first.size() < n) {
        const std::span<int16_t> rest = pcm_buf_.write_span(n - first.size());
        decim_.process(buf.subspan(Decimator4::factor * first.size()), rest);
        pcm_buf_.commit(rest.size());
    }

    if (pcm_buf_.size() >= frame_len) {
        chSysLockFromISR();
        thread_.signalEventsI(evt_wake);
        chSysUnlockFromISR();
    }
}

// Returning short ends the stream, so an underrun is padded with silence.
size_t MELPeRecorder::fill_audio_buffer(std::span<int16_t> buf) {
    const size_t src_size = buf.size() / Interpolator4::factor;// samples at 8kHz
    std::span<int16_t> src_span = pcm_buf_.read_span(src_size);   // first span size (before wrap)
    size_t out_size = Interpolator4::factor * src_span.size();
    interp_.process(src_span, buf.first(out_size));
    pcm_buf_.pop(src_span.size());

    if (src_span.size() < src_size and not pcm_buf_.empty()) {
        // handle the second part of the buffer after wrap
        const std::span<int16_t> rest_span = pcm_buf_.read_span(src_size - src_span.size());
        interp_.process(rest_span, buf.subspan(out_size, Interpolator4::factor * rest_span.size()));
        pcm_buf_.pop(rest_span.size());
        out_size += Interpolator4::factor * rest_span.size();
    }

    if (out_size < buf.size() and not drain_) {
        // Decoding fell behind: pad, so the stream keeps running.
        underruns_++;
        std::fill(buf.begin() + out_size, buf.end(), (int16_t) 0);
        out_size = buf.size();
    }

    chSysLockFromISR();
    thread_.signalEventsI(evt_wake);
    chSysUnlockFromISR();

    // Short only at the end: the speaker pads the rest and stops.
    return out_size;
}

void MELPeRecorder::encode_queued() {
    chDbgAssert(chThdGetSelfX() == thread_.getInner(), "melpe: encode off the codec thread");

    while (pcm_buf_.size() >= frame_len) {
        if (frames_ >= max_frames) {
            // Whole frames only, so the read position stays frame aligned.
            full_ = true;
            pcm_buf_.pop(pcm_buf_.size() / frame_len * frame_len);
            break;
        }

        const std::span<int16_t> frame = pcm_buf_.read_span(frame_len);
        chDbgAssert(frame.size() == frame_len, "melpe: capture frame split by the wrap");
        melpe_a24(&melpe_buf_[frames_ * frame_bytes], frame.data());
        pcm_buf_.pop(frame_len);
        frames_++;
    }
}

void MELPeRecorder::decode_ahead() {
    chDbgAssert(chThdGetSelfX() == thread_.getInner(), "melpe: decode off the codec thread");

    while (play_frame_ < frames_) {
        if (pcm_buf_.free() < frame_len) {
            break;
        }

        const std::span<int16_t> frame = pcm_buf_.write_span(frame_len);
        chDbgAssert(frame.size() == frame_len, "melpe: replay frame split by the wrap");
        melpe_s24(frame.data(), &melpe_buf_[play_frame_ * frame_bytes]);
        pcm_buf_.commit(frame_len);
        play_frame_++;
    }

    if (play_frame_ >= frames_) {
        drain_ = true;
    }
}

void MELPeRecorder::main() {
    chRegSetThreadName("melpe");

    while (true) {
        (void) chEvtWaitAnyTimeout(evt_wake, TIME_MS2I(100));

        const uint8_t req = req_;

        if (req != req_none) {
            req_ = req_none;

            switch (req) {
                case req_record:
                    frames_ = 0U;
                    overruns_ = 0U;
                    underruns_ = 0U;
                    full_ = false;
                    pcm_buf_.reset();
                    decim_.reset();
                    melpe_i24();
                    mode_ = mode_record;
                    break;

                case req_stop:
                    // Encode the whole frames still queued, then go idle.
                    mode_ = mode_drain;
                    break;

                case req_replay:
                    play_frame_ = 0U;
                    drain_ = false;
                    pcm_buf_.reset();
                    interp_.reset();
                    melpe_i24();
                    mode_ = mode_replay;

                    // Prime first, so the first pull finds audio.
                    decode_ahead();

                    if (not sink_->start_stream(*this)) {
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
                if (pcm_buf_.size() < frame_len) {
                    mode_ = mode_idle;
                }
                break;

            case mode_replay:
                decode_ahead();
                if (drain_ and pcm_buf_.empty()) {
                    mode_ = mode_idle;
                }
                break;

            default:
                break;
        }
    }
}

}// namespace pika::audio
