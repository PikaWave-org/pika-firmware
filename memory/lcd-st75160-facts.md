---
name: lcd-st75160-facts
description: NHD-C160100DiZ Rev1C panel facts - address, mono RAM layout, framing, reads
metadata:
  type: reference
---

Newhaven NHD-C160100DiZ-FSW-FBW **Rev1C** (label on the module back), 160x100,
ST75160i controller, I2C address **0x3F**, max 400kHz. Driver lives in
`src/st75160.cpp`.

- Monochrome mode (`0xF0`,`0x10`): one byte is **8 vertical pixels**, and
  **D7 is the top** row of the page - the opposite of the SSD1306/u8g2
  convention. Framebuffer is 13 pages x 160 = 2080 bytes, index
  `page * 160 + column`, bit `7 - (row % 8)`.
- Control byte is `Co<<7 | A0<<6`. Command and parameter bytes each go in
  their own transaction (`0x00,cmd` / `0x40,param`), as in Newhaven's code; a
  frame can be one `0x40` control byte followed by the whole stream.
- `V0 = 3.6 + Vop[8:0] * 0.04`, so the datasheet's `0x81 0x08 0x03` is 11.6V.
- The panel's rows run **bottom to top**: `pixel()` flips y so the driver API
  has a top-left origin. There is no command for this - `0xBC` only sets the
  address scan direction and column order, and **COMSCN has no effect on
  Rev1C** (FPC pin 1 is unconnected; driving it high or low was tried on
  hardware and changed nothing).
- Reads work and are useful: status byte via a write of one control byte then
  a restart-read, **second byte** is the status (the first is the dummy the
  bus holder requires). D3 = display on/off.
- The authoritative register documentation is the **Sitronix ST75161-G2A**
  datasheet; Newhaven only publish a condensed table.

Related: [[i2c-dma-silently-broken]], which is what made all of this look
broken for a whole session.
