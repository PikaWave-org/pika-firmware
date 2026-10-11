// FatFS disk I/O on SDCD1. Transfers go through a bounce buffer the SDMMC1 DMA
// can reach, so callers may pass any buffer.

#include <string.h>

#include "hal.h"

// Must precede diskio.h.
#include "ff.h"

#include "diskio.h"

// Sectors per transfer.
#define BOUNCE_SECTORS 4U

static uint8_t bounce[BOUNCE_SECTORS * MMCSD_BLOCK_SIZE] __attribute__((section(".sdmmc"), aligned(32)));

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0U || blkGetDriverState(&SDCD1) != BLK_READY)
        return STA_NOINIT;
    return 0;
}

// The card is connected before f_mount().
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (disk_status(pdrv) != 0U)
        return RES_NOTRDY;
    while (count > 0U) {
        UINT n = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;
        if (sdcRead(&SDCD1, sector, bounce, n) != HAL_SUCCESS)
            return RES_ERROR;
        memcpy(buff, bounce, n * MMCSD_BLOCK_SIZE);
        buff += n * MMCSD_BLOCK_SIZE;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (disk_status(pdrv) != 0U)
        return RES_NOTRDY;
    while (count > 0U) {
        UINT n = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;
        memcpy(bounce, buff, n * MMCSD_BLOCK_SIZE);
        if (sdcWrite(&SDCD1, sector, bounce, n) != HAL_SUCCESS)
            return RES_ERROR;
        buff += n * MMCSD_BLOCK_SIZE;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (disk_status(pdrv) != 0U)
        return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            return sdcSync(&SDCD1) == HAL_SUCCESS ? RES_OK : RES_ERROR;
        case GET_SECTOR_COUNT:
            *(LBA_t *) buff = mmcsdGetCardCapacity(&SDCD1);
            return RES_OK;
        case GET_BLOCK_SIZE:
            // Unknown.
            *(DWORD *) buff = 1U;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
