# microSD card on FatFS; the target adds FatFS itself with pika_add_fatfs().
file(GLOB PIKA_STORAGE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/*.c")
