#include "DACSpeaker.h"

#include <pika/log.h>

namespace pika::audio {

DACSpeaker *DACSpeaker::instance_ = nullptr;

namespace {

// Must be DMA-reachable and non-cacheable.
#define DMA_BUF __attribute__((section(".nocache"), aligned(4)))

DMA_BUF dacsample_t wave_buffer[2U * wave_buffer_half_len];

}// anonymous namespace

bool DACSpeaker::start_stream(AudioSource &src) {
    if (busy()) {
        LOG("error: speaker is busy");
        return false;
    }

    drain_left_ = 0;

    std::fill(std::begin(wave_buffer), std::end(wave_buffer), DAC_VALUE_MID);

    source_ = &src;

    if (dacStartConversion(cfg_.dac, &dac_grp_, wave_buffer, 2U * wave_buffer_half_len) != HAL_RET_SUCCESS) {
        LOG("error starting DAC conversion");
        source_ = nullptr;
        return false;
    }

    amp(true);
    chThdSleep(AMP_ON_DELAY);

    // Clock last, once the amplifier is up.
    if (not cfg_.clock->start(this)) {
        LOG("error: sample clock is busy");
        amp(false);
        dacStopConversion(cfg_.dac);
        source_ = nullptr;
        return false;
    }

    return true;
}

void DACSpeaker::amp(bool on) { palWriteLine(cfg_.enable, on); }

bool DACSpeaker::init() {
    error_flags_ = 0U;
    ready_ = false;
    source_ = nullptr;
    drain_left_ = 0;

    amp(false);

    instance_ = this;

    if (dacStart(cfg_.dac, &dac_cfg) != HAL_RET_SUCCESS) {
        return false;
    }

    if (not cfg_.clock->init()) {
        dacStop(cfg_.dac);
        return false;
    }

    chThdSleepMilliseconds(cfg_.settle_ms);

    ready_ = true;
    return true;
}

void DACSpeaker::stop_stream() {
    if (not busy()) {
        return;
    }

    osalSysLock();
    stop_stream_i();
    osalOsRescheduleS();
    osalSysUnlock();
}

bool DACSpeaker::wait_idle(sysinterval_t timeout) {
    msg_t msg = MSG_OK;

    // Checked under the lock, so an end just before the suspend is not lost.
    osalSysLock();
    if (source_ != nullptr) {
        msg = osalThreadSuspendTimeoutS(&idle_waiter_, timeout);
    }
    osalSysUnlock();

    return msg == MSG_OK;
}

void DACSpeaker::stop_stream_i() {
    cfg_.clock->stop_i(this);
    amp(false);
    dacStopConversionI(cfg_.dac);
    // Back to mid scale, or the next stream starts with a pop.
    dacPutChannelX(cfg_.dac, 0U, DAC_VALUE_MID);

    source_ = nullptr;
    drain_left_ = 0;
    osalThreadResumeI(&idle_waiter_, MSG_OK);
}

// Refills the half the DMA just finished. A half plays from the next callback
// to the one after, so a short half ends the stream two callbacks later.
void DACSpeaker::fill_cb(DACDriver *dacp) {
    if (instance_ == nullptr or instance_->source_ == nullptr) {
        return;
    }

    if (instance_->drain_left_ > 0U) {
        instance_->drain_left_ = instance_->drain_left_ - 1U;
        if (instance_->drain_left_ == 0U) {
            osalSysLockFromISR();
            instance_->stop_stream_i();
            osalSysUnlockFromISR();
        }
        return;
    }

    dacsample_t *half = dacIsBufferComplete(dacp) ? &wave_buffer[wave_buffer_half_len] : &wave_buffer[0];

    size_t n = instance_->source_->fill_audio_buffer({reinterpret_cast<int16_t *>(half), wave_buffer_half_len});

    instance_->scale({half, n});

    if (n < wave_buffer_half_len) {
        std::fill(half + n, half + wave_buffer_half_len, DAC_VALUE_MID);
        instance_->drain_left_ = 2U;
    }
}

// Clamped, as -32768 would otherwise land one code low.
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

    // The driver stopped the conversion, not the clock.
    osalSysLockFromISR();
    instance_->cfg_.clock->stop_i(instance_);
    instance_->error_flags_ |= err_dma;
    instance_->source_ = nullptr;
    instance_->drain_left_ = 0;
    osalThreadResumeI(&instance_->idle_waiter_, MSG_RESET);
    osalSysUnlockFromISR();
}

// An underrun stops the DMA without the driver noticing.
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

    if (not ready_) {
        return false;
    }

    amp(false);
    stop_stream();
    dacPutChannelX(cfg_.dac, 0U, DAC_VALUE_MID);

    return true;
}

}// namespace pika::audio
