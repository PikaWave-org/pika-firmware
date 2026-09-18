#include "ch.h"
#include "hal.h"

#include <melpe/melpe.h>
#include <pika/audio/sounds/meow8k.h>
#include <pika/log.h>

/*
 * Test image: same board, different firmware. Flashes the backlight fast so
 * the running image can be told apart from the normal firmware at a glance,
 * and says so on the debug UART for the same reason.
 *
 * The working area has room for chvprintf(), which the logger calls on this
 * thread's stack.
 */
static THD_WORKING_AREA(waHeartbeat, 512);
static THD_FUNCTION(heartbeat, arg) {
  (void)arg;
  chRegSetThreadName("heartbeat");

  uint32_t beat = 0;

  while (true) {
    palToggleLine(LINE_LCD_BKLT);
    if ((beat % 10U) == 0U) {
      LOG("heartbeat %u", (unsigned)(beat / 10U));
    }
    beat++;
    chThdSleepMilliseconds(100);
  }
}

/*
 * MELPe 2400 bench.
 *
 * The one question the host checks cannot answer: does a frame encode inside
 * its own 22.5ms on this part, and does the Cortex-M7 -O2 build produce the
 * same bits as the host build that was compared against upstream. Both are
 * answered here, from flash, with nobody at the keyboard.
 *
 * It is fed from a clip rather than from the microphone on purpose. The
 * converters on this board run at 32kHz, so a microphone fed bench would be
 * measuring the resampler as much as the codec, and it could not be compared
 * against anything on the host. meow8k is the same 8kHz material
 * tools/melpe-check encodes, so the CRC below has something to be equal to.
 */
namespace {

constexpr size_t melp_frame = 180U;    /* samples, 22.5ms at 8kHz */
constexpr size_t melp_bytes = 7U;      /* 54 bits, packed 8 to the byte */
constexpr size_t melp_frames = pika::audio::meow8k_len / melp_frame;

/* Kept whole rather than CRCed on the fly so a mismatch can be diffed frame
   by frame over SWD - arm-none-eabi-nm -S gives the address. */
uint8_t melp_bits[melp_frames * melp_bytes];

/* Scratch for one decoded frame. A member rather than a local because the
   codec's own stack appetite is the thing being measured and 360 bytes of
   scratch riding on top of it would blur the answer. */
int16_t melp_pcm[melp_frame];

/* Bitwise, no table: 308 bytes once per pass does not justify 1KB of flash. */
uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = 0xFFFFFFFFU;

  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
    }
  }
  return ~crc;
}

/* Cycles to microseconds at the real core clock, rather than a hardcoded 520. */
uint32_t to_us(uint32_t cycles) {
  return (uint32_t)((uint64_t)cycles * 1000000ULL / (uint64_t)STM32_CORE_CK);
}

struct Stats {
  uint32_t min, max;
  uint64_t sum;
  uint32_t n;

  void reset() {
    min = 0xFFFFFFFFU;
    max = 0U;
    sum = 0U;
    n = 0U;
  }
  void add(uint32_t c) {
    if (c < min) { min = c; }
    if (c > max) { max = c; }
    sum += c;
    n++;
  }
  uint32_t mean() const { return n ? (uint32_t)(sum / n) : 0U; }
};

Stats enc_stats, dec_stats;

/*
 * One encode pass over the whole clip, timed per frame.
 *
 * melpe_i24() is called per pass so both passes start from the same codec
 * state - it clears hpspeech, the pitch tracks and the synthesis carry. It is
 * not a full reset (the one shot firstTime statics survive), which is exactly
 * why the two passes are reported separately rather than averaged.
 */
void encode_pass() {
  enc_stats.reset();
  melpe_i24();

  for (size_t i = 0; i < melp_frames; i++) {
    /* The clip is const and in flash; melpe_a24() does not write its input,
       but analysis() takes a non-const pointer, so the cast is the API's
       fault rather than a claim about the data. */
    short *in = (short *)(uintptr_t)&pika::audio::meow8k[i * melp_frame];
    const rtcnt_t t0 = chSysGetRealtimeCounterX();

    melpe_a24(&melp_bits[i * melp_bytes], in);
    enc_stats.add((uint32_t)(chSysGetRealtimeCounterX() - t0));
  }
}

void decode_pass() {
  dec_stats.reset();
  melpe_i24();

  for (size_t i = 0; i < melp_frames; i++) {
    const rtcnt_t t0 = chSysGetRealtimeCounterX();

    melpe_s24(melp_pcm, &melp_bits[i * melp_bytes]);
    dec_stats.add((uint32_t)(chSysGetRealtimeCounterX() - t0));
  }
}

/* Unused bytes of a working area, from the 0x55 CH_DBG_FILL_THREADS leaves. */
size_t stack_free(const void *wa, size_t size) {
  const uint8_t *p = (const uint8_t *)wa;
  size_t i = 0;

  while (i < size && p[i] == CH_DBG_STACK_FILL_VALUE) {
    i++;
  }
  return i;
}

/* Logging is deliberately outside every timed region: LOG() is chprintf over a
   115200 baud UART and would dominate what it was measuring. */
void report(const char *what, const Stats &s) {
  LOG("melpe24: %s n=%u min=%uus mean=%uus max=%uus (%u%% of 22500us)", what,
      (unsigned)s.n, (unsigned)to_us(s.min), (unsigned)to_us(s.mean()),
      (unsigned)to_us(s.max), (unsigned)(to_us(s.max) * 100U / 22500U));
}

}// namespace

/* 8K provisionally: the codec's stack appetite is undocumented and this thread
   reports what it actually used, which is how the number gets settled. */
static THD_WORKING_AREA(waBench, 8192);
static THD_FUNCTION(bench, arg) {
  (void)arg;
  chRegSetThreadName("melpe-bench");

  LOG("melpe24: %u frames of %u samples, core %u Hz", (unsigned)melp_frames,
      (unsigned)melp_frame, (unsigned)STM32_CORE_CK);

  /* Twice, and reported separately. The first pass pays for cold I-cache and
     flash prefetch on 190KB of codec, which is the margin that applies to the
     first frame after a button press; the second is the steady state. */
  encode_pass();
  report("encode pass1", enc_stats);
  const uint32_t crc1 = crc32(melp_bits, sizeof(melp_bits));

  encode_pass();
  report("encode pass2", enc_stats);
  const uint32_t crc2 = crc32(melp_bits, sizeof(melp_bits));

  LOG("melpe24: crc=%08x %s", (unsigned)crc1,
      crc1 == crc2 ? "(stable)" : "(UNSTABLE between passes)");

  decode_pass();
  report("decode", dec_stats);

  LOG("melpe24: stack free %u of %u bytes",
      (unsigned)stack_free(waBench, sizeof(waBench)), (unsigned)sizeof(waBench));
  LOG("melpe24: done");

  while (true) {
    chThdSleepMilliseconds(1000);
  }
}

int main() {
  halInit();
  chSysInit();

  pika::log_init();
  LOG("\r\n" BOARD_NAME " test image starting");

  chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat,
                    nullptr);
  chThdCreateStatic(waBench, sizeof(waBench), NORMALPRIO + 1, bench, nullptr);

  while (true) {
    chThdSleepMilliseconds(1000);
  }
}
