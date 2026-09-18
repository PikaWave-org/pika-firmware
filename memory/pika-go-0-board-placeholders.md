---
name: pika-go-0-board-placeholders
description: "Pika Go 0 hardware facts confirmed on the bench - 24MHz HSE oscillator gated by PC15, LCD backlight on PE9"
metadata: 
  node_type: memory
  type: project
  originSessionId: d3d3ef7b-5cf5-4db6-8cb7-0dfb4bdf1233
  modified: 2026-09-10T19:54:17.316Z
---

Confirmed on hardware 2026-09-10 (superseding the earlier stub guesses of a
25 MHz crystal and an LED on PB0):

- The 24 MHz HSE is an **oscillator module in bypass mode**
  (`STM32_HSE_BYPASS`), not a crystal. It is powered from an HSE_EN pin on
  **PC15**, which `board.c` drives high in `stm32_gpio_init()` before
  `stm32_clock_init()` runs — without that the clock init spins forever on
  HSERDY.
- **PE9 is the LCD backlight**, not a heartbeat LED - the 2026-09-10 note
  calling it one was a guess from before the schematic was read. It drives the
  gate of Q1 (BSS138) which switches the backlight string on J4 to ground, and
  it is TIM1_CH1 on AF1, so brightness is a PWM duty cycle. `board.c` leaves it
  a plain output, low, and `ST75160::backlight()` takes the pin over when it
  starts the timer. There is no separate indicator LED on this board.
- No 32.768 kHz LSE; RTC runs off LSI.
- Clock tree: HSE 24 MHz / 6 = 4 MHz PLL input, PLL1 VCO 520 MHz →
  520 MHz SYSCLK, 260 MHz HCLK. Verified: `RCC_CR = 0x3F07F1A5`
  (HSEON+HSERDY+HSEBYP, all three PLLs locked).

**Why:** none of this is inferable from the schematic-free board files, and
the PC15 gating is easy to lose if `board.c` is regenerated from a ChibiOS
board template.

**How to apply:** keep the PC15 enable ahead of `stm32_clock_init()`; if the
HSE frequency changes, revisit `STM32_PLLx_DIVM/DIVN` in
`boards/pika_go_0/cfg/mcuconf.h` together.
