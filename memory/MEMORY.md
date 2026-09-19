# Memory

Things about this board that the code cannot tell you, each one paid for with a
debugging session. Read the entry before working in the area it covers.

Keep this set small and true. A note earns its place by saving someone a day;
delete one the moment it stops being accurate, and put style, workflow and
design rules in CLAUDE.md instead of here.

## The board

- [pika_go_0 placeholders](pika-go-0-board-placeholders.md) — 24MHz HSE gated by PC15, LCD backlight (not an LED) on PE9, confirmed on hardware.
- [Button pinout and the EXTI-10 collision](button-driver-exti10-collision.md) — the seven button pads, and why an EXTI channel is chosen by pad *number*, not port, so BTN_PWR has to be polled.
- [LCD ST75160 facts](lcd-st75160-facts.md) — I2C address, the mono RAM layout whose bit order is the opposite of SSD1306, framing, contrast formula, readback.
- [Schematics live in a sibling repo](hardware-repo-schematics.md) — where they are, and how to resolve a net to an MCU pin without kicad-cli.
- [Microphone front end](mic-adc-front-end.md) — PA6 is ADC1_INP3 and EXTSEL 13 is TIM6_TRGO, both confirmed on the bench; why MIC_SHDN is active low; and the ADC3 setting a part without an ADC3 still needs.
- [MELPe 2400 cost](melpe-2400-cost.md) — 7ms of the 22.5ms frame to encode and 2ms to decode, measured; the DWT unlock without which every timing reads zero; and why a bitstream CRC only compares against the same samples.

## Traps that cost a session each

- [I2C DMA silently moves nothing](i2c-dma-silently-broken.md) — DMA needs D2 SRAM buffers *and* an MPU no-cache region, or it transfers zero bytes and reports success.
- [Busy-waits starve every thread](chibios-busy-wait-starves-threads.md) — round-robin is off, so one unbounded spin stops the whole board and looks like a hard fault.

## Working on the hardware

- [SWD run control and fake presses](swd-run-control-and-fake-presses.md) — JLinkExe V7.88 ignores argument-less commands (`r`, `g`, `qc`) in a command file, which is why flashing used to hang; feed them on stdin. Timed run control goes through the GDB server. Flipping PUPDR fakes a button press, which drives any feature needing input with nobody at the keyboard.
- [Isolating a GPIO fault](gpio-fault-isolation-technique.md) — prove whether a dead input is hardware or firmware before touching code, and why a scan without a known-good control proves nothing.
- [Reading the panel over SWD](read-panel-over-swd.md) — where the framebuffer lives and how to decode it, so panel output can be checked on a running board with nobody looking at it.
