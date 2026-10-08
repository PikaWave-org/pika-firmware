#pragma once

#include <pika/drivers/TTYPort.h>

#include "tusb.h"

namespace pika::usb {
/**
 * @addtogroup tinyusb_cdc_module
 * @{ 
 */

class TinyUSBPort : public TTYPort {
public:
    TinyUSBPort();

    ssize_t write(const uint8_t *buf, size_t n) override;

    ssize_t read(uint8_t *buf, size_t n) override;

    void close() override;

    bool is_open() override;

    [[nodiscard]] const event_source_t *event_source() const override;

    [[nodiscard]] event_source_t *event_source() override;

    [[nodiscard]] const char *get_driver_name() const override;
};

/**
 * @}
 */
} // namespace pika::drivers
