# microSD card and FatFS, unpacked from the ChibiOS archive at configure time.
set(FATFS_ARCHIVE "${CHIBIOS_ROOT}/ext/fatfs-0.14b_patched.7z")
set(FATFS_ROOT "${CMAKE_BINARY_DIR}/fatfs/source")
if (NOT EXISTS "${FATFS_ROOT}/ff.c")
    file(ARCHIVE_EXTRACT INPUT "${FATFS_ARCHIVE}" DESTINATION "${CMAKE_BINARY_DIR}"
            PATTERNS "fatfs/source/*")
endif ()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FATFS_ARCHIVE}")

set(FATFS_SOURCES "${FATFS_ROOT}/ff.c" "${FATFS_ROOT}/ffunicode.c")
set(FATFS_INCLUDE_DIRS "${FATFS_ROOT}")
set(FATFS_COMPILE_OPTIONS -w)

file(GLOB PIKA_STORAGE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/*.c")
