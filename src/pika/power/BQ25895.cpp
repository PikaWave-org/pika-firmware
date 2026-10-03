#include "ch.h"
#include "hal.h"

#include "BQ25895.h"

#include <algorithm>

namespace pika::power {
namespace {

/* Registers, as the datasheet numbers them. */
constexpr uint8_t reg_adc_ctrl = 0x02U; /* CONV_START, CONV_RATE, ...   */
constexpr uint8_t reg_ichg = 0x04U;     /* EN_PUMPX, ICHG[6:0]          */
constexpr uint8_t reg_vreg = 0x06U;     /* VREG[7:2], BATLOWV, VRECHG   */
constexpr uint8_t reg_timer = 0x07U;    /* EN_TERM, WATCHDOG[5:4], ...  */
constexpr uint8_t reg_status = 0x0BU;   /* VBUS_STAT, CHRG_STAT, PG ... */
constexpr uint8_t reg_fault = 0x0CU;
constexpr uint8_t reg_batv = 0x0EU; /* first of five ADC results    */
constexpr uint8_t reg_part = 0x14U; /* REG_RST, PN[5:3], DEV_REV    */

constexpr uint8_t conv_start = 0x80U;
constexpr uint8_t conv_rate = 0x40U;
constexpr uint8_t watchdog_mask = 0x30U;
constexpr uint8_t pn_mask = 0x38U;
constexpr uint8_t pn_bq25895 = 0x38U;

/* Synthetic last_error() values, outside the i2cflags_t range. */
constexpr uint32_t err_bus_stuck = 0x80000000U;
constexpr uint32_t err_start = 0x40000000U;
constexpr uint32_t err_part = 0x20000000U;
constexpr uint32_t err_adc_timeout = 0x10000000U;

/*
 * Bus timing from the 130MHz I2C4 kernel clock (PCLK4). PRESC=12 divides it
 * to 10MHz, so one TIMINGR tick is 100ns.
 *
 * Standard mode, 100kHz: SCLL=49 is 5us low, SCLH=39 is 4us high, SCLDEL=4 is
 * 500ns of data setup and SDADEL=2 is 200ns of hold. The chip would take
 * 400kHz, but the pull-ups are 10k, too weak for fast mode's 300ns rise time,
 * and a status read is a handful of bytes.
 */
const I2CConfig i2c_cfg = {STM32_TIMINGR_PRESC(12U) | STM32_TIMINGR_SCLDEL(4U) | STM32_TIMINGR_SDADEL(2U) |
                                   STM32_TIMINGR_SCLH(39U) | STM32_TIMINGR_SCLL(49U),
                           0U, 0U};

/*
 * Transfer buffers, in AHB SRAM4 through the .ram4 section (0x38000000).
 *
 * I2C4 sits in the D3 domain and its DMA controller is the BDMA, which can
 * reach SRAM4 and nothing else - not the D2 .nocache buffers the LCD uses,
 * and certainly not a thread stack in DTCM. SRAM4 is cacheable by default, so
 * board.c also puts an MPU region over it to keep the CPU and the BDMA looking
 * at the same bytes. Get either half wrong and transfers report MSG_OK while
 * moving nothing; see memory/i2c-dma-silently-broken.md.
 */
#define BDMA_BUF __attribute__((section(".ram4"), aligned(4)))

BDMA_BUF uint8_t tx_buf[2];
BDMA_BUF uint8_t rx_buf[5]; /* the longest read is the five ADC results */

} /* anonymous namespace */

bool BQ25895::xfer(size_t tx_len, size_t rx_len) {
    /* Same recovery as ST75160: a timed out transfer leaves the driver in
     I2C_LOCKED, where any further call asserts, so restart it first.*/
    if (cfg_.i2c->state != I2C_READY) {
        i2cStop(cfg_.i2c);
        if (i2cStart(cfg_.i2c, &i2c_cfg) != HAL_RET_SUCCESS) {
            error_flags_ = err_start;
            return false;
        }
    }

    msg_t msg = i2cMasterTransmitTimeout(cfg_.i2c, cfg_.addr, tx_buf, tx_len, (rx_len > 0U) ? rx_buf : nullptr, rx_len,
                                         TIME_MS2I(20));
    if (msg != MSG_OK) {
        error_flags_ = (uint32_t) i2cGetErrors(cfg_.i2c);
        if (error_flags_ == 0U) {
            error_flags_ = err_bus_stuck;
        }
        return false;
    }
    return true;
}

bool BQ25895::read(uint8_t reg, uint8_t *out, size_t n) {
    tx_buf[0] = reg;
    if (!xfer(1U, n)) {
        return false;
    }
    std::copy_n(rx_buf, n, out);
    return true;
}

bool BQ25895::write(uint8_t reg, uint8_t value) {
    tx_buf[0] = reg;
    tx_buf[1] = value;
    return xfer(2U, 0U);
}

bool BQ25895::update(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t v;
    if (!read(reg, &v, 1U)) {
        return false;
    }
    return write(reg, (uint8_t) ((v & ~mask) | (value & mask)));
}

void BQ25895::int_cb(void *arg) {
    chSysLockFromISR();
    chBSemSignalI(&static_cast<BQ25895 *>(arg)->int_sem_);
    chSysUnlockFromISR();
}

bool BQ25895::init() {
    error_flags_ = 0U;

    /* First, so main() has something to wait on even if the chip never
       answers: it then sleeps rather than retrying a dead bus. */
    chBSemObjectInit(&int_sem_, true);

    if (i2cStart(cfg_.i2c, &i2c_cfg) != HAL_RET_SUCCESS) {
        error_flags_ = err_start;
        return false;
    }

    uint8_t part;
    if (!read(reg_part, &part, 1U)) {
        return false;
    }
    if ((part & pn_mask) != pn_bq25895) {
        error_flags_ = err_part;
        return false;
    }

    if (!update(reg_timer, watchdog_mask, 0U)) {
        return false;
    }

    const uint16_t ichg = std::min<uint16_t>(cfg_.charge_current_ma, 5056U) / 64U;
    if (!update(reg_ichg, 0x7FU, (uint8_t) ichg)) {
        return false;
    }

    const uint16_t vreg_mv = std::clamp<uint16_t>(cfg_.charge_voltage_mv, 3840U, 4608U);
    if (!update(reg_vreg, 0xFCU, (uint8_t) (((vreg_mv - 3840U) / 16U) << 2U))) {
        return false;
    }

    /* Disable AUTO_DPDM */
    if (!update(0x02, 0x0DU, 0)) {
        return false;
    }

    /* Set ILIM = 2A */
    if (!update(0x00, 0x3FU, 0x28)) {
        return false;
    }

    /* Throw away whatever faults latched before we were listening, so the
     first read_status() reports the present rather than the boot.*/
    uint8_t stale;
    if (!read(reg_fault, &stale, 1U)) {
        return false;
    }

    palSetLineCallback(cfg_.int_line, int_cb, this);
    palEnableLineEvent(cfg_.int_line, PAL_EVENT_MODE_FALLING_EDGE);

    return true;
}

void BQ25895::main() {
    init();
    for (;;) {
        chBSemWaitTimeout(&int_sem_, TIME_S2I(1));
        update();
    }
}

bool BQ25895::update() {
    uint8_t r[5];
    if (!read(reg_status, r, 2U)) {
        return false;
    }
    status_.input_connected = (r[0] >> 5U) != 0;
    status_.charging = (Charge) ((r[0] >> 3U) & 0x03U);

    /* One shot rather than the 1s continuous mode: the ADC draws current,
     and on battery that is current for a reading nobody asked for.*/
    if (!update(reg_adc_ctrl, conv_start | conv_rate, conv_start)) {
        return false;
    }

    /* CONV_START reads back 1 until the conversion is done. Bounded and
     sleeping, never spinning: see memory/chibios-busy-wait-starves-threads.*/
    uint8_t ctrl = conv_start;
    for (int i = 0; (i < 100) && ((ctrl & conv_start) != 0U); i++) {
        chThdSleepMilliseconds(10);
        if (!read(reg_adc_ctrl, &ctrl, 1U)) {
            return false;
        }
    }
    if ((ctrl & conv_start) != 0U) {
        error_flags_ = err_adc_timeout;
        return false;
    }

    /* REG0E..REG12: BATV, SYSV, TSPCT, VBUSV, ICHGR. */
    if (!read(reg_batv, r, 5U)) {
        return false;
    }

    status_.voltage = (2304U + 20U * (r[0] & 0x7FU));
    status_.charge_current = (50U * (r[4] & 0x7FU));
    return true;
}

} /* namespace pika::power */
