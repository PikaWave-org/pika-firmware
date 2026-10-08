#pragma once

#include <hal.h>

#include <cstdio>
#include <cstdint>

namespace pika::usb {

/// Abstract class for serial data port like UART or USB ACM.
/**
 * Allows writing interface-independent applications like telemetry driver that can use UART or USB ACM or other implementation.
 */
class DataPort {
public:
    virtual ~DataPort() = default;

    /// Write data.
    /**
     * Always non-blocking.
     * @return number of bytes written, possible 0, or -1 on error.
     */
    virtual ssize_t write(const uint8_t *buf, size_t n) = 0;

    /// Read data.
    /**
     * Always non-blocking.
     * @return number of bytes read, possible 0, or -1 on error.
     */
    virtual ssize_t read(uint8_t *buf, size_t n) = 0;

    /// Close port.
    virtual void close() = 0;

    /// Check if port is open
    virtual bool is_open() = 0;

    /// Check if port is connected (ready to read/write data)
    virtual bool is_connected() { return true; }

    [[nodiscard]] virtual const event_source_t *event_source() const = 0;

    [[nodiscard]] virtual event_source_t *event_source() = 0;

    [[nodiscard]] virtual const char *get_driver_name() const = 0;
};

/**
 * @}
 */

}// namespace pika::drivers
