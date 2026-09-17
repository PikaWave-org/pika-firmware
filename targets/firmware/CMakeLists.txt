pika_add_firmware(firmware
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${SRC_ROOT}/pika/log.cpp
                ${SRC_ROOT}/pika/buttons.cpp
                ${SRC_ROOT}/pika/audio/ADCMicrophone.cpp
                ${SRC_ROOT}/pika/audio/DACSpeaker.cpp
                ${SRC_ROOT}/pika/audio/MicLevel.cpp
                ${SRC_ROOT}/pika/audio/SampleClock.cpp
                ${SRC_ROOT}/pika/lcd/ST75160.cpp
                ${SRC_ROOT}/pika/ui.cpp
                ${SRC_ROOT}/pika/ublox.cpp)

target_compile_definitions(firmware.elf PRIVATE CHPRINTF_USE_FLOAT=TRUE)

# Sound effects, generated from sounds/ into the build tree at build time.
pika_add_sound(firmware.elf SOUND "${PROJECT_ROOT}/sounds/meow.mp3")

# MELPe 1200 bps speech codec. Nothing calls it yet, and --gc-sections keeps
# the unreferenced objects out of the image, so this is free until it is used.
target_link_libraries(firmware.elf PRIVATE melpe)
