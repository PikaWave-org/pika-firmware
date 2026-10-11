#include "ch.h"
#include "hal.h"

#include "SDCard.h"

#include <pika/fatfs_wrapper/fatfsWrapper.h>

namespace pika::storage {
namespace {

// 4 bit bus.
const SDCConfig sdc_cfg = {SDC_MODE_4BIT, 0};

// 512 byte sectors to MiB.
constexpr uint32_t sectors_to_mb(uint32_t sectors) { return sectors / 2048U; }

}// namespace

bool SDCard::mount() {
    if (mounted_) {
        return true;
    }
    if (not started_) {
        sdcStart(&SDCD1, &sdc_cfg);
        started_ = true;
    }
    if (sdcConnect(&SDCD1) != HAL_SUCCESS) {
        last_error_ = FR_NOT_READY;
        return false;
    }
    last_error_ = wf_mount(&fs_, "", 1);
    if (last_error_ != FR_OK) {
        sdcDisconnect(&SDCD1);
        return false;
    }
    mounted_ = true;
    return true;
}

void SDCard::unmount() {
    if (mounted_) {
        wf_mount(nullptr, "", 0);
        mounted_ = false;
    }
    if (started_) {
        sdcDisconnect(&SDCD1);
    }
}

uint32_t SDCard::capacity_mb() const {
    if (blkGetDriverState(&SDCD1) != BLK_READY) {
        return 0;
    }
    return sectors_to_mb(mmcsdGetCardCapacity(&SDCD1));
}

uint32_t SDCard::free_mb() {
    if (not mounted_) {
        return 0;
    }
    DWORD clusters;
    FATFS *fs;
    if (wf_getfree("", &clusters, &fs) != FR_OK) {
        return 0;
    }
    return sectors_to_mb(clusters * fs->csize);
}

}// namespace pika::storage
