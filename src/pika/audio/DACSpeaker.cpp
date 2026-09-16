#include "DACSpeaker.h"

#include <pika/log.h>

namespace pika::audio {

DACSpeaker *DACSpeaker::instance_ = nullptr;

namespace {

/*
 * Sample buffer, in AHB SRAM2 via the linker's .nocache section. Placement is
 * not a detail and fails silently - wrong memory and the converter emits only
 * its init level: it has to be in the D2 domain the DMA can reach (not DTCM,
 * where stacks live) and outside the D-cache, or CPU and DMA see different
 * data. File-static rather than a member so that stays the driver's business,
 * at the cost of supporting one instance.
 *
 * The DMA runs circular over both halves, refilling one while it reads the
 * other.
 */
#define DMA_BUF __attribute__((section(".nocache"), aligned(4)))

DMA_BUF dacsample_t wave_buffer[2U * wave_buffer_half_len];

} /* anonymous namespace */

bool DACSpeaker::start_stream(AudioSource &src) {
    if (busy()) {
        LOG("error: speaker is busy");
        return false;// busy
    }

    draining_ = false;

    std::fill(std::begin(wave_buffer), std::end(wave_buffer), DAC_VALUE_MID);

    /* The source pointer is the busy flag, so it is published only once the
     call is committed, and taken back on every failure path below. */
    source_ = &src;

    if (dacStartConversion(cfg_.dac, &dac_grp_, wave_buffer, 2U * wave_buffer_half_len) != HAL_RET_SUCCESS) {
        LOG("error starting DAC conversion");
        source_ = nullptr;
        return false;
    }

    amp(true);
    chThdSleepMilliseconds(AMP_ON_DELAY);

    /*
     * The clock goes last, as the amplifier has to be ready before the first
     * sample rather than 20ms into the clip. If the microphone holds it the
     * claim fails here, after the amplifier came up - which is silent anyway,
     * since the converter is still sitting at its idle level.
     */
    if (!cfg_.clock->start(this)) {
        LOG("error: sample clock is busy");
        amp(false);
        dacStopConversion(cfg_.dac);
        source_ = nullptr;
        return false;
    }

    return true;
}

/* enable_on is the level that un-mutes, which is low on this board. */
void DACSpeaker::amp(bool on) { palWriteLine(cfg_.enable, on); }

/*
 * The output reaches mid scale as soon as the DAC is enabled. What takes time
 * is the amplifier's input capacitor charging through 100k, which is what
 * settle_ms is spent on here, still muted.
 */
bool DACSpeaker::init() {
    error_flags_ = 0U;
    ready_ = false;
    source_ = nullptr;
    draining_ = false;

    amp(false);

    instance_ = this;

    if (dacStart(cfg_.dac, &dac_cfg) != HAL_RET_SUCCESS) {
        return false;
    }

    if (!cfg_.clock->init()) {
        dacStop(cfg_.dac);
        return false;
    }

    chThdSleepMilliseconds(cfg_.settle_ms);

    ready_ = true;
    return true;
}

/*
 * Stopping leaves the output register alone, so it is written back to mid
 * scale below: otherwise the output freezes at the last sample, an arbitrary
 * DC offset that pops at the next tone.
 */
void DACSpeaker::stop_stream() {
    if (!busy()) {
        return;
    }

    osalSysLock();
    stop_stream_i();
    osalSysUnlock();
}

/*
 * The I-class form, used at the end of a stream: the callback runs in the DMA
 * interrupt, where the ordinary calls would take the system lock again - and
 * dacStopConversion() also asserts against the DAC_COMPLETE state the driver
 * sets around the callback, which the I-class version accepts.
 */
void DACSpeaker::stop_stream_i() {
    cfg_.clock->stop_i(this);
    dacStopConversionI(cfg_.dac);
    dacPutChannelX(cfg_.dac, 0U, DAC_VALUE_MID);

    source_ = nullptr;/* idle again */
    draining_ = false;
}

/*
 * Called from the DMA interrupt at the halfway point and at the end of the
 * buffer; dacIsBufferComplete() tells which, and so which half is free. Both
 * can arrive in one entry, hence deciding from the flag and not from state
 * kept between calls.
 *
 * The end of a stream takes one callback more than it looks like it should:
 * the half being refilled is not the one playing, so when the source runs dry
 * there is still 8ms queued ahead of it. The short half is padded to idle -
 * which also overwrites what it held one wrap ago - and draining_ makes the
 * next callback stop, once the last real samples are out.
 */
void DACSpeaker::fill_cb(DACDriver *dacp) {
    if (instance_ == nullptr or instance_->source_ == nullptr) {
        return;
    }

    if (instance_->draining_) {
        osalSysLockFromISR();
        instance_->stop_stream_i();
        osalSysUnlockFromISR();
        return;
    }

    dacsample_t *half = dacIsBufferComplete(dacp) ? &wave_buffer[wave_buffer_half_len] : &wave_buffer[0];

    /* The source writes signed samples into the DMA half itself, which is then
     scaled where they lie: both forms are 16 bit, so one buffer holds either
     and nothing is copied between them. */
    size_t n = instance_->source_->fill_audio_buffer({reinterpret_cast<int16_t *>(half), wave_buffer_half_len});

    n = std::min<size_t>(n, wave_buffer_half_len);

    instance_->scale({half, n});

    if (n < wave_buffer_half_len) {
        std::fill(half + n, half + wave_buffer_half_len, DAC_VALUE_MID);
        instance_->draining_ = true;
    }
}

/*
 * Signed full scale to converter codes, in place. Scale and bias are both the
 * mid point, since signed 16 bit spans twice the half range either side of it.
 * The volume is read once per half so it cannot step mid-buffer, and the
 * result is clamped because -32768 would otherwise land one code low.
 */
void DACSpeaker::scale(std::span<dacsample_t> buf) const {
    const float scale = volume_ * float(DAC_VALUE_MID) / 32768.0f;
    const int16_t *in = reinterpret_cast<const int16_t *>(buf.data());

    for (size_t i = 0; i < buf.size(); i++) {
        buf[i] = dacsample_t(std::clamp(int(DAC_VALUE_MID) + int(float(in[i]) * scale), 0, int(DAC_VALUE_MAX)));
    }
}

void DACSpeaker::error_cb(DACDriver *dacp, dacerror_t err) {
    (void) dacp;
    (void) err;

    if (instance_ == nullptr) {
        return;
    }

    /*
     * The driver has already stopped the conversion, but not the timer, which
     * would go on requesting samples. It cannot be left to recover() either:
     * clearing the source below marks the speaker idle, and stop_stream()
     * returns at once once it is clear. No LOG() here - it would block on the
     * serial queue; recover() reads the flag instead.
     */
    osalSysLockFromISR();
    instance_->cfg_.clock->stop_i(instance_);
    osalSysUnlockFromISR();

    instance_->error_flags_ |= err_dma;
    instance_->source_ = nullptr;
    instance_->draining_ = false;
}

/*
 * An underrun clears DMAEN and the audio stops dead. The low level driver
 * never notices: its error path covers the DMA controller's own faults, not
 * the converter's complaint about being starved.
 */
void DACSpeaker::check_underrun() {
    DAC_TypeDef *dac = cfg_.dac->params->dac;
    uint32_t udr = DAC_SR_DMAUDR1 << cfg_.dac->params->regshift;

    if ((dac->SR & udr) != 0U) {
        dac->SR = udr;
        error_flags_ |= err_underrun;
    }
}

bool DACSpeaker::recover() {
    error_flags_ = 0U;

    if (!ready_) {
        return false;
    }

    amp(false);
    stop_stream();
    dacPutChannelX(cfg_.dac, 0U, DAC_VALUE_MID);

    return true;
}

}// namespace pika::audio
