set(TINYUSB_ROOT "${PROJECT_ROOT}/submodules/tinyusb/src")

include("${TINYUSB_ROOT}/CMakeLists.txt")
tinyusb_sources_get(TINYUSB_SOURCES)
list(APPEND TINYUSB_SOURCES
        "${TINYUSB_ROOT}/portable/synopsys/dwc2/dcd_dwc2.c"
        "${TINYUSB_ROOT}/portable/synopsys/dwc2/dwc2_common.c")

set(TINYUSB_INCLUDE_DIRS "${TINYUSB_ROOT}" "${CMAKE_CURRENT_LIST_DIR}")

set(TINYUSB_COMPILE_OPTIONS -w)

file(GLOB PIKA_USB_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/*.c")
