# Memory

- [Minimal CMake scope](build-scope-minimal-cmake.md) — build system is hardcoded to one MCU on purpose; don't generalize it.
- [pika_go_0 board placeholders](pika-go-0-board-placeholders.md) — 24MHz HSE oscillator gated by PC15, LED on PE9, confirmed on hardware.
- [Plan mode first](prefers-plan-mode-first.md) — user wants the approach approved before files get written.
- [I2C DMA silently broken](i2c-dma-silently-broken.md) — DMA needs D2 SRAM buffers *and* an MPU no-cache region, or it moves nothing silently.
- [LCD ST75160 facts](lcd-st75160-facts.md) — panel address, mono RAM layout, framing, readback.
