---
name: i2c-dma-silently-broken
description: I2C DMA needs the buffers in D2 SRAM AND excluded from the D-cache; without that it reports success and moves nothing
metadata:
  type: project
---

On this board (STM32H733, ChibiOS I2Cv3, I2C1) DMA transfers return `MSG_OK`,
the slave ACKs every byte and `i2cGetErrors()` stays clean **while no data
moves at all** - unless two things are both true:

1. The buffers live in **D2 SRAM** (`0x30004000`, the linker script's
   `.nocache` section - `DMA_BUF` in `src/pika/lcd/ST75160.cpp` and
   `DACSpeaker.cpp`, the same attribute written out on
   `ADCMicrophone::mic_buffer`), the domain DMA1/DMA2 sit in. Its clock is off at reset; `board.c` enables SRAM1/2 in
   `RCC_AHB2ENR` before anything touches it.
2. That memory is **excluded from the data cache**. ChibiOS enables the
   Cortex-M7 D-cache in `crt1.c` (`__cpu_init`), so cached buffers leave the
   CPU and DMA looking at different data. `STM32_NOCACHE_ENABLE TRUE` with
   `STM32_NOCACHE_RBAR 0x30004000` and `MPU_RASR_SIZE_16K` puts an MPU region
   over exactly that area; verified on hardware 2026-09-12 with the D-cache
   on (`SCB_CCR` DC=1) and the region reading back non-cacheable.

**Why:** the cache half of this is invisible from the I2C side and cost a
whole LCD bring-up session. The wrong conclusion that started it was "nothing
enables the D-cache in this build", from grepping `crt0_v7m.S` and
`hal_lld.c` but not `crt1.c`.

**How to apply:** anything a DMA controller touches goes in `.nocache`, never
on a thread stack (those are DTCM, which no DMA controller here can reach).
After any change to a DMA transport, prove data actually moves - read the
panel back ([[read-panel-over-swd]]) or check the mic meter varies - rather
than trusting return codes. See [[lcd-st75160-facts]].
