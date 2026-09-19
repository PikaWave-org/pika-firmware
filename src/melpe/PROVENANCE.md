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
The underlying code also implements 2400 (`rate = RATE2400`), so adding that
rate was a small wrapper change rather than a port — patch 6 below. It does
**not** implement 600: `sc1200.h` defines `RATE2400` and `RATE1200` and nothing
else, and no `RATE600` appears anywhere in the tree. An earlier version of this
file said otherwise and was wrong.

## Patches

Patches 1-5 exist to make the codec survive in a firmware image with no heap
and no stdio; patch 6 adds the rate the board actually records at. Every one is
marked with a `pika:` comment at the site.

1. `qnt12_cb.c` — added `const` to the eleven codebook definitions. They were
   already declared `extern const` in `qnt12_cb.h`, and the `.c` did not
   include its own header, so nothing caught the mismatch and 64KB of tables
   sat in RAM. Also added that missing include so it cannot recur, and made
   the two dead alias pointers `const` to match.
2. `mat_lib.c` — `v_get`/`L_v_get`/`v_free` used `malloc`. Replaced with a
   1KB fixed arena (bump pointer; freeing a block releases everything handed
   out after it, which covers `lsp_to_lpc`'s out-of-order `f0`/`f1` free).
   The 161-word peak was measured, not guessed — at 1200. The 2400 path peaks
   higher, at **179 of the 256 words**, measured the same way by
   `tools/melpe-check` step 1c: `vq_ms4` (`vq_lib.c:118`) is the largest
   allocator in the tree and is reached only from the `RATE2400` branch at
   `melp_ana.c:169`. It fits, which matters because an overflow here is patch
   5's `assert`, i.e. `__builtin_trap` on the board.
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
6. `melpe.c`, `melpe.h` — `melpe_i24()`, `melpe_a24()`, `melpe_s24()`: the
   2400 bps entry points, 180 samples (22.5 ms) to 7 bytes, where upstream
   wraps 1200 only. The field values are what the original SC1200 driver sets
   for 2400. Two of them are load bearing. `frameSize` must be `FRAME`, not
   `BLOCK` — its only reader is `synthesis()`'s pitch remainder carry
   (`melp_syn.c:116-120`), where `BLOCK` copies 540 samples out of a 180
   sample frame, and copying `melpe_i()` verbatim is the natural mistake. And
   `melpe_a24()` does not call `npp()`: the noise preprocessor is a front end
   rather than part of the bitstream, so the stream is still valid MELPe 2400
   without it, and leaving it uncalled is what keeps `npp.c` out of the image.
   Noisy input encodes worse — `tools/melpe-check`'s seventh signal exists to
   hear that. It also means `melpe_a24()` leaves `sp` intact, unlike
   `melpe_a()`.

## Verifying a patch did not change the codec

Patches 1-4 are meant to be bit-exact. They were checked by building both
this tree and pristine upstream on the host and comparing output over six
signals (the project's `meow.mp3` resampled to 8 kHz, silence, Gaussian
noise, an 80 Hz-3.5 kHz sweep, a two-tone buzz, and a full scale square
wave). Bitstreams and decoded audio were identical for all six, and a build
with the checks re-enabled produced identical bitstreams too, so no 40-bit
range violation was being hidden.

That check ran at **1200**, because upstream's `encoder.c` is 1200 only and
hardcoded, so it never executed most of what the board now uses: `vq_ms4`,
`q_gain`, `q_bpvc`, `vq_enc`, `melp_chn_write` and `fec_code` are reached only
from the `RATE2400` branch of `analysis()`.

`tools/melpe-check` closes that gap and subsumes the recipe that used to be
printed here:

    git clone https://github.com/Rhizomatica/melpe.git /tmp/melpe-ref
    git -C /tmp/melpe-ref checkout 16c3e44
    MELPE_REF=/tmp/melpe-ref/melpe tools/melpe-check

It compares the wrapper against the reference wiring, this tree against
pristine upstream (with and without `MELPE_BASIC_OP_CHECKS`), the arena peak,
and the round trip, over seven signals. All of it passed on 2026-09-19.

## Cost on the board

Built `-O2` for Cortex-M7 (see `cmake/melpe.cmake` for why the level is
forced). The whole library is **228KB flash, 26KB RAM**; what a firmware image
actually pays is less, because `--gc-sections` drops what nothing calls.

`pika::audio::MelpRecorder` calls it at 2400, and in that image the codec
accounts for **186KB of flash and 10.9KB of RAM** (106.6KB text, 79.2KB
rodata). The 15KB of RAM that is not there is `npp.c`, dropped because patch 6
does not call the noise preprocessor and nothing calls the two 1200 wrappers
that would. Nothing warns when that stops being true, so re-check it:

    arm-none-eabi-nm build/targets/firmware/firmware.elf | grep -i npp

What does **not** go is `qnt12.c` and `qnt12_cb.c`, about 74KB of the flash
above. They are the 1200 quantizers, referenced from the `else` branches inside
`analysis()`, and `--gc-sections` works on references rather than reachable
paths — so the lever there is `#if`, not a linker flag.

To measure it again: the map file lists discarded sections alongside kept ones
and over-counts by more than a megabyte. Take the symbols `libmelpe.a` defines,
intersect with `arm-none-eabi-nm -S` over the ELF, and sum the survivors.
