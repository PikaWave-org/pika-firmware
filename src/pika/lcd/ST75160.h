/*
 * Driver for ST75160i LCD display controller on I2C.
 *
 * The panel's wiring - its I2C driver, bus address, reset and the two bus pins
 * - is passed in as a Config at construction, so the driver itself does not
 * depend on any board header.
 *
 * Drawing goes into a RAM framebuffer and records which DDRAM pages (8 row
 * bands) it touched; flush() sends just those, full width, in one I2C
 * transaction. A whole frame is ~47ms at 400kHz, a line of text ~4ms, nothing
 * drawn is no bus traffic at all. Draw everything, then flush once.
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

#include <pika/lcd/Font8x8.h>

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

    /*
     * Every primitive takes on/off as a template parameter: each caller knows
     * at compile time whether it sets or clears, so the choice is made once
     * there and the pixel loops carry only the one mask operation. Blocks are
     * clipped to the panel once, before the loops.
     */

    /** @brief  Sets or clears one pixel, out of range coordinates are ignored. */
    template<bool ON>
    void pixel(int x, int y) {
        if ((x < 0) || (x >= width) || (y < 0) || (y >= height)) {
            return;
        }
        set_pixel<ON>(x, y);
        mark_rows(y, 1);
    }

    /** @brief  Draws a filled rectangle. */
    template<bool ON>
    void rect(int x, int y, int w, int h) {
        const int x0 = (x < 0) ? 0 : x;
        const int x1 = ((x + w) > width) ? width : (x + w);
        const int y0 = (y < 0) ? 0 : y;
        const int y1 = ((y + h) > height) ? height : (y + h);

        for (int j = y0; j < y1; j++) {
            for (int i = x0; i < x1; i++) { set_pixel<ON>(i, j); }
        }
        mark_rows(y, h);
    }

    /** @brief  Draws a 1 pixel wide rectangle outline. */
    template<bool ON>
    void frame(int x, int y, int w, int h) {
        if ((w <= 0) || (h <= 0)) {
            return;
        }

        const int x0 = (x < 0) ? 0 : x;
        const int x1 = ((x + w) > width) ? width : (x + w);
        const int y0 = (y < 0) ? 0 : y;
        const int y1 = ((y + h) > height) ? height : (y + h);
        const int bottom = y + h - 1;
        const int right = x + w - 1;

        /* Each edge only if it is on the panel; the corners are drawn twice,
           which is harmless. */
        if ((y >= 0) && (y < height)) {
            for (int i = x0; i < x1; i++) { set_pixel<ON>(i, y); }
        }
        if ((bottom >= 0) && (bottom < height)) {
            for (int i = x0; i < x1; i++) { set_pixel<ON>(i, bottom); }
        }
        if ((x >= 0) && (x < width)) {
            for (int j = y0; j < y1; j++) { set_pixel<ON>(x, j); }
        }
        if ((right >= 0) && (right < width)) {
            for (int j = y0; j < y1; j++) { set_pixel<ON>(right, j); }
        }
        mark_rows(y, h);
    }

    /**
   * @brief   Draws a packed 1bpp image, as tools/bmp2cpp.py emits it.
   * @param   bits  Rows top to bottom, each (w+7)/8 bytes, MSB leftmost.
   * @note    Only set bits are drawn, the way text() draws a glyph: the
   *          background is whatever was already there, so clear() first if
   *          the image is meant to be opaque.
   */
    template<bool ON = true>
    void bitmap(int x, int y, int w, int h, const uint8_t *bits) {
        const int stride = (w + 7) / 8;
        const int r0 = (y < 0) ? -y : 0;
        const int r1 = ((y + h) > height) ? (height - y) : h;
        const int c0 = (x < 0) ? -x : 0;
        const int c1 = ((x + w) > width) ? (width - x) : w;

        for (int row = r0; row < r1; row++) {
            const uint8_t *line = &bits[(size_t) row * (size_t) stride];
            for (int col = c0; col < c1; col++) {
                if (((line[col / 8] >> (7 - (col & 7))) & 1U) != 0U) {
                    set_pixel<ON>(x + col, y + row);
                }
            }
        }
        mark_rows(y, h);
    }

    /**
   * @brief   Draws a string in the 8x8 font, no wrapping.
   * @return  The x coordinate just past the last glyph.
   */
    template<bool ON = true>
    int text(int x, int y, const char *s) {
        const int r0 = (y < 0) ? -y : 0;
        const int r1 = ((y + 8) > height) ? (height - y) : 8;

        if (*s != '\0') {
            mark_rows(y, 8);
        }

        for (; *s != '\0'; s++, x += 8) {
            if (((x + 8) <= 0) || (x >= width)) {
                continue; /* glyph entirely off the panel, x still advances */
            }

            char c = *s;
            if ((c < font8x8_first) || (c > font8x8_last)) {
                c = '?';
            }

            const uint8_t *glyph = font8x8[c - font8x8_first];
            const int c0 = (x < 0) ? -x : 0;
            const int c1 = ((x + 8) > width) ? (width - x) : 8;
            for (int row = r0; row < r1; row++) {
                uint8_t bits = glyph[row];
                for (int col = c0; col < c1; col++) {
                    if (((bits >> col) & 1U) != 0U) {
                        set_pixel<ON>(x + col, y + row);
                    }
                }
            }
        }

        return x;
    }

    /**
     * @brief   Sends the pages drawn on since the last flush to the panel.
     * @return  false if any I2C transaction failed, see last_error(). The
     *          pages stay pending, so the next flush retries them.
     */
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

    /*
     * The framebuffer write behind every primitive: the flip and the bit. It
     * neither range checks nor touches the dirty pages - a primitive clips its
     * block and marks it once, rather than paying for both on every pixel.
     *
     * The panel's rows run bottom to top as far as the controller is
     * concerned, so y is flipped here and the API above is a plain top-left
     * origin. The controller has no command for this: 0xBC only covers the
     * address scan direction and the column order. One byte covers 8 rows of
     * one column, D7 being the topmost row.
     */
    template<bool ON>
    void set_pixel(int x, int y) {
        y = height - 1 - y;
        uint8_t mask = (uint8_t) (1U << (7 - (y & 7)));
        uint8_t *p = &fb_[(unsigned) (y / 8) * width + (unsigned) x];
        if (ON) {
            *p |= mask;
        } else {
            *p &= (uint8_t) ~mask;
        }
    }

    /** @brief  Marks the pages covering rows y..y+h-1 pending, clipped. */
    void mark_rows(int y, int h);

    Config cfg_;
    uint8_t *fb_; /**< Framebuffer, inside the TX buffer.     */
    uint32_t error_flags_ = 0U;
    bool ready_ = false;

    /* First and last DDRAM page drawn on but not yet sent, p0 > p1 when
       nothing is pending. Pages rather than rows because a byte of DDRAM is 8
       rows, the finest the panel can be addressed at. Full width always: the
       column window is then the one this panel has ever been driven with. */
    int dirty_first_ = pages, dirty_last_ = -1;
};

} /* namespace pika::lcd */
