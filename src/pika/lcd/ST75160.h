/*
 * Driver for the Newhaven NHD-C160100DiZ-FSW-FBW display: 160x100
 * monochrome COG panel with an ST75160i controller on I2C.
 *
 * The panel's wiring - its I2C driver, bus address, reset and the two bus pins
 * - is passed in as a Config at construction, so the driver itself does not
 * depend on any board header. The board's own BOARD_LCD_* and LINE_LCD_*
 * definitions are what the caller fills the Config from.
 *
 * The backlight is not here: it is a LED on a PWM channel, driven by its owner
 * through the ChibiOS PWM driver - see main.cpp.
 *
 * Drawing goes into a RAM framebuffer; flush() pushes the whole frame in one
 * I2C transaction (~47ms at 400kHz), so draw everything, then flush once.
 *
 * Usage:
 *
 *   static const pika::lcd::ST75160::Config lcd_cfg = {
 *     &BOARD_LCD_I2C, BOARD_LCD_I2C_ADDR, LINE_LCD_RST,
 *     LINE_LCD_SCL, LINE_LCD_SDA, BOARD_LCD_I2C_PINMODE
 *   };
 *   static pika::lcd::ST75160 lcd{lcd_cfg};
 *
 * @note  The transfer buffers, the framebuffer among them, are shared between
 *        all instances: they have to sit in the one region of memory the DMA
 *        controller can reach and the D-cache leaves alone, and that placement
 *        is not something a caller can be relied on to get right. One instance
 *        is therefore the supported case - see the buffer comment in the .cpp.
 */

#pragma once

#include <cstdint>

#include "hal.h"

namespace pika::lcd {

class ST75160 {
public:
    static constexpr int width = 160;
    static constexpr int height = 100;

    /* 8 display rows per DDRAM page, 13 pages to cover 100 rows. */
    static constexpr int pages = 13;
    static constexpr unsigned fb_size = pages * width;

    /**
   * @brief   How the panel is wired up, taken from the board header.
   */
    struct Config {
        I2CDriver *i2c;   /**< Bus the panel is on.                           */
        i2caddr_t addr;   /**< 7 bit slave address.                           */
        ioline_t rst;     /**< Panel reset, active low.                       */
        ioline_t scl;     /**< Bus clock, driven directly during recovery.    */
        ioline_t sda;     /**< Bus data, driven directly during recovery.     */
        iomode_t pinmode; /**< Mode to restore on scl/sda after recovery.     */
    };

    explicit ST75160(const Config &cfg);

    /**
   * @brief   Resets the panel and runs the initialization sequence.
   * @note    Leaves the display on, showing a cleared framebuffer.
   * @return  false if any I2C transaction failed, see last_error().
   */
    bool init();

    /** @brief  Fills the framebuffer, false = all pixels off. */
    void clear(bool on = false);

    /** @brief  Sets or clears one pixel, out of range coordinates are ignored. */
    void pixel(int x, int y, bool on);

    /** @brief  Draws a filled rectangle. */
    void rect(int x, int y, int w, int h, bool on);

    /** @brief  Draws a 1 pixel wide rectangle outline. */
    void frame(int x, int y, int w, int h, bool on);

    /**
   * @brief   Draws a packed 1bpp image, as tools/bmp2cpp.py emits it.
   * @param   bits  Rows top to bottom, each (w+7)/8 bytes, MSB leftmost.
   * @note    Only set bits are drawn, the way text() draws a glyph: the
   *          background is whatever was already there, so clear() first if
   *          the image is meant to be opaque.
   */
    void bitmap(int x, int y, int w, int h, const uint8_t *bits, bool on = true);

    /**
   * @brief   Draws a string in the 8x8 font, no wrapping.
   * @return  The x coordinate just past the last glyph.
   */
    int text(int x, int y, const char *s, bool on = true);

    /** @brief  Sends the framebuffer to the panel. */
    bool flush();

    /**
   * @brief   Sets the contrast, as the Vop word the datasheet defines.
   * @note    V0 = 3.6 + vop * 0.04 volts; init() programs 200, so 11.6V.
   * @note    One I2C transaction, sharing the sequencing buffer with
   *          flush(), so call it only from the thread that owns the panel.
   * @return  false if the transaction failed, see last_error().
   */
    bool contrast(uint16_t vop);

    /** @brief  I2C error flags from the last failed transaction, 0 if none. */
    uint32_t last_error() const { return error_flags_; }

private:
    bool xfer(const uint8_t *buf, size_t len, sysinterval_t timeout);
    bool run_co1(const uint8_t *script, size_t len);

    Config cfg_;
    uint8_t *fb_; /**< Framebuffer, inside the TX buffer.     */
    uint32_t error_flags_ = 0U;
    bool ready_ = false;
};

} /* namespace pika::lcd */
