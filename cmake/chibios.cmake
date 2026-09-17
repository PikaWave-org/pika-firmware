# ChibiOS build glue for STM32H733 (Cortex-M7, ChibiOS RT + HAL).
#
# Deliberately hardcoded to one MCU family and one kernel configuration: the
# source and include lists below mirror what the ChibiOS makefiles pull in for
# an STM32H7xx RT project (startup_stm32h7xx.mk, platform_type2.mk, rt.mk,
# hal.mk, ARMv7-M port.mk).
#
# ChibiOS sources are compiled per firmware target rather than into a shared
# library, because they depend on the target's chconf.h/halconf.h.

set(CHIBIOS "${CHIBIOS_ROOT}")
set(CHIBIOS_STARTUP_LD "${CHIBIOS}/os/common/startup/ARMCMx/compilers/GCC/ld")

set(CHIBIOS_LLD "${CHIBIOS}/os/hal/ports/STM32/LLD")

# Kernel, oslib and HAL sources are globbed: every file is internally gated by
# chconf.h/halconf.h, so unused ones compile to nothing.
file(GLOB CHIBIOS_KERNEL_SOURCES
        "${CHIBIOS}/os/rt/src/*.c"
        "${CHIBIOS}/os/oslib/src/*.c"
        "${CHIBIOS}/os/hal/src/*.c")

set(CHIBIOS_SOURCES
        # Startup and C runtime.
        "${CHIBIOS}/os/common/startup/ARMCMx/compilers/GCC/crt0_v7m.S"
        "${CHIBIOS}/os/common/startup/ARMCMx/compilers/GCC/vectors.S"
        "${CHIBIOS}/os/common/startup/ARMCMx/compilers/GCC/crt1.c"
        # ARMv7-M port.
        "${CHIBIOS}/os/common/ports/ARMv7-M/chcore.c"
        "${CHIBIOS}/os/common/ports/ARMv7-M/compilers/GCC/chcoreasm.S"
        # Kernel, oslib, HAL and OSAL.
        ${CHIBIOS_KERNEL_SOURCES}
        "${CHIBIOS}/os/hal/osal/rt-nil/osal.c"
        # chprintf() and friends (os/hal/lib/streams/streams.mk).
        "${CHIBIOS}/os/hal/lib/streams/chprintf.c"
        "${CHIBIOS}/os/hal/lib/streams/memstreams.c"
        # STM32H7xx platform and the low level drivers we enable.
        "${CHIBIOS}/os/hal/ports/common/ARMCMx/nvic.c"
        "${CHIBIOS}/os/hal/ports/STM32/STM32H7xx/hal_lld.c"
        "${CHIBIOS}/os/hal/ports/STM32/STM32H7xx/stm32_isr.c"
        "${CHIBIOS_LLD}/DMAv2/stm32_dma.c"
        "${CHIBIOS_LLD}/BDMAv1/stm32_bdma.c"
        "${CHIBIOS_LLD}/MDMAv1/stm32_mdma.c"
        "${CHIBIOS_LLD}/ADCv4/hal_adc_lld.c"
        "${CHIBIOS_LLD}/DACv1/hal_dac_lld.c"
        "${CHIBIOS_LLD}/GPIOv2/hal_pal_lld.c"
        "${CHIBIOS_LLD}/I2Cv3/hal_i2c_lld.c"
        "${CHIBIOS_LLD}/TIMv1/hal_gpt_lld.c"
        "${CHIBIOS_LLD}/SYSTICKv1/hal_st_lld.c"
        "${CHIBIOS_LLD}/USARTv3/hal_serial_lld.c")

# All low level driver directories are on the include path (as PLATFORMINC in
# platform_type2.mk): stm32_isr.c and hal_lld.h pull in headers and .inc files
# from peripherals we do not otherwise build.
set(CHIBIOS_LLD_INCLUDE_DIRS)
foreach (lld ADCv4 BDMAv1 CRYPv1 DACv1 DMAv2 EXTIv1 FDCANv2 GPIOv2 I2Cv3 MACv2
        MDMAv1 OCTOSPIv2 OTGv1 RNGv1 RTCv2 SDMMCv2 SPIv3 SYSTICKv1 TIMv1
        USART USARTv3 xWDGv1)
    list(APPEND CHIBIOS_LLD_INCLUDE_DIRS "${CHIBIOS_LLD}/${lld}")
endforeach ()

set(CHIBIOS_INCLUDE_DIRS
        "${CHIBIOS}/os/license"
        "${CHIBIOS}/os/common/portability/GCC"
        "${CHIBIOS}/os/common/startup/ARMCMx/compilers/GCC"
        "${CHIBIOS}/os/common/startup/ARMCMx/devices/STM32H7xx"
        "${CHIBIOS}/os/common/ext/ARM/CMSIS/Core/Include"
        "${CHIBIOS}/os/common/ext/ST/STM32H7xx"
        "${CHIBIOS}/os/common/ports/ARM-common"
        "${CHIBIOS}/os/common/ports/ARMv7-M"
        "${CHIBIOS}/os/rt/include"
        "${CHIBIOS}/os/oslib/include"
        "${CHIBIOS}/os/hal/include"
        "${CHIBIOS}/os/hal/osal/rt-nil"
        "${CHIBIOS}/os/hal/lib/streams"
        "${CHIBIOS}/os/hal/ports/common/ARMCMx"
        "${CHIBIOS}/os/hal/ports/STM32/STM32H7xx"
        ${CHIBIOS_LLD_INCLUDE_DIRS})

