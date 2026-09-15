pika_add_firmware(firmware
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${SRC_ROOT}/pika/log.cpp
                ${SRC_ROOT}/pika/audio/DACSpeaker.cpp
                ${SRC_ROOT}/pika/st75160.cpp
                ${SRC_ROOT}/pika/ublox.cpp)

# Sound effects, generated from sounds/ into the build tree at build time.
pika_add_audio_sample(firmware.elf SOUND "${PROJECT_ROOT}/sounds/meow.mp3")
