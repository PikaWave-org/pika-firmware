#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <pika/log.h>
#include <pika/speaker.h>
#include <pika/st75160.h>

/* The panel, wired up as the board header describes it. */
static const pika::lcd::Config lcd_cfg = {
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

/* Last measured frame time, in milliseconds. */
uint32_t flush_ms;

/*
 * Bring-up image: a border, a checkerboard block, a diagonal and a text
 * line. Between them these show orientation, mirroring, the pixel grid and
 * any dead rows or columns.
 */
static void draw_test_image(void) {

  lcd.clear();
  lcd.frame(0, 0, pika::lcd::width, pika::lcd::height, true);

  for (int y = 0; y < 24; y += 8) {
    for (int x = 0; x < 32; x += 8) {
      if (((x ^ y) & 8) == 0) {
        lcd.rect(4 + x, 68 + y, 8, 8, true);
      }
    }
  }

  for (int i = 0; i < pika::lcd::height; i++) {
    lcd.pixel(pika::lcd::width - pika::lcd::height + i, i, true);
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

int main(void) {

  halInit();
  chSysInit();

  pika::log_init();
  LOG("\r\n" BOARD_NAME " starting");

  lcd.backlight(true);

  bool ok = lcd.init();
  LOG("lcd: init %s, i2c error 0x%08x, status 0x%02x", ok ? "ok" : "failed",
      (unsigned)lcd.last_error(), (unsigned)lcd.read_status());

  if (ok) {
    bool verified = lcd.verify();
    const uint8_t *got = lcd.verify_read();
    LOG("lcd: readback %s, got %02x %02x %02x %02x %02x %02x %02x %02x",
        verified ? "ok" : "NO DATA",
        (unsigned)got[0], (unsigned)got[1],
        (unsigned)got[2], (unsigned)got[3],
        (unsigned)got[4], (unsigned)got[5],
        (unsigned)got[6], (unsigned)got[7]);

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

    /* stop() rides out the release ramp before muting, so the sleep only has
       to cover the tone itself.*/
    spk.tone(1000, 150);
    chThdSleepMilliseconds(150);
    spk.stop();
  }

  chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat,
                    nullptr);

  while (true) {
    chThdSleepMilliseconds(1000);
  }
}
