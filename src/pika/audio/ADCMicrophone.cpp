#include "ADCMicrophone.h"

#include <pika/log.h>

#include <algorithm>

namespace pika::audio {

ADCMicrophone *ADCMicrophone::instance_ = nullptr;

namespace {

/*
 * Sample buffer, in AHB SRAM2 via the linker's .nocache section. Placement is
 * not a detail and fails silently, exactly as on the speaker's buffer: it has
 * to be in the D2 domain the DMA can reach (not DTCM, where stacks live) and
 * outside the D-cache, or the CPU reads whatever was in the cache line while
 * the DMA writes somewhere it cannot see. Nothing reports this - the callbacks
 * still arrive on time, carrying stale data.
 *
 * The ADCv4 driver does no cache maintenance of its own, so the section is the
 * whole of the answer here.
 *
 * File-static rather than a member so that stays the driver's business, at the
 * cost of supporting one instance.
 */
#define DMA_BUF __attribute__((section(".nocache"), aligned(4)))

DMA_BUF adcsample_t mic_buffer[2U * mic_buffer_half_len];

} /* anonymous namespace */

/*
 * The conversion group is built here rather than being a constant, because
 * which input the preamp is wired to arrives in the Config and decides the
 * channel select, the sampling time slot and the sequence entry.
 */
bool ADCMicrophone::init() {
    error_flags_ = 0U;
    ready_ = false;
    consumer_ = nullptr;
    dc_ = ADC_VALUE_MID << dc_shift;

    preamp(false);

    instance_ = this;

    /* Single ended, no linearity calibration: the low level driver calibrates
     on start either way, and audio does not need the slower variant. */
    adc_cfg_.difsel = 0U;
    adc_cfg_.calibration = 0U;

    adc_grp_.circular = true;
    adc_grp_.num_channels = 1U;
    adc_grp_.end_cb = fill_cb;

    /* Not optional: the error callback being present is what makes the low
     level driver enable the overrun interrupt at all. */
    adc_grp_.error_cb = error_cb;

    /* 12 bit, converting on the sample clock's rising TRGO. CONT stays off -
     the trigger sets the rate, and a continuous ADC would free run at its own
     speed instead. DMNGT is the driver's and must not be set here. */
    adc_grp_.cfgr = ADC_CFGR_RES_12BITS | ADC_CFGR_EXTEN_RISING | ADC_CFGR_EXTSEL_SRC(TRG_TIM6_TRGO);
    adc_grp_.cfgr2 = 0U;

    /* Dual mode only, and this port is single. */
    adc_grp_.ccr = 0U;

    /*
     * The channel preselection. The driver copies this verbatim and never
     * derives it from the sequence, so a missing bit here is a converter that
     * runs perfectly and reads nothing.
     */
    adc_grp_.pcsel = 1U << cfg_.channel;

    /* Watchdogs wide open. None is armed - CFGR carries no AWD1EN and the
     AWD2/3 channel masks are empty - but the driver enables their interrupts
     alongside the overrun one, so the thresholds are set out of range rather
     than left at zero. */
    adc_grp_.ltr1 = 0x00000000U;
    adc_grp_.htr1 = 0x03FFFFFFU;
    adc_grp_.ltr2 = 0x00000000U;
    adc_grp_.htr2 = 0x03FFFFFFU;
    adc_grp_.ltr3 = 0x00000000U;
    adc_grp_.htr3 = 0x03FFFFFFU;
    adc_grp_.awd2cr = 0U;
    adc_grp_.awd3cr = 0U;

    /* SMPR1 covers channels 0 to 9 and SMPR2 the rest, three bits each. */
    adc_grp_.smpr[0] = 0U;
    adc_grp_.smpr[1] = 0U;
    adc_grp_.smpr[cfg_.channel / 10U] = SMP_TIME << (3U * (cfg_.channel % 10U));

    /*
     * A sequence of one: the channel as SQ1. The length field is deliberately
     * absent - the driver ORs ADC_SQR1_NUM_CH in from num_channels, and
     * setting it here as well corrupts it.
     */
    adc_grp_.sqr[0] = ADC_SQR1_SQ1_N(cfg_.channel);
    adc_grp_.sqr[1] = 0U;
    adc_grp_.sqr[2] = 0U;
    adc_grp_.sqr[3] = 0U;

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

/* The preamp's shutdown input is active high, so it is driven low to listen. */
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

    /* Outside the lock: a GPIO write is cheap but there is no reason for it to
     be inside, and stop_capture_i() is also reached from the error path. */
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
 * filled. Unlike the speaker's there is no drain phase: a recording ends when
 * the caller says so, not when the data runs out.
 *
 * adcStartConversion() is given the depth of the whole buffer - the driver
 * takes a total, not a half - and splits it itself, so "complete" means the
 * second half and anything else the first.
 */
void ADCMicrophone::fill_cb(ADCDriver *adcp) {
    if (instance_ == nullptr or instance_->consumer_ == nullptr) {
        return;
    }

    adcsample_t *half = adcIsBufferComplete(adcp) ? &mic_buffer[mic_buffer_half_len] : &mic_buffer[0];

    instance_->consumer_->take_audio_buffer(instance_->convert({half, mic_buffer_half_len}));
}

/*
 * Raw codes to signed full scale, in place. Both forms are 16 bit, so one
 * buffer holds either and nothing is copied between them - the same trick the
 * speaker plays in the other direction.
 *
 * The DC estimate is updated per sample and subtracted as it stands, so it
 * lags the input by one sample. At a 5Hz corner against a 32kHz rate that is
 * not a distinction worth an extra pass.
 */
std::span<const int16_t> ADCMicrophone::convert(std::span<adcsample_t> buf) {

    int16_t *out = reinterpret_cast<int16_t *>(buf.data());
    int32_t dc = (int32_t) dc_;

    for (size_t i = 0; i < buf.size(); i++) {
        const int32_t raw = (int32_t) buf[i];
        const int32_t centered = raw - (dc >> dc_shift);

        dc += ((raw << dc_shift) - dc) >> dc_rate;

        /* Clamped because a full scale swing maps to +32768, one past the
           top of the range. */
        out[i] = (int16_t) std::clamp<int32_t>(centered << ADC_TO_FULL_SCALE, -32768, 32767);
    }

    dc_ = (uint32_t) dc;

    return {out, buf.size()};
}

/*
 * No LOG() here - it would block on the serial queue from an interrupt. The
 * flags are what last_error() reports.
 *
 * An overrun costs one sample and the converter carries on, so it is recorded
 * and the capture left running. A DMA failure is fatal to the stream: the
 * driver has already stopped the conversion, and the clock has to be released
 * too or it goes on triggering a converter that is no longer listening.
 */
void ADCMicrophone::error_cb(ADCDriver *adcp, adcerror_t err) {
    (void) adcp;

    if (instance_ == nullptr) {
        return;
    }

    if ((err & ADC_ERR_OVERFLOW) != 0U) {
        instance_->error_flags_ |= err_mic_overrun;
    }

    if ((err & (ADC_ERR_AWD1 | ADC_ERR_AWD2 | ADC_ERR_AWD3)) != 0U) {
        instance_->error_flags_ |= err_mic_awd;
    }

    if ((err & ADC_ERR_DMAFAILURE) != 0U) {
        instance_->error_flags_ |= err_mic_dma;

        osalSysLockFromISR();
        instance_->stop_capture_i();
        osalSysUnlockFromISR();
    }
}

}// namespace pika::audio
