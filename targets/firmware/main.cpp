#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <pika/log.h>
#include <pika/speaker.h>
#include <pika/st75160.h>
#include <pika/ublox.h>

/* The panel, wired up as the board header describes it. */
static const pika::lcd::St75160::Config lcd_cfg = {
  &BOARD_LCD_I2C, BOARD_LCD_I2C_ADDR,
  LINE_LCD_RST, LINE_LCD_BKLT,
  LINE_LCD_SCL, LINE_LCD_SDA, BOARD_LCD_I2C_PINMODE
};

static pika::lcd::St75160 lcd{lcd_cfg};

/* The speaker, likewise. */
static const pika::spk::Config spk_cfg = {
  &BOARD_SPK_DAC, &BOARD_SPK_TIMER,
  LINE_SPK_EN, BOARD_SPK_EN_ON, BOARD_SPK_SETTLE_MS
};

static pika::spk::Speaker spk{spk_cfg};

/* The GNSS receiver, at one navigation solution per second. */
static const pika::gnss::Ublox::Config gnss_cfg = {
  &BOARD_GNSS_SERIAL, BOARD_GNSS_BAUD_BOOT, BOARD_GNSS_BAUD_RUN, 1000U
};

static pika::gnss::Ublox gnss{gnss_cfg};

/* Last measured frame time, in milliseconds. */
uint32_t flush_ms;

/*
 * Bring-up image: a border, a checkerboard block, a diagonal and a text
 * line. Between them these show orientation, mirroring, the pixel grid and
 * any dead rows or columns.
 */
static void draw_test_image(void) {

  lcd.clear();
  lcd.frame(0, 0, lcd.width, lcd.height, true);

  for (int y = 0; y < 24; y += 8) {
    for (int x = 0; x < 32; x += 8) {
      if (((x ^ y) & 8) == 0) {
        lcd.rect(4 + x, 68 + y, 8, 8, true);
      }
    }
  }

  for (int i = 0; i < lcd.height; i++) {
    lcd.pixel(lcd.width - lcd.height + i, i, true);
  }

  lcd.text(4, 6, BOARD_NAME);
}

/*
 * Heartbeat: a line on the debug UART and a refreshed counter on the LCD,
 * once per second.
 */
static THD_WORKING_AREA(waHeartbeat, 1024);
static THD_FUNCTION(heartbeat, arg) {

  (void)arg;
  chRegSetThreadName("heartbeat");

  uint32_t beat = 0;

  while (true) {
    LOG("heartbeat %u", (unsigned)beat);

    char line[24];
    chsnprintf(line, sizeof line, "beat %u", beat);
    lcd.rect(4, 20, 8 * 12, 8, false);
    lcd.text(4, 20, line);

    systime_t start = chVTGetSystemTimeX();
    bool ok = lcd.flush();
    sysinterval_t took = chVTTimeElapsedSinceX(start);
    flush_ms = (uint32_t)TIME_I2MS(took);

    if (!ok) {
      LOG("lcd: flush failed, i2c error 0x%08x", (unsigned)lcd.last_error());
    }
    else if (beat == 0) {
      LOG("lcd: flush took %ums", (unsigned)flush_ms);
    }

    beat++;
    chThdSleepMilliseconds(1000);
  }
}

/*
 * Formats a 1e-7 degree coordinate as plain decimal degrees. chprintf() has
 * no floating point support in this build, and the sign has to be taken off
 * before the split so that -0.5 degrees does not come out as "-0.-5000000".
 */
static void format_deg(char *out, size_t len, int32_t deg_1e7) {

  bool neg = deg_1e7 < 0;
  uint32_t mag = (uint32_t)(neg ? -(int64_t)deg_1e7 : (int64_t)deg_1e7);

  chsnprintf(out, len, "%s%u.%07u", neg ? "-" : "",
             (unsigned)(mag / 10000000U), (unsigned)(mag % 10000000U));
}

/*
 * GNSS reader. poll() blocks on the serial driver, so this thread spends
 * almost all its time asleep and wakes once per navigation solution.
 *
 * The log line carries the link counters alongside the fix because the two
 * failure modes look identical otherwise: a receiver with no sky view and a
 * receiver that is not talking at all both report fix=0 sv=0. Bytes and
 * frames climbing tells them apart.
 */
