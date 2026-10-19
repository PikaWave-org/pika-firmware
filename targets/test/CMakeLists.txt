pika_add_firmware(test
        BOARD pika_go_0
        SOURCES
                main.cpp
                ${PROJECT_ROOT}/src/pika/buttons.cpp
                ${PIKA_LOG_SOURCES})
