---
name: gpio-fault-isolation-technique
description: "How to prove a dead button is hardware and not firmware - fake the press over SWD, then scan every port with a known-good button as the control."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 992d855b-fcb6-460b-ae10-3f41c6073967
  modified: 2026-09-16T19:25:33.260Z
---

When an input looks dead, settle *where* it is dead before touching code.
Worked on 2026-09-16 for BTN_UP, which turned out to be a bad solder joint on
SW4 / U4 pin 57 and was fixed with an iron - no firmware change at all.

The three checks, cheapest first:

1. **Exonerate the firmware without a finger.** Flip the line's PUPDR from
   pull-up to pull-down over SWD and watch the driver's state change - see
   [[swd-run-control-and-fake-presses]]. If a forced low produces the event,
   the EXTI/debounce/broadcast path is proven good for *that* pin.
2. **Check the pin assignment against the schematic**, don't trust board.h -
   see [[hardware-repo-schematics]].
3. **Scan every port's IDR while a human presses**, ~200 ms sampling, and
   report which bits toggled.

**The rule that makes step 3 mean anything: always include a known-good button
as a positive control, and check the control fired before believing a quiet
pin.** Two scans here reported "no button pin moved" and were pure noise -
nobody was at the keyboard, and the controls were silent too, which was the
only thing distinguishing a real null from an empty room. The run that
mattered showed PD11/DOWN dipping 4 times while PD10/UP never moved once.

**Why:** a firmware-side hypothesis costs hours and a resolved hardware fault
costs minutes with a meter, and a null measurement with no control is
indistinguishable from not having measured at all.

**How to apply:** the PUPDR trick also drives any feature that needs a press,
so a UI state machine can be walked end to end over SWD with nobody at the
keyboard.
