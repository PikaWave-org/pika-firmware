# MELPe 1200 bps speech codec, vendored in src/melpe/.
#
# Built as a static library rather than folded into the firmware sources: it
# is third party C that we do not want compiled with our warning flags, and
# keeping it separate makes the one place its compile options are set obvious.
#
# Nothing links it in yet beyond the firmware image, and --gc-sections drops
# every object nobody calls, so carrying it costs no flash until the first
# caller appears. When one does, expect roughly 188KB of flash and 27KB of
# RAM (the codebooks are const and live in flash; the RAM is npp's state).
#
# The file list is spelled out for the same reason the ChibiOS one is: a glob
# does not re-run when a file appears, and this set only changes when we pull
# a new upstream.

set(MELPE_ROOT "${SRC_ROOT}/melpe")

add_library(melpe STATIC
        "${MELPE_ROOT}/classify.c"
        "${MELPE_ROOT}/coeff.c"
        "${MELPE_ROOT}/dsp_sub.c"
        "${MELPE_ROOT}/fec_code.c"
        "${MELPE_ROOT}/fft_lib.c"
        "${MELPE_ROOT}/fs_lib.c"
        "${MELPE_ROOT}/fsvq_cb.c"
        "${MELPE_ROOT}/global.c"
        "${MELPE_ROOT}/harm.c"
        "${MELPE_ROOT}/lpc_lib.c"
        "${MELPE_ROOT}/mat_lib.c"
        "${MELPE_ROOT}/math_lib.c"
        "${MELPE_ROOT}/mathdp31.c"
        "${MELPE_ROOT}/mathhalf.c"
        "${MELPE_ROOT}/melp_ana.c"
        "${MELPE_ROOT}/melp_chn.c"
        "${MELPE_ROOT}/melp_sub.c"
        "${MELPE_ROOT}/melp_syn.c"
        "${MELPE_ROOT}/melpe.c"
        "${MELPE_ROOT}/msvq_cb.c"
        "${MELPE_ROOT}/npp.c"
        "${MELPE_ROOT}/pit_lib.c"
        "${MELPE_ROOT}/pitch.c"
        "${MELPE_ROOT}/postfilt.c"
        "${MELPE_ROOT}/qnt12.c"
        "${MELPE_ROOT}/qnt12_cb.c"
        "${MELPE_ROOT}/vq_lib.c")

# PUBLIC ${SRC_ROOT} so a caller reaches the entry points as "melpe/melpe.h";
# PRIVATE ${MELPE_ROOT} because the codec's own files include their siblings
# by bare name.
target_include_directories(melpe
        PUBLIC "${SRC_ROOT}"
        PRIVATE "${MELPE_ROOT}")

# Same MCU flags as the firmware, or the static library links against an
# image built for a different float ABI. Trailing -w drops the -Wall -Wextra
# -Wundef those carry: this is frozen upstream code, and its warnings (an
# undefined OVERFLOW_CHECK, unused parameters) are noise we will not act on,
# which would train us to ignore the warnings from our own code beside it.
#
# -O2 is forced rather than inherited from CMAKE_BUILD_TYPE. A Debug build
# compiles this at -O0, where the codec costs 557KB of flash instead of 232KB
# and is far too slow to keep up with 8kHz audio, so a debug image could not
# run it at all. The cost is that stepping through the codec in gdb is not
# useful; that is a fair trade for third party code we do not debug.
#
# -Os would save a further 46KB (186KB against 232KB). -O2 is the pick because
# flash is the plentiful resource here - 1MB, of which the firmware uses 121KB
# - and the codec has a real time deadline. Neither level has been timed on
# the board yet, so if it turns out to fit comfortably, -Os is free flash.
target_compile_options(melpe PRIVATE ${MCU_COMPILE_FLAGS} -w -O2)
target_compile_definitions(melpe PRIVATE CORTEX_USE_FPU=TRUE)

# The codec calls floor() from classify.c and the 40 bit shifts.
target_link_libraries(melpe PUBLIC m)
