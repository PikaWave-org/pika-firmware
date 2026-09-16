---
name: build-scope-minimal-cmake
description: Pika firmware build system is intentionally hardcoded to one MCU/config; keep CMake as small as possible
metadata: 
  node_type: memory
  type: project
  originSessionId: d3d3ef7b-5cf5-4db6-8cb7-0dfb4bdf1233
  modified: 2026-09-10T06:21:38.986Z
---

The CMake setup for pika-firmware is deliberately hardcoded to a single MCU
(STM32H733VGT6, ChibiOS RT + HAL, arm-none-eabi-gcc) and a limited set of HAL
drivers. Requested 2026-09-09: "keep cmake as small as possible, there is no
need to support all MCUs and features of ChibiOS, only hardcoded limited set
for now." Also chosen then: ELF output only — no hex/bin/size/flash/debug
targets.

**Why:** the value is in the board/target split being reusable, not in a
general-purpose ChibiOS CMake port.

**How to apply:** when adding support for a peripheral or a second MCU, extend
the hardcoded lists in `cmake/chibios.cmake` rather than introducing generic
per-MCU abstraction or auto-discovery. Don't add convenience targets
(flash, hex, size) unless asked. See [[pika-go-0-board-placeholders]].
