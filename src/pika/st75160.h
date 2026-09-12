/*
 * Driver for the Newhaven NHD-C160100DiZ-FSW-FBW display: 160x100
 * monochrome COG panel with an ST75160i controller on I2C.
 *
 * The panel, its I2C driver and its address come from the board files
 * (BOARD_LCD_I2C, BOARD_LCD_I2C_ADDR, LINE_LCD_RST, LINE_LCD_BKLT).
 *
 * Drawing goes into a RAM framebuffer; flush() pushes the whole frame in one
 * I2C transaction (~47ms at 400kHz), so draw everything, then flush once.
 */

#ifndef PIKA_ST75160_H
#define PIKA_ST75160_H

#include <cstdint>

namespace pika::lcd {

constexpr int width = 160;
constexpr int height = 100;

/* 8 display rows per DDRAM page, 13 pages to cover 100 rows. */
constexpr int pages = 13;
constexpr unsigned fb_size = pages * width;

/**
 * @brief   Resets the panel and runs the initialization sequence.
 * @note    Leaves the display on, showing a cleared framebuffer.
 * @return  false if any I2C transaction failed, see last_error().
 */
bool init(bool skip_otp = false);

/** @brief  Fills the framebuffer, false = all pixels off. */
void clear(bool on = false);

/** @brief  Sets or clears one pixel, out of range coordinates are ignored. */
void pixel(int x, int y, bool on);

/** @brief  Draws a filled rectangle. */
void rect(int x, int y, int w, int h, bool on);

/** @brief  Draws a 1 pixel wide rectangle outline. */
void frame(int x, int y, int w, int h, bool on);

/**
 * @brief   Draws a string in the 8x8 font, no wrapping.
 * @return  The x coordinate just past the last glyph.
 */
int text(int x, int y, const char *s, bool on = true);

/** @brief  Sends the framebuffer to the panel. */
bool flush(void);

/** @brief  Switches the backlight. */
void backlight(bool on);

/*
 * Bring-up helpers. These talk to the controller directly and ignore the
 * framebuffer, which is what makes them useful when the panel shows nothing:
 * all_pixels(true) lights every segment from inside the controller, so it
 * separates "panel/contrast/init is wrong" from "the image data is wrong".
 */

/** @brief  Display on or off (0xAF / 0xAE). */
bool display(bool on);

/** @brief  Forces every pixel on, regardless of RAM (0xA5 / 0xA4). */
bool all_pixels(bool on);

/** @brief  Inverts the panel (0xA7 / 0xA6). */
bool inverse(bool on);

/**
 * @brief   Sets the LCD drive voltage Vop (0x81).
 * @param   vpr   9 bit register value, 200 is the datasheet's 11.6V. The
 *                panel is specified for 11.3..11.9V, i.e. vpr 195..205, so
 *                stay near that unless sweeping deliberately.
 */
bool set_vop(uint16_t vpr);

/** @brief  Vop register value corresponding to the datasheet's 11.6V. */
constexpr uint16_t vop_default = 200U;

/**
 * @brief   Fills the panel with one byte value, bypassing the framebuffer.
 * @details Writes the display RAM exactly the way the vendor's reference
 *          code does: one I2C transaction per data byte, over the same 25
 *          page window it uses. Slow (about a second), but it is the known
 *          good sequence, so it separates "the panel does not take our data
 *          stream" from "the panel does not take our data at all".
 */
bool fill_raw(uint8_t value);

/**
 * @brief   Reads the controller status byte.
 * @details D3 is display on/off, D4 scan direction, D1 inverse, D5 RMW,
 *          D7/D6 scroll mode. The first byte read back is the dummy byte the
 *          datasheet's bus holder requires, so the status is the second.
 * @return  the status byte, or negative if the read failed.
 */
int read_status(void);

/**
 * @brief   Writes a known pattern to the panel and reads it back.
 * @details The point of this is to tell a transport that moves data from one
 *          that silently moves none: return codes and error flags look
 *          identical either way, which is how a broken I2C DMA path went
 *          unnoticed through a whole bring-up. Run it after any change to
 *          the transport.
 * @note    It checks that the written pattern comes back, not where in the
 *          stream it lands: the controller prefixes a dummy and a status
 *          byte, and the read auto-increment does not follow the datasheet
 *          well enough to pin an exact offset on. verify_read holds the raw
 *          bytes for inspection.
 * @return  true if the pattern was found in what the controller returned.
 */
bool verify(void);

/** @brief  Raw bytes returned by the last verify(). */
extern uint8_t verify_read[8];

/** @brief  I2C error flags from the last failed transaction, 0 if none. */
uint32_t last_error(void);

} /* namespace pika::lcd */

#endif /* PIKA_ST75160_H */
