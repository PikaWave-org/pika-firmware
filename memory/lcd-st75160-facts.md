---
name: lcd-st75160-facts
description: NHD-C160100DiZ Rev1C panel facts - address, mono RAM layout, framing, reads
metadata:
  type: reference
---

Newhaven NHD-C160100DiZ-FSW-FBW **Rev1C** (label on the module back), 160x100,
ST75160i controller, I2C address **0x3F**, max 400kHz. Driver lives in
`src/pika/lcd/ST75160.cpp`.

- Monochrome mode (`0xF0`,`0x10`): one byte is **8 vertical pixels**, and
  **D7 is the top** row of the page - the opposite of the SSD1306/u8g2
  convention. Framebuffer is 13 pages x 160 = 2080 bytes, index
  `page * 160 + column`, bit `7 - (row % 8)`.
- Control byte is `Co<<7 | A0<<6`. **The Co=1 interleaved form works** - each
  byte prefixed by its own control byte (`0x80` command, `0xC0` parameter), a
  whole script in one transaction. An earlier claim that this controller
  ignores Co=1 was wrong: the noise that produced it was
  [[i2c-dma-silently-broken]], not the framing. Newhaven's per-byte Co=0
  transactions also work, they are just slower. A frame stays Co=0 - one
  `0x40`, then the whole stream - because a control byte per pixel byte would
  double both the bytes and the flush time.
- `V0 = 3.6 + Vop[8:0] * 0.04`, so the datasheet's `0x81 0x08 0x03` is 11.6V.
  Vop is split across the two parameter bytes as low 6 bits then high 3, which
  is what makes `0x08, 0x03` mean 200. `0x81` is in extension command set 1,
  so select it with `0x30` first - `init_script` leaves the panel in set 2.
  **Vop 150..250 (9.6V..13.6V) is the usable range**, confirmed by eye on
  Rev1C: legible across the whole span, visibly light at the bottom and dark
  at the top. That is what the menu's contrast setting exposes.
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
