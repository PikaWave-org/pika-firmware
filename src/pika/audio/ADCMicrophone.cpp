#include "ADCMicrophone.h"

#include <pika/log.h>

#include <algorithm>
#include <limits>

namespace pika::audio {

ADCMicrophone *ADCMicrophone::instance_ = nullptr;

__attribute__((section(".nocache"), aligned(4))) adcsample_t ADCMicrophone::mic_buffer[2U * mic_buffer_half_len];

bool ADCMicrophone::init() {
    error_flags_ = 0U;
    ready_ = false;
    consumer_ = nullptr;
    dc_ = ADC_VALUE_MID << dc_shift;

    preamp(false);

    instance_ = this;

    adc_grp_.circular = true;
    adc_grp_.num_channels = 1U;
    adc_grp_.end_cb = fill_cb;
    adc_grp_.error_cb = error_cb;
    adc_grp_.cfgr = ADC_CFGR_RES_12BITS | ADC_CFGR_EXTEN_RISING | ADC_CFGR_EXTSEL_SRC(TRG_TIM6_TRGO);
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

bool ADCMicrophone::start_capture(AudioConsumer &dst) {
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

    /*
     * The preamp comes up first and its output is given time to reach the bias
     * point. Converting through that ramp would feed the DC tracker a sweep,
     * and it would spend the first fraction of a second chasing it.
     */
    preamp(true);
    chThdSleepMilliseconds(cfg_.settle_ms);

    dc_ = ADC_VALUE_MID << dc_shift;

    /* The consumer pointer is the busy flag, so it is published only once the
     call is committed, and taken back on every failure path below. */
    consumer_ = &dst;

    /* This one returns void, unlike its DAC counterpart, so the driver's own
     state is what says whether the conversion took. */
    adcStartConversion(cfg_.adc, &adc_grp_, mic_buffer, 2U * mic_buffer_half_len);

    if (cfg_.adc->state != ADC_ACTIVE) {
        LOG("error starting ADC conversion");
        consumer_ = nullptr;
        preamp(false);
        return false;
    }

    /* The clock goes last, as on the speaker: the converter is armed and
     waiting before the first trigger can arrive. */
    if (!cfg_.clock->start(this)) {
        LOG("error: sample clock is busy");
        adcStopConversion(cfg_.adc);
        consumer_ = nullptr;
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

    consumer_ = nullptr;
}

/*
 * Called from the DMA interrupt at the halfway point and at the end of the
 * buffer; adcIsBufferComplete() tells which, and so which half has just been
 * filled.
 */
void ADCMicrophone::fill_cb(ADCDriver *adcp) {
    /* Stopping the conversion disables the DMA stream and clears its pending
       flags under the lock, so a callback with no consumer is a driver bug. */
    chDbgAssert(instance_ != nullptr, "mic: instance_ == nullptr");
    chDbgAssert(instance_->consumer_ != nullptr, "mic: callback with no consumer");

    adcsample_t *half = adcIsBufferComplete(adcp) ? &mic_buffer[mic_buffer_half_len] : &mic_buffer[0];

    instance_->consumer_->take_audio_buffer(instance_->convert({half, mic_buffer_half_len}));
}

/*
 * Raw codes to signed full scale, in place.
 * The DC estimate is updated per sample and subtracted as it stands, so it
 * lags the input by one sample. At a 5Hz corner against a 32kHz rate that is
 * not a distinction worth an extra pass.
 */
std::span<const int16_t> ADCMicrophone::convert(std::span<adcsample_t> buf) {

    int16_t *out = reinterpret_cast<int16_t *>(buf.data());
    int32_t dc = (int32_t) dc_;

    for (size_t i = 0; i < buf.size(); i++) {
        const int32_t raw = buf[i];
        const int32_t centered = raw - (dc >> dc_shift);

        dc += ((raw << dc_shift) - dc) >> dc_rate;

        /* Clamped because a full scale swing maps to one past the top of the
           range. */
        out[i] = (int16_t) std::clamp<int32_t>(centered << ADC_TO_FULL_SCALE, std::numeric_limits<int16_t>::min(),
                                               std::numeric_limits<int16_t>::max());
    }

    dc_ = (uint32_t) dc;

    return {out, buf.size()};
}

/*
 * An overrun costs one sample and the converter carries on, so it is recorded
 * and the capture left running. A DMA failure is fatal to the stream: the
 * driver has already stopped the conversion, and the clock has to be released
 * too or it goes on triggering a converter that is no longer listening.
 */
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
