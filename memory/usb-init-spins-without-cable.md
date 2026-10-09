---
name: usb-init-spins-without-cable
description: With no USB cable, tinyUSB's init spins forever on the unpowered ULPI PHY at HIGHPRIO, starving main() and the heartbeat; the debug log goes silent and it looks like a boot hang.
metadata:
  type: project
---

The USB3300 PHY is powered from VBUS only (see `board.h`), so with no cable
there is no ULPI clock and tinyUSB's `reset_core()` (`dwc2_common.c`) waits
on `GRSTCTL_CSRST` forever. The `tinyUSB` thread is HIGHPRIO with round-robin
off, so every lower thread starves - `main()` past `usb.start()` and the
heartbeat included. See [[chibios-busy-wait-starves-threads]].

**What it looks like:** `tools/hw check` flashes fine and captures zero
bytes. Halting over SWD shows the PC in `reset_core` and the current thread
"tinyUSB".

**How to apply:** on a silent log, halt over SWD and rule this out before
suspecting your change. Anything `main()` must do runs before `usb.start()`;
the SD mount does for this reason.
