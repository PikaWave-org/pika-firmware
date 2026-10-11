# FatFS calls from any thread, run on one worker thread. Only the wrapper is
# built: its fatfs_diskio.c hands caller buffers straight to the SD DMA, which
# storage/diskio.c exists to avoid.
set(PIKA_FATFS_WRAPPER_SOURCES "${CMAKE_CURRENT_LIST_DIR}/fatfsWrapper.c")
