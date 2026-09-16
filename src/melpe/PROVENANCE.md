# MELPe 1200 bps speech codec — where this came from

Vendored 2026-09-17 from **https://github.com/Rhizomatica/melpe**, commit
`16c3e440790c8dcb0d9ec14164f0d8e4d5368eb2` (2020-09-14), subdirectory
`melpe/`. Upstream's `encoder.c`, `decoder.c` and `Makefile` are host-side
command line drivers and were left behind; everything else is here verbatim
apart from the patches listed below.

## Read this before shipping anything that uses it

Rhizomatica's repository declares GPL-3.0, but the files themselves carry
three layers of someone else's copyright: `Copyright (C) 2000, Microsoft
Corp.` over Texas Instruments' 1998 MELP federal-standard code, whose fixed
point basic operations derive from ETSI's GSM 06.06. TI holds IP on the MELP
algorithm and names a licensing contact in the headers; Compandent claims
rights in MELPe and its derivatives. The GPL-3.0 label upstream applied does
not obviously have the authority to relicense any of that.

Fine for development and evaluation. **Get a license before this ships in a
product.** That is not a question the firmware can settle.

## Why this repo and not another

Every public C MELPe is the same SC1200 v7.0 reference source; they differ
only in packaging, so there is no better implementation to find, only better
packaging. What was compared:

- **osherbakov/MELPe_fxp** — the raw reference dump, 7MB with Win32 and ARM
  project files, no license file.
- **gegel/jackpair** `Src/melpe_old` — same code plus ETSI Cortex-M4
  intrinsics (`etsi.h`), which is genuinely interesting for speed, but it is
  welded into a CubeMX project and has no license file. Worth revisiting if
  the codec turns out to be too slow.
- **Suwzw/MELPe-C6748-DSP** — nicely restructured, but specific to a TI
  C6748 and 1200 bps only.
- **HeroesLament/melpe-rs** — MIT, `no_std` Rust, actively maintained, but
  600 bps only and the wrong language for this tree.

Rhizomatica won because the codec is already separated into its own
directory, and it compiled for Cortex-M7 with zero source changes.

Vendored rather than added as a submodule because upstream has been dead
since 2020 and the patches below have to live somewhere.

## Rate

Upstream's `melpe.h` exposes **1200 bps only**: `melpe_i()` once, then
`melpe_a()` compresses 540 samples (67.5 ms at 8 kHz) to 11 bytes and
`melpe_s()` reverses it. `melpe_n()` is the noise preprocessor on its own.
The underlying code also implements 2400 and 600 (`rate = RATE2400` in
`melpe_i()`), so adding a rate is a small wrapper change, not a port.

## Patches

All five exist to make the codec survive in a firmware image with no heap and
no stdio. Every one is marked with a `pika:` comment at the site.

1. `qnt12_cb.c` — added `const` to the eleven codebook definitions. They were
   already declared `extern const` in `qnt12_cb.h`, and the `.c` did not
   include its own header, so nothing caught the mismatch and 64KB of tables
   sat in RAM. Also added that missing include so it cannot recur, and made
   the two dead alias pointers `const` to match.
2. `mat_lib.c` — `v_get`/`L_v_get`/`v_free` used `malloc`. Replaced with a
   1KB fixed arena (bump pointer; freeing a block releases everything handed
   out after it, which covers `lsp_to_lpc`'s out-of-order `f0`/`f1` free).
   The 161-word peak was measured, not guessed.
3. `mathhalf_i.h` — the nine 40-bit accumulator checks called `fprintf` and
   `exit` from `always_inline` helpers, so they landed at every call site.
   Now `MELPE_CHECK_40`, off unless `MELPE_BASIC_OP_CHECKS` is defined. The
   dropped `acc != floor(acc)` term was meaningless: `acc` is an `int64_t`.
4. `qnt12.c` — the unstable-filter `fprintf` became `MELPE_DIAG`, on only
   under `MELPE_DIAGNOSTICS`.
5. `pika_no_stdio.h` — new file, not upstream. Routes `assert` to
   `__builtin_trap` and defines `MELPE_DIAG`. Both exist because one
   `assert` or `fprintf` links `vfprintf`, and `vfprintf` drags in `malloc`
   and `_sbrk` — which would have quietly undone patch 2.

## Verifying a patch did not change the codec

Patches 1-4 are meant to be bit-exact. They were checked by building both
this tree and pristine upstream on the host and comparing output over six
signals (the project's `meow.mp3` resampled to 8 kHz, silence, Gaussian
noise, an 80 Hz-3.5 kHz sweep, a two-tone buzz, and a full scale square
wave). Bitstreams and decoded audio were identical for all six, and a build
with the checks re-enabled produced identical bitstreams too, so no 40-bit
range violation was being hidden.

To repeat it:

    git clone https://github.com/Rhizomatica/melpe.git /tmp/melpe-ref
    cd /tmp/melpe-ref/melpe && git checkout 16c3e44
    # upstream's Makefile puts -lm before the objects and will not link
    gcc -O2 -w -o /tmp/enc_ref $(ls *.c | grep -vE 'encoder|decoder') encoder.c -lm
    cd <this directory>
    gcc -O2 -w -o /tmp/enc_new $(ls *.c) /tmp/melpe-ref/melpe/encoder.c -lm
    ffmpeg -i ../../sounds/meow.mp3 -ac 1 -ar 8000 -f s16le /tmp/in.pcm
    /tmp/enc_ref /tmp/in.pcm /tmp/a.bit && /tmp/enc_new /tmp/in.pcm /tmp/b.bit
    cmp /tmp/a.bit /tmp/b.bit

## Cost on the board

Built `-O2` for Cortex-M7 (see `cmake/melpe.cmake` for why the level is
forced): **228KB flash, 26KB RAM**, the RAM mostly `npp.c`'s noise
preprocessor state. `-Os` costs 186KB flash instead, untimed.

Nothing calls the codec yet. It is linked into the firmware image, but
`--gc-sections` drops every object with no caller, so today it adds nothing —
the measured firmware is byte-identical with and without it.