# Port the J-Link GDB server listens on.
set(GDB_PORT 2331)

# Cortex-M7 with double precision FPU, as in ChibiOS rules.mk.
set(MCU_FLAGS -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16)
set(MCU_COMPILE_FLAGS ${MCU_FLAGS} -ffunction-sections -fdata-sections
        -fno-common -falign-functions=16 -Wall -Wextra -Wundef)

# pika_add_firmware(<name> BOARD <board> SOURCES <src>...)
#
# Creates the <name>.elf firmware image from the given sources plus ChibiOS and
# the board support files. The calling directory's cfg/ (chconf.h, halconf.h)
# comes first on the include path so each target owns its kernel configuration.
function(pika_add_firmware name)
    cmake_parse_arguments(FW "" "BOARD" "SOURCES" ${ARGN})

    set(board_dir "${PROJECT_ROOT}/boards/${FW_BOARD}")

    add_executable(${name}.elf
            ${FW_SOURCES}
            "${board_dir}/board.c"
            ${CHIBIOS_SOURCES})

    target_include_directories(${name}.elf PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/cfg"
            "${board_dir}"
            "${board_dir}/cfg"
            "${SRC_ROOT}"
            ${CHIBIOS_INCLUDE_DIRS})

    # The startup code only enables the FPU when this is set (DDEFS/DADEFS in
    # ChibiOS rules.mk); without it -mfloat-abi=hard faults with UFSR.NOCP on
    # the first FP instruction.
    target_compile_definitions(${name}.elf PRIVATE CORTEX_USE_FPU=TRUE)

    # No atexit registration for static objects with destructors: the firmware
    # never exits, and without it the link wants a __dso_handle that nothing
    # here (nosys, no crt0) provides.
    target_compile_options(${name}.elf PRIVATE
            ${MCU_COMPILE_FLAGS}
            $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti -fno-exceptions -fno-threadsafe-statics -fno-use-cxa-atexit>)

    target_link_options(${name}.elf PRIVATE
            ${MCU_FLAGS}
            -nostartfiles
            # Stack sizes, as USE_PROCESS_STACKSIZE/USE_EXCEPTIONS_STACKSIZE
            # in ChibiOS rules.mk.
            -Wl,--defsym=__process_stack_size__=0x400
            -Wl,--defsym=__main_stack_size__=0x400
            --specs=nano.specs
            --specs=nosys.specs
            -T${BOARD_LDSCRIPT}
            -Wl,--library-path=${CHIBIOS_STARTUP_LD}
            -Wl,--gc-sections
            -Wl,-Map=${name}.map)

    set_target_properties(${name}.elf PROPERTIES LINK_DEPENDS "${BOARD_LDSCRIPT}")

    # Intel HEX image for the flashing tool.
    set(FLASH_HEX "${CMAKE_CURRENT_BINARY_DIR}/${name}.hex")
    add_custom_command(TARGET ${name}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${name}.elf> "${FLASH_HEX}"
            BYPRODUCTS "${FLASH_HEX}"
            COMMENT "Generating ${name}.hex")

    # "make flash-<name>": program over SWD with a SEGGER J-Link, given the
    # same commands one would type at its prompt. They arrive on stdin rather
    # than via -CommandFile because J-Link Commander V7.88 does not parse
    # argument-less commands ("r", "g", "qc") out of a command file, and
    # without a working "qc" it falls into its interactive prompt and hangs
    # whatever build tool ran it.
    configure_file("${PROJECT_ROOT}/cmake/flash.jlink.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${name}.jlink" @ONLY)
    add_custom_target(flash-${name}
            COMMAND sh -c "JLinkExe -nogui 1 -device ${BOARD_JLINK_DEVICE} -if SWD -speed 4000 -autoconnect 1 < ${CMAKE_CURRENT_BINARY_DIR}/${name}.jlink"
            DEPENDS ${name}.elf
            USES_TERMINAL VERBATIM
            COMMENT "Flashing ${name}.hex via J-Link")

    # "make debug-<name>": run a J-Link GDB server for this image. Connect
    # from another terminal with
    #   gdb-multiarch -x <name>.gdbinit <name>.elf
    # The ChibiOS RTOS plugin makes GDB's "info threads" show ChibiOS threads.
    set(FW_NAME ${name})
    configure_file("${PROJECT_ROOT}/cmake/gdbinit.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${name}.gdbinit" @ONLY)
    add_custom_target(debug-${name}
            COMMAND JLinkGDBServerCLExe -device ${BOARD_JLINK_DEVICE} -if SWD
                    -speed 4000 -port ${GDB_PORT} -nogui -singlerun
                    -rtos GDBServer/RTOSPlugin_ChibiOS
            DEPENDS ${name}.elf
            USES_TERMINAL
            COMMENT "J-Link GDB server on port ${GDB_PORT} for ${name}.elf")
endfunction()
