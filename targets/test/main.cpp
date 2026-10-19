#include "ch.h"
#include "hal.h"

#include <pika/buttons.h>
#include <pika/log.h>

// Test image: the button driver against the PTT contact. Read out over SWD
// by symbol, all timestamped with the system tick timer (TIM2, 100us):
//   ptt_n, ptt_edges[0..ptt_n)  every raw PTT edge, {ticks, level read in the
//                               interrupt}, taken ahead of the driver's own
//                               callback;
//   btn_n, btn_events[0..btn_n) what the driver reported, {ticks, button,
//                               with 0x100 set for a release}.
struct Stamp {
  uint32_t ticks;
  uint32_t value;
};
volatile uint32_t ptt_n = 0U;
Stamp ptt_edges[1024];
volatile uint32_t btn_n = 0U;
Stamp btn_events[256];

static const pika::input::Buttons::Config btn_cfg = {
    .lines = {{LINE_BTN_PWR, true},
              {LINE_BTN_UP, false},
              {LINE_BTN_DOWN, false},
              {LINE_BTN_RIGHT, false},
              {LINE_BTN_LEFT, false},
              {LINE_BTN_PTT, false},
              {LINE_BTN_LSN, false}},
    .event_offset = 0};
static pika::input::Buttons buttons{btn_cfg};
static palcallback_t ptt_driver_cb;

static void ptt_edge(void *arg) {
  const uint32_t n = ptt_n;
  if (n < 1024U) {
    ptt_edges[n] = {STM32_ST_TIM->CNT, (uint32_t)palReadLine(LINE_BTN_PTT)};
    ptt_n = n + 1U;
  }
  ptt_driver_cb(arg);
}

static void record(uint32_t value) {
  const uint32_t n = btn_n;
  if (n < 256U) {
    btn_events[n] = {STM32_ST_TIM->CNT, value};
    btn_n = n + 1U;
  }
}

int main() {
  halInit();
  chSysInit();

  pika::log_init();
  LOG("\r\n" BOARD_NAME " test image starting");

  buttons.init();
  palevent_t *pe = pal_lld_get_line_event(LINE_BTN_PTT);
  ptt_driver_cb = pe->cb;
  palSetLineCallback(LINE_BTN_PTT, ptt_edge, pe->arg);

  event_listener_t listener;
  chEvtRegister(&buttons.events(), &listener, 0);
  while (true) {
    chEvtWaitAny(EVENT_MASK(0));
    const eventflags_t flags = chEvtGetAndClearFlags(&listener);
    for (unsigned i = 0; i < pika::input::Buttons::count; i++) {
      const auto b = (pika::input::Button)i;
      if (flags & buttons.press_flag(b)) {
        record(i);
      }
      if (flags & buttons.release_flag(b)) {
        record(i | 0x100U);
      }
    }
  }
}
