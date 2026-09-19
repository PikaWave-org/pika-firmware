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
 * MELPe 2400 bench: the two questions the host checks cannot answer. Does a
 * frame encode inside its own 22.5ms on this part, and does the Cortex-M7 -O2
 * build produce the same bits as the host build compared against upstream.
 *
 * Fed from a clip, not the microphone: the converters run at 32kHz, so a mic
 * fed bench would measure the resampler too and have nothing on the host to
 * equal.
 *
 * To compare the CRC, encode the same samples on the host - and they have to
 * be the same samples. wav2cpp.py does its own resampling and normalises to
 * -1 dBFS, so an ffmpeg extraction of meow.mp3 is a different signal and
 * gives a different, perfectly correct, CRC. Dump the generated array from
 * build/generated_src/pika/audio/sounds/meow8k.cpp to s16le, run it through
 * tools/melpe24_driver.c, and CRC the first melp_frames * 7 bytes.
 */
namespace {

constexpr size_t melp_frame = 180U;/* samples, 22.5ms at 8kHz            */
constexpr size_t melp_bytes = 7U;  /* 54 bits, packed 8 to the byte      */
constexpr size_t melp_frames = pika::audio::meow8k_len / melp_frame;

/* Kept whole so a mismatch can be diffed frame by frame over SWD. */
uint8_t melp_bits[melp_frames * melp_bytes];

/* Static, so 360 bytes of scratch do not blur the stack measurement. */
int16_t melp_pcm[melp_frame];

/* Bitwise, no table: 308 bytes a pass does not justify 1KB of flash. */
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

/* melpe_i24() per pass, so both start from the same codec state. It is not a
   full reset, which is why the passes are reported separately. */
void encode_pass() {
  enc_stats.reset();
  melpe_i24();

  for (size_t i = 0; i < melp_frames; i++) {
    /* melpe_a24() does not write its input; analysis() just takes a
       non-const pointer. */
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

/* chSysGetRealtimeCounterX() is DWT->CYCCNT, and on this M7 the DWT comes up
   locked, so the port's own enable silently does nothing and every reading is
   zero. Unlock, then enable, then check it actually moves. */
bool dwt_init() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55U;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  const uint32_t a = DWT->CYCCNT;
  return DWT->CYCCNT != a;
}

/* Outside every timed region: LOG() at 115200 baud would dominate it. */
void report(const char *what, const Stats &s) {
  LOG("melpe24: %s n=%u min=%uus mean=%uus max=%uus (%u%% of 22500us)", what,
      (unsigned)s.n, (unsigned)to_us(s.min), (unsigned)to_us(s.mean()),
      (unsigned)to_us(s.max), (unsigned)(to_us(s.max) * 100U / 22500U));
}

}// namespace

/* 8K, and the thread reports what it actually used. */
static THD_WORKING_AREA(waBench, 8192);
static THD_FUNCTION(bench, arg) {
  (void)arg;
  chRegSetThreadName("melpe-bench");

  /* tools/hw check flashes, resets, and only then opens the VCOM, so anything
     logged in the first second or so of a boot is never captured. */
  chThdSleepMilliseconds(3000);

  LOG("melpe24: %u frames of %u samples, core %u Hz, cycle counter %s",
      (unsigned)melp_frames, (unsigned)melp_frame, (unsigned)STM32_CORE_CK,
      dwt_init() ? "running" : "DEAD - timings below are meaningless");

  /* Twice: the first pass pays for cold I-cache over 190KB of codec, which is
     the margin at the first frame after a press; the second is steady state. */
  encode_pass();
  report("encode pass1", enc_stats);
  const uint32_t crc1 = crc32(melp_bits, sizeof(melp_bits));

  encode_pass();
  report("encode pass2", enc_stats);
  const uint32_t crc2 = crc32(melp_bits, sizeof(melp_bits));

  /* The two CRCs differ, and should: melpe_i24() clears hpspeech, the pitch
     tracks and the synthesis carry, but not the one shot firstTime statics,
     so pass 2 does not start where pass 1 did. Only pass 1 begins from a cold
     boot, so it is the one to compare against a fresh host process. */
  LOG("melpe24: crc pass1=%08x pass2=%08x (pass1 is the host comparable one)",
      (unsigned)crc1, (unsigned)crc2);

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
