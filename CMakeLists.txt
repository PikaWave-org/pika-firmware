cmake_minimum_required(VERSION 3.20)

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SRC_ROOT "${PROJECT_ROOT}/src")
set(CHIBIOS_ROOT "${PROJECT_ROOT}/submodules/ChibiOS")

include(${PROJECT_ROOT}/cmake/toolchain-chibios.cmake)

project(pika C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${PROJECT_ROOT}/cmake/chibios.cmake)
include(${PROJECT_ROOT}/cmake/audio.cmake)
include(${PROJECT_ROOT}/cmake/melpe.cmake)
include(${PROJECT_ROOT}/boards/pika_go_0/board.cmake)

add_subdirectory(targets/firmware)
add_subdirectory(targets/test)
