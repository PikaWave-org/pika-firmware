/*
 * Drawing primitives over a framebuffer: pixels, blocks, lines, images and 8x8
 * text, top-left origin.
 *
 * Canvas knows pixels and nothing else. How they are stored, and what has to
 * be sent where, belongs to the framebuffer type FB, which the display driver
 * provides; all Canvas asks of it is
 *
 *   FB::width, FB::height          the panel in pixels
 *   fb.set_pixel<ON>(x, y)         one pixel, always on the panel
 *   fb.mark(x, y, w, h)            a region drawn on, clipped, never empty
 *
 * A template rather than a virtual interface: the per-pixel call inlines into
 * the loops, which is where all the time goes.
 *
 * Draw everything, then flush the driver once.
 *
 * Usage:
 *
 *   pika::gfx::Canvas<pika::lcd::ST75160::Framebuffer> canvas{lcd.framebuffer()};
 *   canvas.text(0, 0, "hello");
 *   lcd.flush();
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <pika/gfx/Font8x8.h>

namespace pika::gfx {

template<class FB>
class Canvas {
public:
    static constexpr int width = FB::width;
    static constexpr int height = FB::height;

    explicit Canvas(FB &fb) : fb_(fb) {}

    /** @brief  Fills the whole panel, false = all pixels off. */
    void clear(bool on = false) {
        if (on) {
            rect<true>(0, 0, width, height);
        } else {
            rect<false>(0, 0, width, height);
        }
    }

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
        fb_.template set_pixel<ON>(x, y);
        fb_.mark(x, y, 1, 1);
    }

    /** @brief  Draws a filled rectangle. */
    template<bool ON>
    void rect(int x, int y, int w, int h) {
        const int x0 = (x < 0) ? 0 : x;
        const int x1 = ((x + w) > width) ? width : (x + w);
        const int y0 = (y < 0) ? 0 : y;
        const int y1 = ((y + h) > height) ? height : (y + h);

        for (int j = y0; j < y1; j++) {
            for (int i = x0; i < x1; i++) { fb_.template set_pixel<ON>(i, j); }
        }
        mark(x, y, w, h);
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
            for (int i = x0; i < x1; i++) { fb_.template set_pixel<ON>(i, y); }
        }
        if ((bottom >= 0) && (bottom < height)) {
            for (int i = x0; i < x1; i++) { fb_.template set_pixel<ON>(i, bottom); }
        }
        if ((x >= 0) && (x < width)) {
            for (int j = y0; j < y1; j++) { fb_.template set_pixel<ON>(x, j); }
        }
        if ((right >= 0) && (right < width)) {
            for (int j = y0; j < y1; j++) { fb_.template set_pixel<ON>(right, j); }
        }
        mark(x, y, w, h);
    }

    /** @brief  Draws a horizontal line w pixels long, rightwards from (x, y). */
    template<bool ON>
    void hline(int x, int y, int w) {
        rect<ON>(x, y, w, 1);
    }

    /** @brief  Draws a vertical line h pixels long, downwards from (x, y). */
    template<bool ON>
    void vline(int x, int y, int h) {
        rect<ON>(x, y, 1, h);
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
                    fb_.template set_pixel<ON>(x + col, y + row);
                }
            }
        }
        mark(x, y, w, h);
    }

    /**
     * @brief   Draws a string in the 8x8 font, no wrapping.
     * @return  The x coordinate just past the last glyph.
     */
    template<bool ON = true>
    int text(int x, int y, const char *s) {
        const int r0 = (y < 0) ? -y : 0;
        const int r1 = ((y + 8) > height) ? (height - y) : 8;
        const int left = x;

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
                        fb_.template set_pixel<ON>(x + col, y + row);
                    }
                }
            }
        }

        mark(left, y, x - left, 8);
        return x;
    }

private:
    /** @brief  Clips a region to the panel and reports it, if anything is left. */
    void mark(int x, int y, int w, int h) {
        const int x0 = (x < 0) ? 0 : x;
        const int x1 = ((x + w) > width) ? width : (x + w);
        const int y0 = (y < 0) ? 0 : y;
        const int y1 = ((y + h) > height) ? height : (y + h);
        if ((x0 < x1) && (y0 < y1)) {
            fb_.mark(x0, y0, x1 - x0, y1 - y0);
        }
    }

    FB &fb_;
};

} /* namespace pika::gfx */