static THD_WORKING_AREA(waGnss, 2048);
static THD_FUNCTION(gnss_reader, arg) {

  (void)arg;
  chRegSetThreadName("gnss");

  uint32_t silent = 0;

  while (true) {
    if (!gnss.poll(TIME_MS2I(1500))) {
      pika::gnss::Ublox::Stats s = gnss.stats();
      silent++;
      LOG("gnss: silent for %us, bytes %u frames %u csum %u resync %u",
          (unsigned)(silent * 3U / 2U), (unsigned)s.bytes_rx,
          (unsigned)s.frames_ok, (unsigned)s.checksum_errors,
          (unsigned)s.resyncs);
      continue;
    }

    silent = 0;

    pika::gnss::ubx_nav_pvt pvt = gnss.nav_pvt();
    pika::gnss::Ublox::Stats s = gnss.stats();

    /* A "!" marks a solution the receiver itself does not trust. */
    bool fix_ok = (pvt.flags & pika::gnss::flags_gnss_fix_ok) != 0U;

    char lat[16], lon[16];
    format_deg(lat, sizeof lat, pvt.lat);
    format_deg(lon, sizeof lon, pvt.lon);

    LOG("gnss: fix=%u%s sv=%u lat=%s lon=%s alt=%dm hacc=%um pdop=%u.%02u "
        "frames=%u csum=%u",
        (unsigned)pvt.fixType, fix_ok ? "" : "!", (unsigned)pvt.numSV,
        lat, lon, (int)(pvt.hMSL / 1000), (unsigned)(pvt.hAcc / 1000U),
        (unsigned)(pvt.pDOP / 100U), (unsigned)(pvt.pDOP % 100U),
        (unsigned)s.frames_ok, (unsigned)s.checksum_errors);

    /* Date, time and the UTC offset all have to be resolved before the
       timestamp is worth printing.*/
    constexpr uint8_t time_ready = pika::gnss::valid_date |
                                   pika::gnss::valid_time |
                                   pika::gnss::valid_fully_resolved;

    if ((pvt.valid & time_ready) == time_ready) {
      LOG("gnss: utc %04u-%02u-%02u %02u:%02u:%02u", (unsigned)pvt.year,
          (unsigned)pvt.month, (unsigned)pvt.day, (unsigned)pvt.hour,
          (unsigned)pvt.min, (unsigned)pvt.sec);
    }
  }
}

int main(void) {

  halInit();
  chSysInit();

  pika::log_init();
  LOG("\r\n" BOARD_NAME " starting");

  lcd.backlight(true);

  bool ok = lcd.init();
  LOG("lcd: init %s, i2c error 0x%08x", ok ? "ok" : "failed",
      (unsigned)lcd.last_error());

  if (ok) {
    draw_test_image();
    if (!lcd.flush()) {
      LOG("lcd: flush failed, i2c error 0x%08x", (unsigned)lcd.last_error());
    }
  }

  /* Speaker. verify() runs with the amplifier muted, so it makes no sound:
     it is there to catch a DMA path that moves nothing, which otherwise
     looks exactly like a working one until you put an ear to the speaker.*/
  bool spk_ok = spk.init();
  LOG("spk: init %s", spk_ok ? "ok" : "failed");

  if (spk_ok) {
    bool dc = spk.verify_dc();
    bool moved = spk.verify();
    LOG("spk: dc %s, verify %s, err 0x%08x", dc ? "ok" : "FAILED",
        moved ? "ok" : "FAILED", (unsigned)spk.last_error());

    /* 1kHz for a second, 20dB below full scale. The volume byte is linear in
       amplitude, so 255 * 10^(-20/20) rounds to 26, which lands on -19.9dB -
       the byte is about a third of a dB per step down here, so 20.0 exactly
       is not on the grid.

       stop() rides out the release ramp before muting, so the sleep only has
       to cover the tone itself.*/
    spk.tone(1000, 1000, 26);
    chThdSleepMilliseconds(1000);
    spk.stop();
  }

  bool gnss_ok = gnss.init();
  LOG("gnss: init %s, error %u", gnss_ok ? "ok" : "failed",
      (unsigned)gnss.last_error());

  if (gnss_ok) {
    chThdCreateStatic(waGnss, sizeof(waGnss), NORMALPRIO, gnss_reader,
                      nullptr);
  }

  chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat,
                    nullptr);

  while (true) {
    chThdSleepMilliseconds(1000);
  }
}
