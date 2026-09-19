---
name: melpe-2400-cost
description: Measured on hardware - MELPe 2400 encodes a frame in 7ms of its 22.5ms budget on this part, decode is 2ms, and the DWT cycle counter has to be unlocked by hand or every timing reads zero.
metadata:
  node_type: memory
  type: project
---

What the MELPe 2400 codec actually costs on this board, measured 2026-09-19
with the bench in `targets/test/main.cpp` at 520MHz, `-O2`, caches on. None of
it is derivable from the source, and the first number decides whether the
feature is possible at all.

- **Encode is 7.0ms per 22.5ms frame — 31% of realtime.** Over 44 frames:
  min 5.3ms, mean 6.8ms, max 7.0ms. **Decode is 2.0ms**, max, or 8%. So a
  duplex link would still fit in under 40%, and there is room to do the
  arithmetic before deciding it is too slow.
- **Cold and warm are the same.** A second pass over the same clip measured
  6835us mean against 6826us. The cold I-cache cost over ~190KB of codec is
  real but lands almost entirely in the first frame or two, which is not
  enough to move the maximum. Worth knowing because it means the margin at
  the first frame after a button press is the same margin as everywhere else.
- **The codec needs about 3.2KB of stack**, measured from the `0x55` fill with
  encode and decode both exercised (5432 free of 8640). `waMelp` is 8192 and
  therefore generous; that is deliberate, not unmeasured.

**`chSysGetRealtimeCounterX()` returns zero on this part until the DWT is
unlocked.** It is `DWT->CYCCNT`, and the ChibiOS port's enable silently does
nothing because the M7 brings the DWT up locked. Write `0xC5ACCE55` to
`DWT->LAR` after setting `TRCENA`, then set `CYCCNTENA` — and read the counter
twice to confirm it moves, because the failure is silent and every timing
reads exactly `0us`, which looks like a very fast codec rather than a broken
measurement. The bench prints `cycle counter running` for this reason.

**A CRC of the bitstream is only comparable against the same samples.**
`tools/wav2cpp.py` resamples on its own and normalises the clip to -1 dBFS, so
encoding `meow.mp3` through ffmpeg at 8kHz gives a different signal and a
different, perfectly valid, bitstream. Dump the generated array out of
`build/generated_src/pika/audio/sounds/meow8k.cpp` instead. Done that way the
board and the host agree exactly — `57CDBE3F` over the first 44 frames — which
is what proves the Cortex-M7 build is bit-identical to the host build that
`tools/melpe-check` compares against pristine upstream.

Also measured, and the reason the 1200 bps figure in `PROVENANCE.md` was not
enough: the `mat_lib.c` arena peaks at **179 of its 256 words at 2400**,
against 161 at 1200. `vq_ms4` is the largest allocator in the tree and is
reached only from the `RATE2400` branch, so the older number never covered
this path. Overflowing it is `__builtin_trap`, not a diagnostic.
