/*
 * microSD card on SDCD1 with a FatFS volume; after mount() use the FatFS API.
 * No card detect: a card inserted later needs mount() again. One thread only.
 */

#pragma once

#include <cstdint>

#include "hal.h"

#include "ff.h"

namespace pika::storage {

class SDCard {
public:
    /** @brief  Connects to the card and mounts its volume. */
    bool mount();

    /** @brief  Unmounts the volume and disconnects the card. */
    void unmount();

    bool mounted() const { return mounted_; }

    /** @brief  Result of the last mount(). */
    FRESULT last_error() const { return last_error_; }

    /** @brief  MiB, 0 when not connected. */
    uint32_t capacity_mb() const;

    /** @brief  Free MiB, 0 when not mounted. */
    uint32_t free_mb();

private:
    FATFS fs_{};
    bool started_ = false;
    bool mounted_ = false;
    FRESULT last_error_ = FR_OK;
};

}// namespace pika::storage
