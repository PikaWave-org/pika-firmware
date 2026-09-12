# Pika Go 0 board (STM32H733VGT6).
#
# The memory map lives here rather than in ChibiOS: that tree ships no script
# for the H733, and the H735xG one it would otherwise borrow has the D2 SRAM2
# origin wrong for this part. The script still includes ChibiOS's rules_*.ld
# for the section layout, which the link finds through the library path set
# in cmake/chibios.cmake.
set(BOARD_LDSCRIPT "${CMAKE_CURRENT_LIST_DIR}/pika_go_0.ld")

# Device name passed to J-Link when flashing over SWD.
set(BOARD_JLINK_DEVICE "STM32H733VG")
