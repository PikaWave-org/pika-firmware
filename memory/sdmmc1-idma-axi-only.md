---
name: sdmmc1-idma-axi-only
description: SDMMC1's IDMA reaches only AXI SRAM, so the SD buffers live in their own non-cacheable AXI window, not .nocache; how the ChibiOS LLD buffers are steered there.
metadata:
  type: project
---

The microSD slot is on **SDMMC1**, whose IDMA is a D1 master: it reaches AXI
SRAM (0x24000000) but **not** D2 SRAM, where `.nocache` lives, and not DTCM,
where the stacks live. The SDMMCv2 LLD also does no cache maintenance and
DMAs straight into the caller's buffer. Either mistake moves garbage with no
error - the same shape as [[i2c-dma-silently-broken]].

- **8K window at the top of AXI SRAM**, `0x2404E000`, section `.sdmmc`,
  non-cacheable through **MPU region 4** in `boardInit()` (5 is SRAM4, 6 the
  ChibiOS nocache region, 7 the stack guard).
- **The LLD names its own buffers `__nocache_sd1_*`**, which the `.nocache`
  rule would claim. `.sdmmc` comes first in the linker script and matches
  them, because the first matching rule wins. After a linker change,
  `arm-none-eabi-nm -S firmware.elf | grep __nocache_sd1` must show
  0x2404Exxx.
- **FatFS uses `src/pika/storage/diskio.c`**, not ChibiOS's
  `fatfs_bindings`, and bounces every transfer through `.sdmmc`, so callers
  may pass any buffer.

Verified on hardware 2026-10-08 with a 3.6GiB SDHC FAT32 card, running
4-bit at 26MHz (`CLKCR` 0x5001): files written, read back and compared.

**How to apply:** any new SDMMC1 DMA buffer goes in `.sdmmc`; never hand a
caller's buffer to `sdcRead`/`sdcWrite`.
