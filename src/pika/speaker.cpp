#include <pika/speaker.h>

namespace pika::spk {

namespace {

/*
 * Sample clock.
 *
 * The timer runs from STM32_TIMCLK1, which is 260MHz on this board (PCLK1
 * 130MHz doubled, STM32_TIMPRE_ENABLE). ChibiOS derives the prescaler from
 * the requested frequency and asserts that the division is exact, and both
 * steps here are: 260MHz / 4MHz = 65, and 4MHz / 32kHz = 125.
 *
 * Note that the timer counts to tim_interval, so the frequency below cannot
 * simply be sample_rate with an interval of 1: that leaves ARR at 0 and the
 * timer never raises an update event, so no trigger ever reaches the DAC.
 */
constexpr uint32_t tim_frequency = 4000000U;
constexpr uint32_t tim_interval = tim_frequency / sample_rate;

static_assert(tim_frequency % sample_rate == 0U,
              "sample rate must divide the timer frequency exactly");
static_assert(tim_interval > 1U, "interval 0 and 1 produce no update events");
static_assert(STM32_TIMCLK1 % tim_frequency == 0U,
              "timer prescaler must be exact, or the sample rate drifts");
static_assert((STM32_TIMCLK1 / tim_frequency) - 1U <= 0xFFFFU,
              "prescaler does not fit the 16 bit register");

/*
 * TSEL value selecting TIM6's TRGO as the DAC trigger. DAC_TRG() is the
 * identity macro; the low level driver shifts the value into TSEL1 itself.
 * ChibiOS's own H7 DAC test uses the same 5 against GPTD6.
 */
constexpr uint32_t trg_tim6_trgo = 5U;

/*
 * Amplifier turn on time, after /SD goes high and before the first sample is
 * worth listening to. The SSM2305 suppresses its own turn on transient but
 * needs a moment to do it.
 */
constexpr unsigned amp_on_ms = 20U;

/*
 * Full scale amplitude. Samples are mid +/- this, so it stops one count
 * short of the rails and the waveform never clips against them.
 */
constexpr int32_t full_amp = 2047;

/* Amplitude step per sample, so a full scale ramp takes ramp_samples. */
constexpr int32_t ramp_step = (full_amp + (int32_t)ramp_samples - 1) /
                              (int32_t)ramp_samples;

/* remaining_ value meaning "until stop() says otherwise". */
constexpr uint32_t forever = 0xFFFFFFFFU;

/*
 * How long to wait for a release ramp to finish. Only the refill callback
 * ever declares the output silent, so this doubles as the detector for a
 * transport that has stopped moving: the ramp itself is a few milliseconds,
 * and anything beyond a buffer's worth of time means the callbacks are not
 * running.
 */
constexpr unsigned release_wait_ms = 50U;

/*
 * Sine table, 256 entries over a full period, signed so that scaling by an
 * amplitude and ramping to silence both land exactly on mid.
 *
 * It is built at compile time and therefore lives in flash: std::sin is not
 * constexpr, so the Taylor series below stands in for it. The argument is
 * reduced to [-pi, pi] first, where twelve terms are far past the point that
 * matters for a 16 bit result.
 */
constexpr unsigned table_len = 256U;
constexpr double pi = 3.14159265358979323846;

constexpr double taylor_sin(double x) {

  double term = x;
  double sum = x;

  for (int n = 1; n < 12; n++) {
    term *= -x * x / (double)((2 * n) * (2 * n + 1));
    sum += term;
  }

  return sum;
}

struct SineTable {
  int16_t v[table_len];

