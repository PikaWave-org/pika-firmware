cmake_minimum_required(VERSION 3.20)

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SRC_ROOT "${PROJECT_ROOT}/src")
set(CHIBIOS_ROOT "${PROJECT_ROOT}/submodules/ChibiOS")

include(${PROJECT_ROOT}/cmake/toolchain-chibios.cmake)

project(pika C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${PROJECT_ROOT}/cmake/chibios.cmake)
include(${PROJECT_ROOT}/cmake/ff.cmake)
include(${PROJECT_ROOT}/cmake/audio.cmake)
include(${PROJECT_ROOT}/cmake/images.cmake)
include(${PROJECT_ROOT}/boards/pika_go_0/board.cmake)
include(${PROJECT_ROOT}/src/sources.cmake)

include("submodules/messgen/integrations/cmake/messgen.cmake")
set(MESSGEN_TYPE_DIR "${CMAKE_CURRENT_LIST_DIR}/msg/types")
set(MESSGEN_PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/msg/protocols")
messgen_add_types_library(msgs_types "${PROJECT_ROOT}/msg/types" "cpp_standard=23")
messgen_add_proto_library(msgs_proto_mac "${PROJECT_ROOT}/msg/proto" "pika/proto/mac" msgs_types)

add_subdirectory(targets/firmware)
add_subdirectory(targets/test)
