pika_add_firmware(test
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${PIKA_LOG_SOURCES}
                ${PIKA_RADIO_SOURCES}
                ${MELPE_SOURCES})

# Third party code: its own options and private include directory on its
# files alone, see src/melpe/sources.cmake. Source properties are scoped to
# the directory that creates the target, so they are set here. libm is for
# the codec's floor().
set_source_files_properties(${MELPE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${MELPE_COMPILE_OPTIONS}"
        INCLUDE_DIRECTORIES "${MELPE_ROOT}")
target_link_libraries(test.elf PRIVATE m)

# The MELPe 2400 bench. 8kHz because that is the rate the codec works at and
# what tools/melpe-check encodes on the host, so the CRCs are comparable.
pika_add_sound(test.elf SOUND "${PROJECT_ROOT}/sounds/meow.mp3" NAME meow8k RATE 8000)

