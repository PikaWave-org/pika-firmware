---
name: gnss-slow-clock-emi-dead-end
description: Slowing the core to 4MHz to cut EMI kills the 115200 GNSS link, and the stale readings it leaves behind look like a negative result.
metadata:
  node_type: memory
  type: project
  originSessionId: 9bc822b4-dec4-47fb-8f73-83e9f61391cb
  modified: 2026-09-16T00:00:00.000Z
---

Tried on 2026-09-13: cut EMI into the GNSS antenna by dropping the core from
the 520MHz PLL1 to the 4MHz CSI and powering down the 24MHz HSE through PC15.
Reverted. The commits are dangling but still in the object database at
`8e148da` (`git show 8e148da`) until a gc.

LSI was the first idea and is not available at all: `RCC_CFGR.SW` has only four
encodings on this part (HSI, CSI, HSE, PLL1), so 4MHz from CSI is the floor.

**The switch itself worked**, confirmed over SWD: `RCC_CFGR` read SW and SWS
both CSI, `RCC_CR` read HSEON and HSERDY clear with all three PLLs off. Log,
LCD and a 1Hz heartbeat all survived - but only after rewriting every register
derived from the boot clock at compile time: both USART BRRs, TIM2's prescaler
(otherwise ChibiOS's whole sense of time runs 65x slow) and I2C1's TIMINGR.
Note TIMINGR cannot be poked directly: the LCD recovery path restarts the
peripheral from the driver's own config and puts the old value straight back.

**What killed it: a 4MHz core cannot service 115200.** A byte lands every ~347
CPU cycles, about what ChibiOS's `serve_interrupt` costs. Measured by reading a
byte counter out of RAM over SWD: **~24 B/s arriving against the ~168 B/s the
receiver sends**, so ~86% of bytes lost and a frame never assembles.

## The trap - why this looked like a finished experiment

After the switch `frames` and `errors` both freeze, and `mon_hw()` keeps
returning **the last message from before the switch**. Noise and AGC therefore
read unchanged, which is very easy to report as "slowing the core did not
reduce antenna noise". It is not a measurement at all: no MON-HW arrived after
the switch. `errors` freezing is not reassurance either - the driver only
increments it at the moment sync is *lost*, not per garbled byte.

**So the EMI question is still open.** Nobody has measured it.

To make it testable the link has to survive first, either:

- move the receiver to 9600 with a CFG-PRT *before* switching - a byte every
  ~4166 cycles at 4MHz is comfortable, and 9600 carries the 168 B/s at 17%
  utilisation; or
- use HSI at 64MHz instead of CSI - the PLLs and the HSE still stop, so most of
  the EMI change remains, and 115200 keeps working untouched.

Related: [[gnss-front-end-baseline]], [[chibios-busy-wait-starves-threads]]
