/*
 * Driver for ST75160i LCD display controller on I2C.
 *
 * The panel's wiring - its I2C driver, bus address, reset and the two bus pins
 * - is passed in as a Config at construction, so the driver itself does not
 * depend on any board header.
 *
 * The driver does not draw. It owns the framebuffer, in the panel's own
 * memory layout, and hands it out as a Framebuffer that a gfx::Canvas draws
 * pixels into; flush() sends the DDRAM pages those pixels landed on, full
 * width, in one I2C transaction. A whole frame is ~47ms at 400kHz, a line of
 * text ~4ms, nothing drawn is no bus traffic at all. Draw everything, then
 * flush once.
 *
 * Usage:
 *
 *   static const pika::lcd::ST75160::Config lcd_cfg = {
 *     &BOARD_LCD_I2C, BOARD_LCD_I2C_ADDR, LINE_LCD_RST,
 *     LINE_LCD_SCL, LINE_LCD_SDA, BOARD_LCD_I2C_PINMODE
 *   };
 *   static pika::lcd::ST75160 lcd{lcd_cfg};
 *   static pika::gfx::Canvas<pika::lcd::ST75160::Framebuffer> canvas{lcd.framebuffer()};
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
    /**
     * @brief   The panel's pixels, as gfx::Canvas sees them.
     * @details Canvas only ever deals in pixels: it clips to width x height,
     *          writes through set_pixel() and reports each region it drew on
     *          with mark(). Everything about how those pixels are stored is
     *          in here, and is the panel's monochrome DDRAM, so flush() is a
     *          plain copy:
     *
     *          - Pages of 8 rows, each page `width` bytes, one byte per
     *            column; the last page is partly unused when height is not a
     *            multiple of 8.
     *          - Within a byte D7 is the topmost row of the page.
     *          - The panel's rows run bottom to top, so pixel row y lives in
     *            DDRAM row height - 1 - y, and the top of the picture is on
     *            the highest page.
     *
     *          Changes are tracked per page because a page is the finest the
     *          panel can be addressed at.
     */
    class Framebuffer {
    public:
        static constexpr int width = 160;
        static constexpr int height = 100;

        /**
         * @brief   Sets or clears one pixel.
         * @note    No range check and no dirty marking: Canvas clips a whole
         *          block once and marks it once, rather than paying for both
         *          on every pixel.
         */
        template<bool ON>
        void set_pixel(int x, int y) {
            y = height - 1 - y;
            uint8_t mask = (uint8_t) (1U << (7 - (y & 7)));
            uint8_t *p = &bits_[(unsigned) (y / page_rows) * width + (unsigned) x];
            if (ON) {
                *p |= mask;
            } else {
                *p &= (uint8_t) ~mask;
            }
        }

        /** @brief  Records a drawn region, already clipped and not empty. */
        void mark(int x, int y, int w, int h) {
            (void) x;
            (void) w; /* full width pages are all flush() sends */

            /* The flip in set_pixel() puts the top row on the highest page. */
            const int first = (height - y - h) / page_rows;
            const int last = (height - 1 - y) / page_rows;
            dirty_ |= (uint16_t) (((2U << last) - 1U) & ~((1U << first) - 1U));
        }

    private:
        friend class ST75160;

        /* 8 display rows per DDRAM page, rounded up to cover every row. */
        static constexpr int page_rows = 8;
        static constexpr int pages = (height + page_rows - 1) / page_rows;

    public:
        /** Bytes of storage, for the TX buffer the .cpp has to place. */
        static constexpr unsigned size = pages * width;

    private:
        static constexpr uint16_t all_pages = (1U << pages) - 1U;
        static_assert(pages <= 16, "dirty_ has one bit per page");

        explicit Framebuffer(uint8_t *bits) : bits_(bits) {}

        uint8_t *bits_;      /**< size bytes, inside the TX buffer.          */
        uint16_t dirty_ = 0; /**< Bit p set: page p changed since the flush. */
    };

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

    /** @brief  The framebuffer flush() sends, for a gfx::Canvas to draw on. */
    Framebuffer &framebuffer() { return fb_; }

    /**
     * @brief   Sends the pages marked dirty since the last flush to the panel.
     * @note    Clean pages between the first and last dirty one go too: one
     *          window and one transaction, rather than one per run of pages.
     * @return  false if any I2C transaction failed, see last_error(). The
     *          pages stay dirty, so the next flush retries them.
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

    Config cfg_;
    Framebuffer fb_;
    uint32_t error_flags_ = 0U;
    bool ready_ = false;
};

} /* namespace pika::lcd */
