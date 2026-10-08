#pragma once

#include <pika/drivers/DataPort.h>

#include <cstdint>

namespace pika::usb {

/// Abstract class for TTY port like UART or USB ACM.
class TTYPort : public DataPort {
public:
    ~TTYPort() override = default;

    virtual int open() = 0;

    enum class SerialParity { NONE, EVEN, ODD };

    enum class SerialStopBits { ONE, TWO, HALF, ONE_AND_HALF };

    using Parity = SerialParity;
    using StopBits = SerialStopBits;

    static constexpr uint32_t SERIAL_DEFAULT_BAUDRATE = 9600;
    static constexpr uint32_t DEFAULT_BAUDRATE = SERIAL_DEFAULT_BAUDRATE;

    virtual int configure(uint32_t baudrate,
                          Parity parity = Parity::NONE,
                          StopBits stop_bits = StopBits::ONE) {
        _baudrate = baudrate;
        _parity = parity;
        _stop_bits = stop_bits;
        return 0;
    }

    virtual uint32_t get_baudrate() const {
        return _baudrate;
    }

protected:
    uint32_t _baudrate = DEFAULT_BAUDRATE;
    Parity _parity = Parity::NONE;
    StopBits _stop_bits = StopBits::ONE;
};

/**
 * @}
 */
} // namespace pika::drivers
