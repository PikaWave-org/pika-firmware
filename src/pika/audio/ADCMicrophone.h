/*
 * Driver for the microphone: an analog preamp into ADC1, read by DMA.
 *
 * The capture-side mirror of DACSpeaker, and shaped like it. The wiring comes
 * in as a Config filled from the board's BOARD_MIC_* and LINE_MIC_* defines,
 * so the driver depends on no board header.
 *
 * Samples come in at a fixed rate: the SampleClock's TRGO paces the ADC, and
 * DMA fills a two-half circular buffer whose transfer callback hands each
 * finished half to an AudioConsumer. The direction is the only real difference
 * from the speaker - there the callback asks a source to fill the buffer, here
 * it gives a full one away.
 *
 * @note  The sample buffer is shared between all instances, because where it
 *        sits in memory is the driver's business and not a caller's. One
 *        instance is therefore the supported case - see the .cpp.
 */

#pragma once

#include "SampleClock.h"
#include "audio_io.h"

#include "hal.h"

#include <cstdint>
#include <span>

namespace pika::audio {

/**
 * @brief   Error flags reported by last_error().
 */
enum : uint32_t {
    err_mic_dma = 1U << 0,     /**< DMA transfer error, the stream was freed.  */
    err_mic_overrun = 1U << 1, /**< A conversion landed before the last was
                                    read; the sample is lost.                  */
};

class ADCMicrophone {
public:
    /**
     * @brief   How the microphone is wired up, taken from the board header.
     */
    struct Config {
        ADCDriver *adc{};     /**< Converter reading the preamp output.         */
        SampleClock *clock{}; /**< Sample clock, shared with the speaker.       */
        ioline_t shutdown{};  /**< Preamp shutdown pin, active high.            */
        uint32_t channel{};   /**< ADC input the preamp is wired to.            */
        uint32_t settle_ms{}; /**< Bias settling after the preamp wakes up.     */
    };

    explicit ADCMicrophone(const Config &cfg) { cfg_ = cfg; }

    /**
     * @brief   Starts the ADC and builds the conversion group.
     * @details Leaves the preamp shut down and the clock stopped. Nothing is
     *          converted until start_capture().
     * @return  false if the ADC or the sample clock could not be started.
     */
    bool init();

    /**
     * @brief   Powers the preamp up and begins capturing.
     * @return  false if init() has not succeeded, capture is already running,
     *          or the speaker holds the sample clock.
     */
    bool start_capture();

    /** @brief  Stops the converter and shuts the preamp down again. */
    void stop_capture();

    /** @brief  True between a successful start_capture() and stop_capture(). */
    [[nodiscard]] bool capturing() const { return running_; }

    /** @brief  Error flags since the last init(), 0 if none. */
    [[nodiscard]] uint32_t last_error() const { return error_flags_; }

    [[nodiscard]] uint32_t sample_rate() const { return SampleClock::rate; }

    void add_consumer(AudioConsumer &consumer) {
        consumers_.add(consumer);
    }

    void remove_consumer(AudioConsumer &consumer) {
        consumers_.remove(consumer);
    }
private:
    static ADCMicrophone *instance_;

    static constexpr int ADC_BIT_DEPTH = 12;
    static constexpr uint32_t ADC_VALUE_MID = 1U << (ADC_BIT_DEPTH - 1);

    /* 12 bit codes to signed 16 bit full scale is a shift of four. */
    static constexpr int ADC_TO_FULL_SCALE = 16 - ADC_BIT_DEPTH;

    /*
     * EXTSEL value selecting TIM6's TRGO as the ADC trigger. Not the same
     * number the DAC uses for the same timer - the two converters have
     * separate trigger tables - and RM0468 is the only place either appears.
     */
    static constexpr uint32_t TRG_TIM6_TRGO = 13U;

    /*
     * Sampling time. At the 20MHz ADC clock 64.5 + 7.5 cycles is 3.6us against
     * a 31.25us sample period, so the long setting costs nothing and leaves
     * the sample-and-hold ample time to track the preamp through whatever
     * series resistance is on the pin.
     */
    static constexpr uint32_t SMP_TIME = ADC_SMPR_SMP_64P5;

    /* Samples per buffer half, i.e. 8ms of audio and a 125Hz drain callback. The
       same figure as the speaker's, so a recording and a playback move in blocks
       of the same size. */
    static constexpr unsigned mic_buffer_half_len = 256U;
    static __attribute__((section(".nocache"), aligned(4))) adcsample_t mic_buffer[2U * mic_buffer_half_len];

    static void fill_cb(ADCDriver *adcp);
    static void error_cb(ADCDriver *adcp, adcerror_t err);

    IntrusiveList<AudioConsumer> consumers_;

    void preamp(bool on);

    /* I-class twin of stop_capture(), for use from the DMA callback. */
    void stop_capture_i();

    /* Rewrites raw codes in place as signed full scale samples, taking the
       bias to be mid scale. Returns the block as the consumer sees it. */
    std::span<const int16_t> convert(std::span<adcsample_t> buf);

    Config cfg_{};

    /*
     * Filled in init() rather than being a constant initializer like the
     * speaker's, because which input the preamp is wired to arrives in the
     * Config and decides three of the fields below.
     */
    ADCConversionGroup adc_grp_{};

    ADCConfig adc_cfg_{};

    volatile uint32_t error_flags_ = 0U; /**< Also set from the callback.  */
    bool ready_ = false;                 /**< init() succeeded.            */
    bool running_ = false;
};

}// namespace pika::audio
