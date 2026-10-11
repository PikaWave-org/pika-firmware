# The source lists come from src/sources.cmake.
pika_add_firmware(firmware
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${PIKA_LOG_SOURCES}
                ${PIKA_CORE_SOURCES}
                ${PIKA_APP_SOURCES}
                ${PIKA_AUDIO_SOURCES}
                ${PIKA_LCD_SOURCES}
                ${PIKA_POWER_SOURCES}
                ${PIKA_USB_SOURCES}
                ${PIKA_RADIO_SOURCES}
                ${PIKA_STORAGE_SOURCES}
                ${PIKA_FATFS_WRAPPER_SOURCES}
                ${MELPE_SOURCES}
                ${TINYUSB_SOURCES})

target_include_directories(firmware.elf SYSTEM PRIVATE ${TINYUSB_INCLUDE_DIRS})

pika_add_fatfs(firmware.elf)

set_source_files_properties(${MELPE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${MELPE_COMPILE_OPTIONS}"
        INCLUDE_DIRECTORIES "${MELPE_ROOT}")
set_source_files_properties(${TINYUSB_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${TINYUSB_COMPILE_OPTIONS}")

target_compile_definitions(firmware.elf PRIVATE CHPRINTF_USE_FLOAT=TRUE)

pika_add_sound(firmware.elf SOUND "${PROJECT_ROOT}/sounds/meow.mp3")
pika_add_image(firmware.elf IMAGE "${PROJECT_ROOT}/images/pika_logo.bmp")

target_link_libraries(firmware.elf PRIVATE msgs_types msgs_proto_mac msgs_proto_app m)
