#include "ch.h"
#include "hal.h"

/*
 * Heartbeat: one short blink per second.
 */
static THD_WORKING_AREA(waBlinker, 128);
static THD_FUNCTION(blinker, arg) {

  (void)arg;
  chRegSetThreadName("blinker");

  while (true) {
    palSetPad(GPIOB, GPIOB_LED);
    chThdSleepMilliseconds(100);
    palClearPad(GPIOB, GPIOB_LED);
    chThdSleepMilliseconds(900);
  }
}

int main(void) {

  halInit();
  chSysInit();

  chThdCreateStatic(waBlinker, sizeof(waBlinker), NORMALPRIO, blinker, nullptr);

  while (true) {
    chThdSleepMilliseconds(1000);
  }
}
