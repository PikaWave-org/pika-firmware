# FatFS (ChaN), from the archive ChibiOS ships in ext/.
#
# pika_add_fatfs(<target>)
#
# Adds FatFS to <target>, unpacked into the calling directory's build tree and
# re-unpacked whenever the archive is newer. The target provides ffconf.h and
# the disk I/O functions.
set(FATFS_ARCHIVE "${CHIBIOS_ROOT}/ext/fatfs-0.14b_patched.7z")

function(pika_add_fatfs target)
    set(root "${CMAKE_CURRENT_BINARY_DIR}/fatfs/source")
    set(sources "${root}/ff.c" "${root}/ffunicode.c")
    set(headers "${root}/ff.h" "${root}/diskio.h")

    # Extraction keeps the archive's own old timestamps, hence the touch.
    add_custom_command(OUTPUT ${sources} ${headers}
            COMMAND ${CMAKE_COMMAND} -E tar xf "${FATFS_ARCHIVE}" fatfs/source
            COMMAND ${CMAKE_COMMAND} -E touch ${sources} ${headers}
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            DEPENDS "${FATFS_ARCHIVE}"
            COMMENT "Unpacking FatFS")

    target_sources(${target} PRIVATE ${sources} ${headers})
    target_include_directories(${target} SYSTEM PRIVATE "${root}")
    set_source_files_properties(${sources} PROPERTIES COMPILE_OPTIONS -w)
endfunction()
