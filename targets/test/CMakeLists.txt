pika_add_firmware(test
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${SRC_ROOT}/pika/log.cpp)

# The MELPe 2400 bench. 8kHz because that is the rate the codec works at and
# what tools/melpe-check encodes on the host, so the CRCs are comparable.
pika_add_sound(test.elf SOUND "${PROJECT_ROOT}/sounds/meow.mp3" NAME meow8k RATE 8000)

target_link_libraries(test.elf PRIVATE melpe)
