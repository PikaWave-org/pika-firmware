---
name: read-panel-over-swd
description: The framebuffer can be read out of D2 SRAM while the firmware runs, which is how you check what the panel says when the log is dead and you cannot see the board.
metadata:
  node_type: memory
  type: project
---

Anything drawn on the LCD can be read back over SWD from a running board, no
halt and no reset. This is the only way to verify panel output from a session
that cannot see the hardware, and it works on a boot whose log has already
died in [[lcd-flush-hangs-after-few-beats]] - the framebuffer keeps whatever
the last successful draw put there.

The buffer is `frame_tx + 1` in D2 SRAM, the byte after the I2C control byte -
**0x30004801** on the images built here, but take it from the running object
rather than trusting that number: `_ZL3lcd`'s `fb_` member holds it, at offset
0x1c into the object on 2026-09-17. `arm-none-eabi-nm -S` gives both symbols.

    tools/hw run -r "read panel" -- JLinkExe -nogui 1 -device STM32H743ZI \
        -if SWD -speed 4000 -CommanderScript <file with: mem8 0x30004801, 0x280>

Reading is passive: no `h` in the command file. **Do not halt** - JLinkExe
cannot resume the core afterwards (see [[swd-run-control-and-fake-presses]]),
so a halt costs you the surviving boot you were measuring.

Decoding takes both transforms in `ST75160::pixel()` together, and getting
either one wrong still produces plausible-looking noise:

    yy = 99 - y                              # rows run bottom to top
    bit = (fb[(yy // 8) * 160 + x] >> (7 - (yy & 7))) & 1     # D7 is topmost

Text comes back exactly by lifting the 8x8 cell at `(x0 + 0..7, y0 + 0..7)`
into eight row bytes, bit b of row r being the pixel at `x0 + b`, and looking
that up in the `font8x8` table in `src/pika/lcd/Font8x8.h`. Every glyph matches
a table entry exactly - if some come back as misses, the bit order is wrong,
not the font.

Worth doing even when a `tools/hw log` capture is alive, because it reads what
the panel *shows* rather than what the firmware says it drew. It caught the mic
meter's line at its full 19 characters, which is the widest the 8x8 font fits
across 160px, and let the bar's pixel fill be checked against the number
printed beside it.
