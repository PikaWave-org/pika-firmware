/*
 * Driver for the speaker: DAC1_OUT1 into an SSM2305 class D amplifier.
 *
 * The wiring - the DAC channel, the timer pacing it, the amplifier's
 * shutdown pin and the settling time its input network needs - is passed in
 * as a Config at construction, so the driver itself does not depend on any
 * board header. The board's BOARD_SPK_* and LINE_SPK_* definitions are what
 * the caller fills the Config from.
 *
 * Samples go out at a fixed rate: the timer's TRGO paces the DAC and DMA
 * feeds it from a two-half circular buffer, refilled a half at a time from
 * the transfer callback. Tones are generated there by a phase accumulator,
 * so any frequency works without a per-frequency table, and swapping in
 * recorded audio later means replacing fill() and nothing else.
 *
 * Usage:
 *
 *   static const pika::spk::Config spk_cfg = {
 *     &BOARD_SPK_DAC, &BOARD_SPK_TIMER,
 *     LINE_SPK_EN, BOARD_SPK_EN_ON, BOARD_SPK_SETTLE_MS
 *   };
 *   static pika::spk::Speaker spk{spk_cfg};
 *   ...
 *   spk.tone(1000, 200);
 *   while (spk.busy()) chThdSleepMilliseconds(5);
 *
 * @note  The sample buffer is shared between all instances: it has to sit in
 *        the one region of memory the DMA controller can reach and the
 *        D-cache leaves alone, and that placement is not something a caller
 *        can be relied on to get right. One instance is therefore the
 *        supported case - see the buffer comment in the .cpp.
 */

#pragma once

#include <cstdint>

#include "hal.h"

namespace pika::spk {

/*
 * Sample rate. Nothing filters the DAC output before the amplifier, so the
 * zero order hold images sit at multiples of this rate around the tone;
 * 32kHz keeps the nearest of them well clear of anything audible.
 *
 * It also has to come out of the timer exactly, which it does: see the
 * prescaler arithmetic in the .cpp.
 */
constexpr unsigned sample_rate = 32000U;

/* Samples per buffer half, i.e. 8ms of audio and a 125Hz refill callback. */
constexpr unsigned half = 256U;

/* 12 bit mid scale, the DC level the output idles at. */
constexpr uint16_t mid = 2048U;

/* Attack and release ramp, in samples. About 3ms, see tone(). */
constexpr unsigned ramp_samples = 96U;

/**
 * @brief   How the speaker is wired up, taken from the board header.
 */
struct Config {
  DACDriver *dac;       /**< DAC channel driving the amplifier input.       */
  GPTDriver *tim;       /**< Timer whose TRGO paces the DAC.                */
  ioline_t   enable;    /**< Amplifier shutdown pin.                        */
  bool       enable_on; /**< Level on that pin that un-mutes the amplifier. */
  unsigned   settle_ms; /**< Output settling before un-muting, see below.    */
};

/**
 * @brief   Error flags reported by last_error().
 */
enum : uint32_t {
  err_dma       = 1U << 0, /**< DMA transfer error, the stream was freed.   */
  err_underrun  = 1U << 1, /**< DAC underrun, the hardware stopped the DMA. */
  err_no_dma    = 1U << 2, /**< verify(): the DMA never moved anything.     */
  err_no_output = 1U << 3  /**< verify(): the converter output never moved. */
};

class Speaker {
public:

  explicit Speaker(const Config &cfg);

  /**
   * @brief   Starts the DAC and brings the output to its idle level.
   * @details Leaves the amplifier muted and the sample clock stopped. The
   *          output sits at mid scale from here on, which is what lets a
   *          later tone start without waiting for the input capacitor
   *          again.
   * @return  false if the DAC could not be started.
   */
  bool init(void);

  /**
   * @brief   Starts a tone, returning immediately.
   * @details The tone is shaped with a short attack and release so it does
   *          not step the amplifier input; see ramp_samples. Calling this
   *          while another tone sounds replaces it.
   * @param   hz      Frequency, ignored unless below half the sample rate.
   * @param   ms      Duration. 0 sounds until stop() is called.
   * @param   volume  Amplitude, 255 is full scale.
   */
  void tone(unsigned hz, unsigned ms, uint8_t volume = 255U);

  /** @brief  True while a tone is still sounding. */
  bool busy(void) const;

  /**
   * @brief   Ends the current tone and mutes the amplifier.
   * @details Rides out the release ramp first, so this does not click.
   */
  void stop(void);

  /**
   * @brief   Checks that samples actually reach the converter.
   * @details The point of this is to tell a transport that moves data from
   *          one that silently moves none: every return code and error flag
   *          looks identical either way, which is how a broken I2C DMA path
   *          went unnoticed through a whole LCD bring-up. Run it after any
   *          change to the transport.
   * @note    Everything it checks is upstream of the amplifier, so it runs
   *          with the amplifier muted and makes no sound.
   * @return  true if the callbacks ran, the DMA advanced and the converter
   *          output moved. See last_error() for which of those failed.
   */
  bool verify(void);

  /**
   * @brief   Checks that a silent output holds mid scale.
   * @details Separates "the DMA is stalled" from "the DMA runs but the
   *          samples are wrong", which verify() alone conflates.
   */
  bool verify_dc(void);

  /** @brief  Error flags since the last init(), 0 if none. */
  uint32_t last_error(void) const { return error_flags_; }

  /** @brief  Clears the error flags and, if the DMA died, re-arms it. */
  bool recover(void);

private:

  /* The DMA transfer and error callbacks, and the conversion group holding
     them. The group is a member so that its initializer may name them while
     they stay private.*/
  static void fill_cb(DACDriver *dacp);
  static void error_cb(DACDriver *dacp, dacerror_t err);
  static const DACConversionGroup dac_grp_;

  void fill(dacsample_t *dst);
  bool wait_silent(unsigned ms);
  bool start_stream(void);
  void stop_stream(void);
  void amp(bool on);
  void check_underrun(void);

  Config cfg_;

  /* Written by the caller under chSysLock(), read by the callback. */
  uint32_t phase_inc_ = 0U;     /**< NCO step per sample.                   */
  int32_t  target_gain_ = 0;    /**< Amplitude the ramp is heading for.     */
  uint32_t remaining_ = 0U;     /**< Samples left, ~0 means until stop().   */

  /* Callback state, touched by the callback only once streaming. */
  uint32_t phase_ = 0U;
  int32_t  gain_ = 0;           /**< Current ramped amplitude.              */

  volatile uint32_t half_count_ = 0U;
  volatile uint32_t full_count_ = 0U;
  volatile bool sounding_ = false;

  volatile uint32_t error_flags_ = 0U;   /**< Also set from the callback.  */
  bool ready_ = false;          /**< init() succeeded, DAC holds mid.       */
  bool streaming_ = false;      /**< Conversion and sample clock running.   */
};

} /* namespace pika::spk */

