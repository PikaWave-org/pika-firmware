#include "ch.h"
#include "hal.h"

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

int main() {

  halInit();
  chSysInit();

  pika::log_init();
  LOG("\r\n" BOARD_NAME " test image starting");

  chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat,
                    nullptr);

  while (true) {
    chThdSleepMilliseconds(1000);
  }
}
