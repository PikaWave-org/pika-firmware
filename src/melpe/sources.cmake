# MELPe speech codec, vendored in src/melpe/. Upstream wraps 1200 bps; patch 6
# adds the 2400 bps entry points, which is the rate the board records at.
#
# Compiled straight into the firmware target that uses it, like everything
# else; what sets it apart is MELPE_COMPILE_OPTIONS below, which the target
# puts on these files alone, together with MELPE_ROOT as their private include
# directory.
#
# Measured in the firmware image that calls it: 186KB of flash and 10.9KB of
# RAM. The codebooks are const and live in flash, and npp() is dropped by
# --gc-sections, which is most of the gap to the library's own 26KB.
#
# Every .c here is part of the codec, so the sources are globbed like the
# pika modules; CONFIGURE_DEPENDS re-runs the glob when a new upstream adds or
# drops a file.

set(MELPE_ROOT "${CMAKE_CURRENT_LIST_DIR}")

file(GLOB MELPE_SOURCES CONFIGURE_DEPENDS "${MELPE_ROOT}/*.c")

# A caller reaches the entry points as "melpe/melpe.h" through the src/
# include path every firmware target has. MELPE_ROOT goes on the codec's own
# files only: they include their siblings by bare name, and global.h or
# macro.h on everyone's include path would shadow anything else of that name.
#
# -w drops the firmware's -Wall -Wextra -Wundef for these files: this is
# frozen upstream code, and its warnings (an undefined OVERFLOW_CHECK, unused
# parameters) are noise we will not act on, which would train us to ignore the
# warnings from our own code beside it.
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
set(MELPE_COMPILE_OPTIONS -w -O2)

# The codec calls floor() from classify.c and the 40 bit shifts, so a target
# that compiles it links libm.
