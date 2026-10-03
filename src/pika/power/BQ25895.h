/*
 * Driver for the TI BQ25895 single cell charger on I2C.
 *
 * The chip charges on its own: CE is strapped low on this board, so it charges
 * whenever VBUS is present whether or not the MCU ever talks to it. What the
 * driver adds is the charge settings, the status and fault registers, and the
 * chip's ADC - battery, system and VBUS voltage, charge current, and the TS
 * (thermistor) reading.
 *
 * Usage:
 *
 *   static const pika::power::BQ25895::Config chg_cfg = {
 *     &BOARD_CHG_I2C, BOARD_CHG_I2C_ADDR, LINE_CHG_INT,
 *     BOARD_CHG_ICHG_MA, BOARD_CHG_VREG_MV
 *   };
 *   static pika::power::BQ25895 charger{chg_cfg};
 *
 * @note  Like ST75160, the transfer buffers are file-static because their
 *        placement is what makes DMA work (see the .cpp), so one instance is
 *        the supported case, used from one thread.
 */

#pragma once

#include "BatteryStatus.h"

#include <ch.hpp>
#include <hal.h>

#include <cstdint>

namespace pika::power {

class BQ25895 : public chibios_rt::BaseStaticThread<1024> {
public:
    /**
     * @brief   How the charger is wired up and what to charge with, taken from
     *          the board header.
     */
    struct Config {
        I2CDriver *i2c;             /**< Bus the charger is on.                 */
        i2caddr_t addr;             /**< 7 bit slave address.                   */
        ioline_t int_line;          /**< ~INT, active low pulse, open drain.    */
        uint16_t charge_current_ma; /**< Fast charge current, 64mA steps.       */
        uint16_t charge_voltage_mv; /**< Charge voltage, 16mV steps from 3840.  */
    };

    /* Fault bits in Status::faults, which is REG0C as read. */
    static constexpr uint8_t fault_watchdog = 0x80U;
    static constexpr uint8_t fault_boost = 0x40U;
    static constexpr uint8_t fault_charge_mask = 0x30U; /* 1 input, 2 thermal, 3 timer */
    static constexpr uint8_t fault_battery = 0x08U;     /* BAT overvoltage            */
    static constexpr uint8_t fault_ntc_mask = 0x07U;    /* 2 warm, 3 cool, 5 cold, 6 hot */

    explicit BQ25895(const Config &cfg) : cfg_(cfg) {}

    /**
     * @brief   Checks the part number, disables the I2C watchdog, programs the
     *          charge current and voltage, and arms the interrupt line.
     * @return  false if the chip did not answer or is not a BQ25895, see
     *          last_error().
     */
    bool init();

    /**
     * @brief   The charger thread: init(), then update() once and again each
     *          time ~INT pulses, sleeping on int_sem_ in between.
     */
    void main() override;

    /** @brief  I2C error flags from the last failed transaction, 0 if none. */
    uint32_t last_error() const { return error_flags_; }

    const BatteryStatus &status() const { return status_; };

private:
    bool xfer(size_t tx_len, size_t rx_len);
    bool read(uint8_t reg, uint8_t *out, size_t n);
    bool write(uint8_t reg, uint8_t value);
    bool update(uint8_t reg, uint8_t mask, uint8_t value);

    static void int_cb(void *arg);

    Config cfg_;
    uint32_t error_flags_ = 0U;
    /* Signalled from the ~INT edge, which the chip pulses on any fault and on
       every charge or input state change. Binary: pulses that land while an
       update() is running collapse into one more update(), which then reads
       the latest state anyway. */
    binary_semaphore_t int_sem_;

    BatteryStatus status_{};

    /**
     * @brief   Reads the status and fault registers.
     * @note    The fault register latches: the first read after a fault
     *          reports it and clears it, a second read reports the current
     *          state. A fault therefore shows up exactly once here.
     */
    bool update();
};

} /* namespace pika::power */
