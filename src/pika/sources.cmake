set(PIKA_LOG_SOURCES "${CMAKE_CURRENT_LIST_DIR}/log.cpp")

file(GLOB PIKA_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
list(REMOVE_ITEM PIKA_CORE_SOURCES ${PIKA_LOG_SOURCES})

include("${CMAKE_CURRENT_LIST_DIR}/app/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/audio/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fatfs_wrapper/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/lcd/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/power/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/radio/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/storage/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/usb/sources.cmake")
