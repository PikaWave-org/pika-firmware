# Memory

Things about this board that the code cannot tell you, each one paid for with a
debugging session. Read the entry before working in the area it covers.

Keep this set small and true. A note earns its place by saving someone a day;
delete one the moment it stops being accurate, and put style, workflow and
design rules in CLAUDE.md instead of here.

## Live problems

- [LCD flush hangs after a few beats](lcd-flush-hangs-after-few-beats.md) — unfixed. The log dying a few seconds in is an I2C lost wakeup, not your change, and a long healthy run does not mean a fix. Read this before scoring any boot.

## The board

- [pika_go_0 placeholders](pika-go-0-board-placeholders.md) — 24MHz HSE gated by PC15, LED on PE9, confirmed on hardware.
- [Button pinout and the EXTI-10 collision](button-driver-exti10-collision.md) — the seven button pads, and why an EXTI channel is chosen by pad *number*, not port, so BTN_PWR has to be polled.
- [LCD ST75160 facts](lcd-st75160-facts.md) — I2C address, the mono RAM layout whose bit order is the opposite of SSD1306, framing, contrast formula, readback.
- [Schematics live in a sibling repo](hardware-repo-schematics.md) — where they are, and how to resolve a net to an MCU pin without kicad-cli.

## Traps that cost a session each

- [I2C DMA silently moves nothing](i2c-dma-silently-broken.md) — DMA needs D2 SRAM buffers *and* an MPU no-cache region, or it transfers zero bytes and reports success.
- [Busy-waits starve every thread](chibios-busy-wait-starves-threads.md) — round-robin is off, so one unbounded spin stops the whole board and looks like a hard fault.
- [Worktrees need their submodules initialised](worktree-needs-submodules-init.md) — a fresh worktree will not build until you populate `submodules/`, and `--reference` is how to do it without a slow clone.

## Working on the hardware

- [SWD run control and fake presses](swd-run-control-and-fake-presses.md) — JLinkExe command files cannot resume the core; use the GDB server. Flipping PUPDR fakes a button press, which drives any feature needing input with nobody at the keyboard.
- [Isolating a GPIO fault](gpio-fault-isolation-technique.md) — prove whether a dead input is hardware or firmware before touching code, and why a scan without a known-good control proves nothing.