  constexpr SineTable() : v() {
    for (unsigned i = 0; i < table_len; i++) {
      double a = 2.0 * pi * (double)i / (double)table_len;
      if (a > pi) {
        a -= 2.0 * pi;
      }
      v[i] = (int16_t)(taylor_sin(a) * 32767.0);
    }
  }
};

constexpr SineTable sine{};

/*
 * Sample buffer, placed in AHB SRAM2 through the linker script's .nocache
 * section (0x30004000 on this part).
 *
 * Placement is not a detail, and it fails silently: dacStartConversion()
 * returns MSG_OK, the callbacks even run, and the converter emits nothing
 * but the level it was initialized to.
 *
 *   - The memory has to be in the D2 domain, where DMA1/DMA2 live. Thread
 *     stacks are worse than merely wrong, being in DTCM, which no DMA
 *     controller here can touch.
 *   - It also has to be outside the D-cache, which ChibiOS enables in crt1.c;
 *     otherwise the CPU and the DMA controller look at different data. The
 *     MPU region configured by STM32_NOCACHE_RBAR covers exactly this
 *     section.
 *
 * The buffer is deliberately file-static rather than a member of Speaker:
 * the placement above is the whole point of it, and making it a member would
 * hand that responsibility to whoever declares the object. The cost is that
 * one instance is the supported case, which is what the board has.
 *
 * The DMA runs circular over the whole thing and reports at the halfway
 * point and at the end, so one half is always being read while the other is
 * refilled.
 */
#define DMA_BUF __attribute__((section(".nocache"), aligned(4)))

DMA_BUF dacsample_t wave[2U * half];

/*
 * The instance the callbacks belong to. They are plain function pointers
 * with no user argument, and the buffer above already limits this to one
 * instance, so a file-static pointer is the honest way to bridge them.
 */
Speaker *active = nullptr;

/*
 * MODE1 = 000: normal mode, output buffer enabled, connected to the external
 * pin only. The buffer is what makes the output a low impedance source, and
 * therefore what lets it charge the amplifier's 100nF input capacitor; with
 * it disabled the source impedance turns that capacitor into a lossy divider.
 *
 * CR is left at zero on purpose. In particular DMAUDRIE1 stays off: a DAC
 * underrun raises the interrupt shared with TIM6, and this port compiles no
 * handler for that vector (STM32_TIM6_SUPPRESS_ISR). Underruns are picked up
 * by polling instead, see check_underrun().
 */
const DACConfig dac_cfg = {
  mid,                          /* init, the level the output idles at    */
  DAC_DHRM_12BIT_RIGHT,         /* datamode                               */
  0U,                           /* cr                                     */
  0U                            /* mcr                                    */
};

/*
 * The callback must be NULL, not merely unused: this port defines
 * STM32_TIM6_SUPPRESS_ISR, so no TIM6 handler is compiled and no vector is
 * enabled, but the low level driver still sets DIER_UIE when a callback is
 * present - which would arm an interrupt that nothing services.
 *
 * CR2 selects the update event as TRGO, which is the whole reason the timer
 * is here.
 */
const GPTConfig tim_cfg = {
  tim_frequency,                /* frequency                              */
  nullptr,                      /* callback                               */
  TIM_CR2_MMS_1,                /* cr2, MMS = 010 = update event on TRGO  */
  0U                            /* dier                                   */
};

/* Reads the converter's output register, which has no side effects. */
uint32_t read_dor(DACDriver *dacp) {

  return *(&dacp->params->dac->DOR1 + dacp->params->dataoffset);
}

} /* anonymous namespace */

const DACConversionGroup Speaker::dac_grp_ = {
  1U,                           /* num_channels                           */
  Speaker::fill_cb,             /* end_cb                                 */
  Speaker::error_cb,            /* error_cb                               */
  DAC_TRG(trg_tim6_trgo)        /* trigger                                */
};

/*
 * Deliberately does nothing but take the config.
 *
 * A static instance is constructed before main(), and therefore before
 * halInit(): at that point the MPU region that makes .nocache non-cacheable
 * has not been programmed yet, so writing the sample buffer here would go
 * through the data cache rather than to the memory DMA reads. init() fills
 * it once that is safe.
 */
Speaker::Speaker(const Config &cfg) : cfg_(cfg) {
}

/* enable_on is the level that un-mutes, which is low on this board. */
void Speaker::amp(bool on) {

  bool level = on ? cfg_.enable_on : !cfg_.enable_on;

  palWriteLine(cfg_.enable, level ? PAL_HIGH : PAL_LOW);
}

/*
 * The output reaches mid scale as soon as the DAC is enabled - the low level
 * driver writes the init value to DHR, and with no trigger armed the holding
 * register reaches the output one APB cycle later. What takes time after
 * that is the amplifier's input capacitor charging through 100k, which is
 * why settle_ms is spent here with the amplifier still muted.
 */
bool Speaker::init(void) {

  error_flags_ = 0U;
  ready_ = false;
  streaming_ = false;
  sounding_ = false;

  amp(false);

  active = this;

  if (dacStart(cfg_.dac, &dac_cfg) != HAL_RET_SUCCESS) {
    return false;
  }

  if (gptStart(cfg_.tim, &tim_cfg) != HAL_RET_SUCCESS) {
    dacStop(cfg_.dac);
    return false;
  }

  chThdSleepMilliseconds(cfg_.settle_ms);

  ready_ = true;
  return true;
}

/*
 * Order matters on the way up: the conversion has to be armed before the
 * timer runs, or triggers arrive while DHR still holds the init value and
 * the DMA is only half configured.
 */
bool Speaker::start_stream(void) {

  if (streaming_) {
    return true;
  }

  for (unsigned i = 0; i < 2U * half; i++) {
    wave[i] = mid;
  }

  phase_ = 0U;
  gain_ = 0;
  phase_inc_ = 0U;
  target_gain_ = 0;
  remaining_ = 0U;
  half_count_ = 0U;
  full_count_ = 0U;

  if (dacStartConversion(cfg_.dac, &dac_grp_, wave, 2U * half) !=
      HAL_RET_SUCCESS) {
    return false;
  }

  gptStartContinuous(cfg_.tim, tim_interval);

  streaming_ = true;
  return true;
}

/*
 * And the reverse on the way down, with one extra step: dacStopConversion()
 * is documented as leaving the output register alone, so without the write
 * below the output would freeze at whatever sample the DMA happened to hand
 * over last. That is an arbitrary DC offset, which discharges the input
 * capacitor through the amplifier and pops at the next tone.
 */
void Speaker::stop_stream(void) {

  if (!streaming_) {
    return;
  }

  gptStopTimer(cfg_.tim);
  dacStopConversion(cfg_.dac);
  dacPutChannelX(cfg_.dac, 0U, mid);

  streaming_ = false;
  sounding_ = false;
}

void Speaker::tone(unsigned hz, unsigned ms, uint8_t volume) {

  if (!ready_ || (hz == 0U) || (hz >= sample_rate / 2U)) {
    return;
  }

  if (!streaming_) {
    if (!start_stream()) {
      return;
    }
    amp(true);
    chThdSleepMilliseconds(amp_on_ms);
  }

  /* phase_inc_, target_gain_ and remaining_ are read together by the refill
     callback, so they have to change together. The DAC DMA interrupt runs
     below CORTEX_MAX_KERNEL_PRIORITY, so the lock masks it and the callback
     cannot observe a half updated tone.*/
  chSysLock();
  phase_inc_ = (uint32_t)(((uint64_t)hz << 32) / (uint64_t)sample_rate);
  target_gain_ = ((int32_t)volume * full_amp) / 255;
  remaining_ = (ms != 0U) ?
               (uint32_t)(((uint64_t)ms * sample_rate) / 1000U) : forever;
  sounding_ = true;
  chSysUnlock();
}

bool Speaker::busy(void) const {

  return sounding_;
}

void Speaker::stop(void) {

  if (!streaming_) {
    return;
  }

  /* Cut the tone short rather than silencing it outright: leaving the
     release ramp to run means the output walks back to mid scale instead of
     stepping there, which is the difference between a tone ending and a
     click.*/
  chSysLock();
  if (remaining_ > ramp_samples) {
    remaining_ = ramp_samples;
  }
  chSysUnlock();

  wait_silent(release_wait_ms);

  check_underrun();
  stop_stream();
  amp(false);
}

/*
 * Called from the DMA interrupt at the halfway point and again at the end of
 * the buffer. dacIsBufferComplete() distinguishes the two: it is only true
 * on the transfer complete path, which is the moment the controller wraps to
 * the start, so that is when the second half is free to be rewritten.
 *
 * Both branches can run in a single interrupt entry if the two flags are
 * seen together, which is why this decides from the flag rather than from
 * anything it keeps between calls.
 */
void Speaker::fill_cb(DACDriver *dacp) {

  if (active == nullptr) {
    return;
  }

  if (dacIsBufferComplete(dacp)) {
    active->full_count_ = active->full_count_ + 1U;
    active->fill(&wave[half]);
  }
  else {
    active->half_count_ = active->half_count_ + 1U;
    active->fill(&wave[0]);
  }
}

void Speaker::error_cb(DACDriver *dacp, dacerror_t err) {

  (void)dacp;
  (void)err;

  if (active == nullptr) {
    return;
  }

  /* Nothing but a flag: the low level driver has already stopped the
     conversion and freed the DMA stream from this interrupt, and LOG() would
     take a mutex and block on the serial queue. recover() picks it up.*/
  active->error_flags_ |= err_dma;
  active->streaming_ = false;
  active->sounding_ = false;
}

void Speaker::fill(dacsample_t *dst) {

  uint32_t phase = phase_;
  int32_t gain = gain_;
  int32_t target = target_gain_;
  uint32_t rem = remaining_;
  const uint32_t inc = phase_inc_;

  for (unsigned i = 0; i < half; i++) {

    /* Start the release early enough that the tone reaches silence exactly
       as its duration runs out.*/
    if ((rem != forever) && (rem <= ramp_samples)) {
      target = 0;
    }

    if (gain < target) {
      gain += ramp_step;
      if (gain > target) {
        gain = target;
      }
    }
    else if (gain > target) {
      gain -= ramp_step;
      if (gain < target) {
        gain = target;
      }
    }

    if ((gain == 0) && (target == 0)) {
      /* Silence is mid scale, and the phase resets so the next tone starts
         at a zero crossing.*/
      dst[i] = mid;
      phase = 0U;
    }
    else {
      int32_t s = (int32_t)sine.v[phase >> 24];
      dst[i] = (dacsample_t)((int32_t)mid + ((s * gain) >> 15));
      phase += inc;
    }

    if ((rem != forever) && (rem != 0U)) {
      rem--;
    }
  }

  phase_ = phase;
  gain_ = gain;
  target_gain_ = target;
  remaining_ = rem;

  if ((rem == 0U) && (gain == 0)) {
    sounding_ = false;
  }
}

/*
 * Waits for the release ramp to reach silence, but not forever: sounding_ is
 * cleared by the refill callback and by nothing else, so a DMA path that
 * moves nothing would leave a caller spinning here for good. Treating the
 * timeout as a transport failure keeps a silent fault from becoming a hang.
 */
bool Speaker::wait_silent(unsigned ms) {

  systime_t start = chVTGetSystemTimeX();

  while (sounding_) {
    if (TIME_I2MS(chVTTimeElapsedSinceX(start)) > ms) {
      chSysLock();
      sounding_ = false;
      gain_ = 0;
      target_gain_ = 0;
      remaining_ = 0U;
      chSysUnlock();

      error_flags_ |= err_no_dma;
      return false;
    }
    chThdSleepMilliseconds(2);
  }

  return true;
}

/*
 * An underrun clears DMAEN in the hardware, so the audio stops dead. The low
 * level driver never notices: its error path only covers the DMA
 * controller's own transfer and mode errors, not the converter's complaint
 * about being starved.
 */
void Speaker::check_underrun(void) {

  DAC_TypeDef *dac = cfg_.dac->params->dac;
  uint32_t udr = DAC_SR_DMAUDR1 << cfg_.dac->params->regshift;

  if ((dac->SR & udr) != 0U) {
    dac->SR = udr;
    error_flags_ |= err_underrun;
  }
}

bool Speaker::recover(void) {

  error_flags_ = 0U;

  if (!ready_) {
    return false;
  }

  amp(false);
  stop_stream();
  dacPutChannelX(cfg_.dac, 0U, mid);

  return true;
}

/*
 * Everything checked here is upstream of the amplifier, so it runs with the
 * amplifier muted and makes no sound. The three checks answer three
 * different questions, and only the last of them distinguishes a converter
 * that is receiving samples from one that merely has a DMA controller
 * running next to it.
 */
bool Speaker::verify(void) {

  if (!ready_) {
    return false;
  }

  error_flags_ &= ~(err_no_dma | err_no_output);

  bool was_streaming = streaming_;

  amp(false);

  if (!start_stream()) {
    error_flags_ |= err_no_dma;
    return false;
  }

  /* A mid band tone at full scale, so the output has as much room to move as
     it ever will.*/
  chSysLock();
  phase_inc_ = (uint32_t)(((uint64_t)1000U << 32) / (uint64_t)sample_rate);
  target_gain_ = full_amp;
  remaining_ = forever;
  sounding_ = true;
  chSysUnlock();

  /* 1. The refill callbacks run at all. A full transfer completes every
        2 * half samples, so 100ms is worth about six of them.*/
  uint32_t full_before = full_count_;
  chThdSleepMilliseconds(100);
  uint32_t callbacks = full_count_ - full_before;

  /* 2. The DMA controller is moving, not just interrupting: NDTR counts the
        transfers it has left, so it has to change.*/
  size_t ndtr_before = dmaStreamGetTransactionSize(cfg_.dac->dma);
  chThdSleepMilliseconds(1);
  size_t ndtr_after = dmaStreamGetTransactionSize(cfg_.dac->dma);

  /* 3. The converter is following the data. This is the readback: a wrong
        trigger, a stalled holding register or a DMA aimed at the wrong
        register all leave the output pinned at one value, while return codes
        and error flags stay clean.*/
  uint32_t dor_min = 0xFFFFFFFFU;
  uint32_t dor_max = 0U;

  for (unsigned i = 0; i < 50U; i++) {
    uint32_t d = read_dor(cfg_.dac);
    if (d < dor_min) {
      dor_min = d;
    }
    if (d > dor_max) {
      dor_max = d;
    }
    chThdSleepMicroseconds(100);
  }

  check_underrun();

  chSysLock();
  target_gain_ = 0;
  remaining_ = ramp_samples;
  chSysUnlock();

  wait_silent(release_wait_ms);

  if (!was_streaming) {
    stop_stream();
  }

  if ((callbacks == 0U) || (ndtr_before == ndtr_after)) {
    error_flags_ |= err_no_dma;
  }

  if (dor_min == dor_max) {
    error_flags_ |= err_no_output;
  }

  return error_flags_ == 0U;
}

/*
 * The companion to verify(): a silent stream still moves samples, so this
 * separates "the DMA stalled" from "the DMA runs but the samples are wrong",
 * which the moving-output test on its own cannot tell apart.
 */
bool Speaker::verify_dc(void) {

  if (!ready_) {
    return false;
  }

  bool was_streaming = streaming_;

  amp(false);

  if (!start_stream()) {
    error_flags_ |= err_no_dma;
    return false;
  }

  chThdSleepMilliseconds(20);

  size_t ndtr_before = dmaStreamGetTransactionSize(cfg_.dac->dma);
  chThdSleepMilliseconds(1);
  size_t ndtr_after = dmaStreamGetTransactionSize(cfg_.dac->dma);

  uint32_t dor = read_dor(cfg_.dac);

  check_underrun();

  if (!was_streaming) {
    stop_stream();
  }

  if (ndtr_before == ndtr_after) {
    error_flags_ |= err_no_dma;
    return false;
  }

  return dor == mid;
}

} /* namespace pika::spk */
