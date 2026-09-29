pika_add_firmware(firmware
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${SRC_ROOT}/pika/log.cpp
                ${SRC_ROOT}/pika/buttons.cpp
                ${SRC_ROOT}/pika/audio/ADCMicrophone.cpp
                ${SRC_ROOT}/pika/audio/DACSpeaker.cpp
                ${SRC_ROOT}/pika/audio/MelpRecorder.cpp
                ${SRC_ROOT}/pika/audio/MicLevel.cpp
                ${SRC_ROOT}/pika/audio/resample.cpp
                ${SRC_ROOT}/pika/audio/SampleClock.cpp
                ${SRC_ROOT}/pika/lcd/ST75160.cpp
                ${SRC_ROOT}/pika/radio/SX1262.cpp
                ${SRC_ROOT}/pika/App.cpp
                ${SRC_ROOT}/pika/ublox.cpp)

set(MESSGEN_TYPE_DIR "${CMAKE_CURRENT_LIST_DIR}/msg/types")
set(MESSGEN_PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/msg/protocols")

messgen_add_types_library(msgs_types "${PROJECT_ROOT}/msg/types" "cpp_standard=23")
messgen_add_proto_library(msgs_proto_mac "${PROJECT_ROOT}/msg/proto" "pika/proto/mac" msgs_types)

target_compile_definitions(firmware.elf PRIVATE CHPRINTF_USE_FLOAT=TRUE)

# Sound effects, generated from sounds/ into the build tree at build time.
pika_add_sound(firmware.elf SOUND "${PROJECT_ROOT}/sounds/meow.mp3")

# Splash screen, generated from images/ into the build tree at build time.
pika_add_image(firmware.elf IMAGE "${PROJECT_ROOT}/images/pika_logo.bmp")

# MELPe speech codec, called at 2400 bps by MelpRecorder. --gc-sections still
# drops npp() and its 36KB of code plus 15.6KB of state, since nothing calls
# melpe_n()/melpe_a(); after a change here, check that it still does:
#   arm-none-eabi-nm firmware.elf | grep -i npp
target_link_libraries(firmware.elf PRIVATE melpe msgs_types msgs_proto_mac)
