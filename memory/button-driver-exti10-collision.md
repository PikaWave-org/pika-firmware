---
name: button-driver-exti10-collision
description: BTN_PWR (PE10) is polled rather than interrupt driven because PD10 already owns EXTI channel 10.
metadata: 
  node_type: memory
  type: project
  originSessionId: 68c27190-0dc1-49a8-8576-18d88f5ee199
  modified: 2026-09-16T20:31:12.908Z
---

The pika_go_0 front panel buttons are PE10 = PWR, PD10 = UP, PD11 = DOWN,
PD14 = RIGHT, PC4 = LEFT, PC5 = PTT, PB2 = LSN, all shorting to ground against
internal pull-ups.

An STM32 EXTI channel is selected by **pad number** and maps to one port at a
time, so PE10 and PD10 cannot both be interrupt driven — ChibiOS catches the
second with a "channel already in use" assertion
(`GPIOv2/hal_pal_lld.c`). PD10 keeps the interrupt; **PE10 is polled**, which
is why `pika::input::Buttons::Config` carries a per-line `polled` flag and the
driver runs a periodic virtual timer at all.

**The polling path is a known temporary workaround and is going away.** The
user confirmed on 2026-09-16 that BTN_PWR moves to another line in the next
hardware revision, after which the whole polling path and its timer can be
deleted with no API change — `init()` already skips the timer when no line is
marked polled. So **nothing else may come to depend on that timer running.**
A rescan-every-line design was tried and reverted for exactly this reason: it
made bounce recovery rely on the timer, which would have regressed silently the
moment polling was removed. Edges dispatch straight to the line that fired.

The six interrupt-driven lines land on channels 2, 4, 5, 10, 11 and 14, which
are all distinct.

**How to apply:** when adding or moving a button, check the *pad number*
against the ones already in use, not the port. Verify on hardware by reading
`SYSCFG->EXTICR` at 0x58000408 — one nibble per channel, the value being the
port index (B = 1, C = 2, D = 3, E = 4) — rather than trusting that
`palEnableLineEvent()` succeeded. Re-flash before trusting any such reading:
a J-Link session that set breakpoints can leave the image on the board altered,
so read state inside one `tools/hw exec` hold with a fresh flash.
