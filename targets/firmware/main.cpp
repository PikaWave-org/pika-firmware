#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include "log.h"
#include "st75160.h"

/*
 * Bring-up image: a border, a checkerboard block, a diagonal and a text
 * line. Between them these show orientation, mirroring, the pixel grid and
 * any dead rows or columns.
 */
static void draw_test_image(void) {

  lcd::clear();
  lcd::frame(0, 0, lcd::width, lcd::height, true);

  for (int y = 0; y < 24; y += 8) {
    for (int x = 0; x < 32; x += 8) {
      if (((x ^ y) & 8) == 0) {
        lcd::rect(4 + x, 68 + y, 8, 8, true);
      }
    }
  }

  for (int i = 0; i < lcd::height; i++) {
    lcd::pixel(lcd::width - lcd::height + i, i, true);
  }

  lcd::text(4, 6, BOARD_NAME);
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
    lcd::rect(4, 20, 8 * 12, 8, false);
    lcd::text(4, 20, line);

    systime_t start = chVTGetSystemTimeX();
    bool ok = lcd::flush();
    sysinterval_t took = chVTTimeElapsedSinceX(start);

    if (!ok) {
      LOG("lcd: flush failed, i2c error 0x%08x", (unsigned)lcd::last_error());
    }
    else if (beat == 0) {
      LOG("lcd: flush took %ums", (unsigned)TIME_I2MS(took));
    }

    beat++;
    chThdSleepMilliseconds(1000);
  }
}

int main(void) {

  halInit();
  chSysInit();

  log_init();
  LOG("\r\n" BOARD_NAME " starting");

  lcd::backlight(true);

  bool ok = lcd::init();
  LOG("lcd: init %s, i2c error 0x%08x, status 0x%02x", ok ? "ok" : "failed",
      (unsigned)lcd::last_error(), (unsigned)lcd::read_status());

  if (ok) {
    draw_test_image();
    if (!lcd::flush()) {
      LOG("lcd: flush failed, i2c error 0x%08x", (unsigned)lcd::last_error());
    }
  }

  chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat,
                    nullptr);

  while (true) {
    chThdSleepMilliseconds(1000);
  }
}
