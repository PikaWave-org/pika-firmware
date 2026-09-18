pika_add_firmware(test
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${SRC_ROOT}/pika/log.cpp)

# The MELPe 2400 bench, fed from a clip rather than the microphone: the
# converters run at 32kHz, so a mic fed bench would measure the resampler too,
# and 8kHz material is what tools/melpe-check encodes on the host - which is
# what makes the CRC the bench prints comparable to anything.
pika_add_sound(test.elf SOUND "${PROJECT_ROOT}/sounds/meow.mp3" NAME meow8k RATE 8000)

target_link_libraries(test.elf PRIVATE melpe)
