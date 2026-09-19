#include "ADCMicrophone.h"

#include <cmath>
#include <pika/log.h>

namespace pika::audio {

ADCMicrophone *ADCMicrophone::instance_ = nullptr;

__attribute__((section(".nocache"), aligned(4))) adcsample_t ADCMicrophone::mic_buffer[2U * mic_buffer_half_len];

bool ADCMicrophone::init() {
    error_flags_ = 0U;
    ready_ = false;
    running_ = false;

    preamp(false);

    instance_ = this;

    adc_grp_.circular = true;
    adc_grp_.num_channels = 1U;
    adc_grp_.end_cb = fill_cb_static;
    adc_grp_.error_cb = error_cb;
    adc_grp_.cfgr = ADC_CFGR_RES_16BITS | ADC_CFGR_EXTEN_RISING | ADC_CFGR_EXTSEL_SRC(TRG_TIM6_TRGO);
    adc_grp_.pcsel = 1U << cfg_.channel;
    adc_grp_.smpr[cfg_.channel / 10U] = SMP_TIME << (3U * (cfg_.channel % 10U));
    adc_grp_.sqr[0] = ADC_SQR1_SQ1_N(cfg_.channel);

    if (adcStart(cfg_.adc, &adc_cfg_) != HAL_RET_SUCCESS) {
        return false;
    }

    if (!cfg_.clock->init()) {
        adcStop(cfg_.adc);
        return false;
    }

    ready_ = true;
    return true;
}

void ADCMicrophone::preamp(bool on) { palWriteLine(cfg_.shutdown, !on); }

bool ADCMicrophone::start_capture() {
    if (!ready_) {
        LOG("error: microphone is not initialized");
        return false;
    }

    if (capturing()) {
        LOG("error: microphone is busy");
        return false;
    }

    /* An early out, not the interlock: it saves powering the preamp up for
     20ms only to find the speaker talking. The claim below is what actually
     decides, and it is checked under the system lock. */
    if (cfg_.clock->held()) {
        LOG("error: sample clock is busy");
        return false;
    }

    /* The preamp comes up first and its output is given time to reach the
       bias point, so the ramp is not recorded as a thump. */
    preamp(true);
    chThdSleepMilliseconds(cfg_.settle_ms);

    running_ = true;
    adcStartConversion(cfg_.adc, &adc_grp_, mic_buffer, 2U * mic_buffer_half_len);

    if (cfg_.adc->state != ADC_ACTIVE) {
        LOG("error starting ADC conversion");
        running_ = false;
        preamp(false);
        return false;
    }

    /* The clock goes last, as on the speaker: the converter is armed and
     waiting before the first trigger can arrive. */
    if (!cfg_.clock->start(this)) {
        LOG("error: sample clock is busy");
        adcStopConversion(cfg_.adc);
        running_ = false;
        preamp(false);
        return false;
    }

    return true;
}

void ADCMicrophone::stop_capture() {
    if (!capturing()) {
        return;
    }

    osalSysLock();
    stop_capture_i();
    osalSysUnlock();

    preamp(false);
}

/*
 * The I-class form, for the error callback: it runs in the DMA interrupt,
 * where the ordinary calls would take the system lock again.
 */
void ADCMicrophone::stop_capture_i() {
    cfg_.clock->stop_i(this);

    if (cfg_.adc->state == ADC_ACTIVE) {
        adcStopConversionI(cfg_.adc);
    }

    running_ = false;
}

/*
 * Called from the DMA interrupt at the halfway point and at the end of the
 * buffer; adcIsBufferComplete() tells which, and so which half has just been
 * filled.
 */
void ADCMicrophone::fill_cb_static(ADCDriver *adcp) {
    /* Stopping the conversion disables the DMA stream and clears its pending
       flags under the lock, so a callback with no consumer is a driver bug. */
    chDbgAssert(instance_ != nullptr, "mic: instance_ == nullptr");

    adcsample_t *half = adcIsBufferComplete(adcp) ? &mic_buffer[mic_buffer_half_len] : &mic_buffer[0];
    instance_->fill_cb({half, mic_buffer_half_len});
}

/*
 * Raw codes to signed full scale, in place.
 */
void ADCMicrophone::fill_cb(std::span<adcsample_t> buf) {
    std::span<int16_t> out{reinterpret_cast<int16_t *>(buf.data()), buf.size()};
    for (size_t i = 0; i < buf.size(); i++) {
        float v = float((int32_t) buf[i] - (int32_t) ADC_VALUE_MID);
        float v_abs = std::fabs(v);
        float v_abs_gained = v_abs * agc_gain_;
        if (v_abs_gained >= ADC_VALUE_MID) {
            // Hard overload, set min gain immediately
            agc_gain_ = 1.0f;
        } else {
            // Proportional asymmetric gain regulation - slow up, fast down
            float gain_err = v_abs * agc_gain_ / AGC_TARGET;
            float w = gain_err > 1.0f ? AGC_W_DOWN : AGC_W_UP;
            agc_gain_ = std::fminf(AGC_GAIN_MAX, agc_gain_ * (1.0f + (1.0f - gain_err) * w));
        }
        out[i] = (int16_t) (((int32_t) buf[i] - (int32_t) ADC_VALUE_MID) * agc_gain_);
    }
    instance_->consumers_.for_each([&](AudioConsumer &consumer) { consumer.take_audio_buffer(out); });
}

void ADCMicrophone::error_cb(ADCDriver *adcp, adcerror_t err) {
    (void) adcp;

    chDbgAssert(instance_ != nullptr, "mic: instance_ == nullptr");

    if ((err & ADC_ERR_OVERFLOW) != 0U) {
        instance_->error_flags_ |= err_mic_overrun;
    }

    if ((err & ADC_ERR_DMAFAILURE) != 0U) {
        instance_->error_flags_ |= err_mic_dma;

        osalSysLockFromISR();
        instance_->stop_capture_i();
        osalSysUnlockFromISR();
    }
}

}// namespace pika::audio
