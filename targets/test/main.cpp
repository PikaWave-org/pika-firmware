#include "ch.h"
#include "hal.h"

/*
 * Test image: same board, different firmware. Blinks fast so the running
 * image can be told apart from the normal firmware at a glance.
 */
static THD_WORKING_AREA(waBlinker, 128);
static THD_FUNCTION(blinker, arg) {

  (void)arg;
  chRegSetThreadName("blinker");

  while (true) {
    palTogglePad(GPIOB, GPIOB_LED);
    chThdSleepMilliseconds(100);
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
