# Pika GO rev.0 board (STM32H733VGT6).
#
# ChibiOS ships no linker script for the H733; the H735xG one describes the
# same memory map (1M flash, 320k AXI SRAM, 128k DTCM, 64k ITCM) and is used
# as is.
set(BOARD_LDSCRIPT "${CHIBIOS_STARTUP_LD}/STM32H735xG_ITCM64k.ld")
