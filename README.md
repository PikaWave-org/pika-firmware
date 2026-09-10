# pika-firmware
Firmware and tools for Pika

## Building

Requires `arm-none-eabi-gcc` and CMake 3.20+, plus ChibiOS in
`submodules/ChibiOS`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Produces `build/targets/firmware/firmware.elf` and
`build/targets/test/test.elf`.

## Layout

- `boards/<board>/` — hardware description reused by every firmware image:
  `board.h`/`board.c` (pinout, oscillators, MCU type), `cfg/mcuconf.h` (clock
  tree and peripheral settings), `board.cmake` (linker script).
- `targets/<target>/` — one firmware image: `main.cpp`, its own
  `cfg/chconf.h`/`cfg/halconf.h` (kernel and HAL configuration) and a
  `CMakeLists.txt` calling `pika_add_firmware(<name> BOARD <board> SOURCES ...)`.
- `src/` — code shared between targets (on the include path).
- `cmake/chibios.cmake` — ChibiOS source/include lists and the
  `pika_add_firmware()` helper. Hardcoded for STM32H733 (Cortex-M7) with
  ChibiOS RT; the enabled low level drivers are listed there.

Adding a firmware image means creating a directory under `targets/`, with no
changes to the board files.
