#include "ch.h"
#include "hal.h"

#include "ST75160.h"

#include <pika/lcd/Font8x8.h>

namespace pika::lcd {
namespace {

/*
 * I2C control bytes. Bit 7 is Co ("another control byte follows the data
 * byte"), bit 6 is A0 (0 = command, 1 = command parameter or display data).
 *
 * Everything here uses the Co=0 form: one control byte, then bytes of that
 * one kind until the STOP. Commands and their parameters therefore go out as
 * separate transactions, which is exactly what Newhaven's own example code
 * for this module does. The Co=1 interleaved form is in the datasheet's
 * timing diagram, but this controller does not appear to honour it: driving
 * init through Co=1 pairs left the panel showing noise, i.e. the command
 * bytes were being taken as display data.
 */
constexpr uint8_t ctrl_data = 0x40U; /* Co=0, A0=1: parameter or pixels */

/* Synthetic last_error() values, outside the i2cflags_t range. */
constexpr uint32_t err_bus_stuck = 0x80000000U;
constexpr uint32_t err_start = 0x40000000U;

/*
 * Bus timing from the 130MHz I2C1 kernel clock (PCLK1). PRESC=12 divides it
 * to 10MHz, so one TIMINGR tick is 100ns.
 *
 * With the 5.1k pull-ups fitted on the board the bus runs at the panel's
 * maximum 400kHz: SCLL=15 is 1.6us low (min 1.3us), SCLH=8 is 0.9us high
 * (min 0.6us), so the period is 2.5us. SCLDEL=3 is 400ns of data setup,
 * SDADEL=1 is 100ns of data hold. 5.1k into the bus capacitance is close to
 * the 300ns rise time fast mode allows, so if transfers ever start erroring
 * this is the first thing to halve (SCLL=49, SCLH=39 gives 100kHz).
 */
const I2CConfig i2c_cfg = {STM32_TIMINGR_PRESC(12U) | STM32_TIMINGR_SCLDEL(3U) | STM32_TIMINGR_SDADEL(1U) |
                                   STM32_TIMINGR_SCLH(8U) | STM32_TIMINGR_SCLL(15U),
                           0U, 0U};

/*
 * Init and addressing scripts. Each entry is a tag, a length and that many
 * bytes; OP_DELAY carries the delay in milliseconds in the length field.
 * Command bytes are sent one transaction each, parameters likewise, matching
 * the reference code for this module.
 */
enum : uint8_t { OP_CMD = 0U, OP_PAR = 1U, OP_DELAY = 2U };

#define CMD(x) OP_CMD, 1U, (uint8_t) (x)
#define PAR(x) OP_PAR, 1U, (uint8_t) (x)
#define DELAY(ms) OP_DELAY, (uint8_t) (ms)

/*
 * Initialization sequence from the NHD-C160100DiZ-FSW-FBW specification.
 */
const uint8_t init_otp[] = {
        CMD(0x31),            /* extension command set 2      */
        CMD(0xD7), PAR(0x9F), /* disable auto read            */
        CMD(0xE0), PAR(0x00), /* enable OTP read              */
        DELAY(10), CMD(0xE3), /* OTP up-load                  */
        DELAY(20), CMD(0xE1)  /* OTP control out              */
};

const uint8_t init_script[] = {
        CMD(0x30),                                   /* extension command set 1      */
        CMD(0x94),                                   /* sleep out                    */
        CMD(0xAE),                                   /* display off                  */
        DELAY(50),  CMD(0x20), PAR(0x0B),            /* power control: VB, VR, VF on */
        DELAY(100), CMD(0x81), PAR(0x08), PAR(0x03), /* Vop = 11.6V                  */
        CMD(0x31),                                   /* extension command set 2      */
        CMD(0x20),                                   /* gray scale levels            */
        PAR(0x00),  PAR(0x00), PAR(0x00), PAR(0x17), PAR(0x17), PAR(0x17), PAR(0x00),
        PAR(0x00),  PAR(0x1D), PAR(0x00), PAR(0x00), PAR(0x1D), PAR(0x1D), PAR(0x1D),
        PAR(0x00),  PAR(0x00), CMD(0x32), PAR(0x00), PAR(0x01), PAR(0x03), /* analog set, bias 1/11      */
        CMD(0x51),  PAR(0xFB),                                             /* booster level x10            */
        CMD(0x30),                                                         /* extension command set 1      */
        CMD(0xF0),  PAR(0x10),                                             /* display mode: monochrome     */
        CMD(0xCA),  PAR(0x00), PAR(0x63), PAR(0x00),                       /* display control, 100 duty  */
        CMD(0xBC),  PAR(0x00),                                             /* data scan direction          */
        CMD(0xA6),                                                         /* normal (not inverted)        */
        CMD(0x31),  CMD(0x40),                                             /* internal power supply        */
        CMD(0x30),                                                         /* extension command set 1      */
        CMD(0x77),                                                         /* enable ICON RAM              */
        CMD(0x15),  PAR(0x00), PAR(0x9F),                                  /* columns 0..159               */
        CMD(0x76),                                                         /* disable ICON RAM             */
        CMD(0x30),                                                         /* extension command set 1      */
        CMD(0x75),  PAR(0x00), PAR(0x18),                                  /* row window                   */
        CMD(0xAF),                                                         /* display on                   */
        DELAY(200)};

/*
 * Addressing preamble sent before every frame. 0x5C rewinds the column and
 * page counters to the start of the window, after which the column address
 * auto-increments per byte and rolls over into the next page, so the whole
 * frame is one stream.
 */
const uint8_t frame_addr[] = {
        CMD(0x30),                                     /* extension command set 1      */
        CMD(0x15), PAR(0x00), PAR(0x9F),               /* columns 0..159               */
        CMD(0x75), PAR(0x00), PAR(ST75160::pages - 1), /* pages 0..12                  */
        CMD(0x5C)                                      /* write data                   */
};

/*
 * Transfer buffers, placed in AHB SRAM2 through the linker script's .nocache
 * section (0x30004000 on this part).
 *
 * Placement is not a detail, and it has two halves, both of which fail
 * silently: i2cMasterTransmitTimeout() returns MSG_OK, the slave ACKs every
 * byte, and nothing of what was meant to be sent arrives.
 *
 *   - The memory has to be in the D2 domain, where DMA1/DMA2 live. Thread
 *     stacks are worse than merely wrong, being in DTCM, which no DMA
 *     controller here can touch.
 *   - It also has to be outside the D-cache, which ChibiOS enables in crt1.c;
 *     otherwise the CPU and the DMA controller look at different data. The
 *     MPU region configured by STM32_NOCACHE_RBAR covers exactly this
 *     section.
 *
 * These buffers are deliberately file-static rather than members of ST75160:
 * the placement above is the whole point of them, and making them members
 * would hand that responsibility to whoever declares the object. The cost is
 * that one instance is the supported case, which is what the board has.
 */
#define DMA_BUF __attribute__((section(".nocache"), aligned(4)))

DMA_BUF uint8_t frame_tx[1 + ST75160::fb_size]; /* control byte + frame */

/*
 * Scratch for run_co1(): a whole script emitted as one transaction with Co=1
 * control bytes, so each command's parameters follow it without an
 * intervening STOP. The vendor's code uses one transaction per byte instead;
 * which of the two this controller actually honours is what the selftest is
 * here to establish.
 */
DMA_BUF uint8_t seq_buf[192];

} /* anonymous namespace */

/*
 * Deliberately does nothing but take the config and work out where the
 * framebuffer starts, both of which are plain address arithmetic.
 *
 * A static instance is constructed before main(), and therefore before
 * halInit(): at that point the MPU region that makes .nocache non-cacheable
 * has not been programmed yet, so any write here would go through the data
 * cache rather than to the memory DMA reads. Nothing needs writing this
 * early - init() sets the control byte along with the rest of the frame.
 */
ST75160::ST75160(const Config &cfg) : cfg_(cfg), fb_(&frame_tx[1]) {}

bool ST75160::xfer(const uint8_t *buf, size_t len, sysinterval_t timeout) {
    /* A transfer that timed out leaves the driver in I2C_LOCKED, and the high
     level driver asserts on any further call in that state, so the bus is
     restarted before giving up on the transfer.*/
    if (cfg_.i2c->state != I2C_READY) {
        i2cStop(cfg_.i2c);
        if (i2cStart(cfg_.i2c, &i2c_cfg) != HAL_RET_SUCCESS) {
            error_flags_ = err_start;
            return false;
        }
    }

    msg_t msg = i2cMasterTransmitTimeout(cfg_.i2c, cfg_.addr, buf, len, nullptr, 0, timeout);
    if (msg != MSG_OK) {
        error_flags_ = (uint32_t) i2cGetErrors(cfg_.i2c);
        if (error_flags_ == 0U) {
            /* MSG_TIMEOUT with no error flag: the peripheral never saw an idle
         bus, i.e. SCL or SDA is stuck low.*/
            error_flags_ = err_bus_stuck;
        }
        return false;
    }
    return true;
}

bool ST75160::run_co1(const uint8_t *script, size_t len) {
    size_t i = 0U;
    size_t n = 0U;

    while (i < len) {
        uint8_t op = script[i++];
        uint8_t arg = script[i++];

        if (op == OP_DELAY) {
            if ((n > 0U) && !xfer(seq_buf, n, TIME_MS2I(100))) {
                return false;
            }
            n = 0U;
            chThdSleepMilliseconds(arg);
            continue;
        }

        if (n > (sizeof seq_buf - 2U)) {
            if (!xfer(seq_buf, n, TIME_MS2I(100))) {
                return false;
            }
            n = 0U;
        }

        seq_buf[n++] = (op == OP_CMD) ? 0x80U : 0xC0U;
        seq_buf[n++] = script[i++];
    }

    return (n == 0U) || xfer(seq_buf, n, TIME_MS2I(100));
}

bool ST75160::init(bool skip_otp) {
    error_flags_ = 0U;
    ready_ = false;
    frame_tx[0] = ctrl_data;
    clear();

    /* Reset pulse. The datasheet only asks for 1us low and 1ms of settling,
     but the vendor's reference code uses 200ms and 100ms, so use those.*/
    palClearLine(cfg_.rst);
    chThdSleepMilliseconds(200);
    palSetLine(cfg_.rst);
    chThdSleepMilliseconds(100);

    if (i2cStart(cfg_.i2c, &i2c_cfg) != HAL_RET_SUCCESS) {
        error_flags_ = err_start;
        return false;
    }

    /* The OTP up-load also disables the controller's own auto-read of the
     factory trim, so skipping the whole block leaves the power-on values in
     place, which is the safer of the two if the manual load misbehaves.*/
    if (!skip_otp && !run_co1(init_otp, sizeof init_otp)) {
        return false;
    }

    if (!run_co1(init_script, sizeof init_script)) {
        return false;
    }

    ready_ = true;
    if (!flush()) {
        ready_ = false;
        return false;
    }

    return true;
}

void ST75160::clear(bool on) {
    uint8_t v = on ? 0xFFU : 0x00U;
    for (unsigned i = 0U; i < fb_size; i++) { fb_[i] = v; }
}

void ST75160::pixel(int x, int y, bool on) {
    if ((x < 0) || (x >= width) || (y < 0) || (y >= height)) {
        return;
    }

    /* The panel's rows run bottom to top as far as the controller is
     concerned, so y is flipped here and the API above is a plain top-left
     origin. The controller has no command for this: 0xBC only covers the
     address scan direction and the column order.*/
    y = height - 1 - y;

    /* One byte covers 8 rows of one column, D7 being the topmost row.*/
    uint8_t mask = (uint8_t) (1U << (7 - (y & 7)));
    uint8_t *p = &fb_[((unsigned) y / 8U) * width + (unsigned) x];

    if (on) {
        *p |= mask;
    } else {
        *p &= (uint8_t) ~mask;
    }
}

void ST75160::rect(int x, int y, int w, int h, bool on) {
    for (int j = y; j < (y + h); j++) {
        for (int i = x; i < (x + w); i++) { pixel(i, j, on); }
    }
}

void ST75160::frame(int x, int y, int w, int h, bool on) {
    for (int i = x; i < (x + w); i++) {
        pixel(i, y, on);
        pixel(i, y + h - 1, on);
    }
    for (int j = y; j < (y + h); j++) {
        pixel(x, j, on);
        pixel(x + w - 1, j, on);
    }
}

int ST75160::text(int x, int y, const char *s, bool on) {
    for (; *s != '\0'; s++) {
        char c = *s;
        if ((c < font8x8_first) || (c > font8x8_last)) {
            c = '?';
        }

        const uint8_t *glyph = font8x8[c - font8x8_first];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (((bits >> col) & 1U) != 0U) {
                    pixel(x + col, y + row, on);
                }
            }
        }
        x += 8;
    }

    return x;
}

bool ST75160::flush() {
    if (!ready_) {
        return false;
    }

    if (!run_co1(frame_addr, sizeof frame_addr)) {
        return false;
    }

    return xfer(frame_tx, sizeof frame_tx, TIME_MS2I(500));
}

/*
 * Vop is split 6 bits then 3 across the two parameters, which is why the
 * datasheet's 0x08, 0x03 means 200 and therefore 11.6V. Command set 1 has to
 * be selected first: 0x81 lives there and init_script leaves the panel in
 * set 2.
 */
bool ST75160::contrast(uint16_t vop) {
    if (!ready_) {
        return false;
    }

    const uint8_t script[] = {CMD(0x30), CMD(0x81), PAR(vop & 0x3FU), PAR((vop >> 6) & 0x07U)};

    return run_co1(script, sizeof script);
}

void ST75160::backlight(bool on) {
    if (on) {
        palSetLine(cfg_.bklt);
    } else {
        palClearLine(cfg_.bklt);
    }
}

} /* namespace pika::lcd */
