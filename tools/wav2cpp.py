#!/usr/bin/env python3
"""
Turn an audio file into a C++ source/header pair holding its samples.

The output is a plain int16 array in .rodata, which is where audio belongs on
a part with more flash than RAM - 4s of 32kHz mono is 256kB of the 1M bank.

    tools/wav2cpp.py hi_pika.wav --rate 32000 --name hi_pika \
        --out-dir src/pika/audio/samples --namespace pika::audio

What it does to the audio, in order:

  - decodes integer PCM WAV with the standard library and everything else
    through ffmpeg, mixing down to mono for the one converter there is;
  - resamples to --rate with a polyphase windowed sinc filter. The DAC's clock
    is fixed, so another rate plays at the wrong pitch, and dropping samples
    instead would alias into a rasp nothing afterwards removes;
  - normalizes the peak to --peak-dbfs, leaving headroom the resampler's own
    ringing needs on material that already touches full scale.

Requires numpy, and ffmpeg on PATH for anything that is not a WAV file.
Everything else is the standard library.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import shutil
import subprocess
import sys
import wave
from math import gcd
from pathlib import Path

import numpy as np

# Kaiser beta for the resampling window. 8.6 puts the stopband near -90dB,
# which is below the noise floor of 16 bit material and well below that of a
# 12 bit converter.
KAISER_BETA = 8.6

# Filter taps per polyphase branch. The transition band scales inversely with
# this, so it is the knob that trades generation time for how much of the top
# octave survives the resample.
DEFAULT_TAPS_PER_PHASE = 64

# Fraction of the lower Nyquist frequency the passband is allowed to reach.
# The remainder is the transition band, which has to fit somewhere.
ROLLOFF = 0.95

VALUES_PER_LINE = 16


def read_audio(path: Path) -> tuple[np.ndarray, int]:
    """Decode any supported file to mono float64 in [-1, 1), with its rate."""
    if path.suffix.lower() == ".wav":
        return read_wav(path)
    return read_ffmpeg(path)


def read_ffmpeg(path: Path) -> tuple[np.ndarray, int]:
    """
    Decode through ffmpeg, which is what makes mp3 and the rest work.

    The downmix and the 16 bit output are ffmpeg's; the rate deliberately is
    not, so there is one resampler in this pipeline rather than two.
    """
    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        raise SystemExit(f"{path}: ffmpeg and ffprobe are needed to read anything but WAV, and are not on PATH")

    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0",
         "-show_entries", "stream=sample_rate", "-of", "csv=p=0", str(path)],
        capture_output=True, text=True,
    )
    if probe.returncode != 0 or not probe.stdout.strip():
        raise SystemExit(f"{path}: ffprobe found no audio stream\n{probe.stderr.strip()}")

    rate = int(probe.stdout.strip().splitlines()[0])

    decode = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path),
         "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-"],
        capture_output=True,
    )
    if decode.returncode != 0:
        raise SystemExit(f"{path}: ffmpeg failed to decode\n{decode.stderr.decode(errors='replace').strip()}")

    return np.frombuffer(decode.stdout, dtype="<i2").astype(np.float64) / 32768.0, rate


def read_wav(path: Path) -> tuple[np.ndarray, int]:
    """Decode a PCM WAV file to float64 in [-1, 1) and return it with its rate."""
    with wave.open(str(path), "rb") as w:
        if w.getcomptype() != "NONE":
            raise SystemExit(f"{path}: compressed WAV ({w.getcompname()}) is not supported")

        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())

    if width == 1:
        # 8 bit WAV is unsigned by definition, every other width is signed.
        data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
    elif width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif width == 3:
        # No 24 bit dtype exists: pad each sample to 32 bit, keeping the sign
        # in the top byte, then scale by the wider range.
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        padded = np.zeros((b.shape[0], 4), dtype=np.uint8)
        padded[:, 1:] = b
        data = padded.view("<i4").ravel().astype(np.float64) / (1 << 31)
    elif width == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float64) / (1 << 31)
    else:
        raise SystemExit(f"{path}: unsupported sample width of {width} bytes")

    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)

    return data, rate


def design_lowpass(cutoff_norm: float, num_taps: int, gain: float) -> np.ndarray:
    """A windowed sinc lowpass, cutoff in cycles/sample of the rate it runs at."""
    n = np.arange(num_taps) - (num_taps - 1) / 2.0
    h = 2.0 * cutoff_norm * np.sinc(2.0 * cutoff_norm * n)
    return h * np.kaiser(num_taps, KAISER_BETA) * gain


def resample(x: np.ndarray, src_rate: int, dst_rate: int, taps_per_phase: int) -> np.ndarray:
    """
    Rational resampling by L/M, as a polyphase filter.

    This computes the textbook form - stuff L-1 zeros, lowpass, keep every Mth
    - without building the zero stuffed signal, which for 44.1k to 32k would
    be 320x longer and almost all zeros. Each output uses only the taps that
    land on real input, and which those are depends solely on (k*M) % L, so
    the filter splits into L subfilters, each an ordinary FIR over the input.
    """
    g = gcd(src_rate, dst_rate)
    up, down = dst_rate // g, src_rate // g

    if up == 1 and down == 1:
        return x

    # Both the interpolation image and the decimation alias have to be kept
    # out, so the cutoff is the lower of the two Nyquist limits, expressed
    # against the notional upsampled rate that the full filter runs at.
    cutoff_norm = ROLLOFF * 0.5 / max(up, down)

    num_taps = taps_per_phase * max(up, down)
    num_taps += 1 - (num_taps % 2)  # odd, so the delay is a whole sample
    h = design_lowpass(cutoff_norm, num_taps, gain=float(up))

    half = (num_taps - 1) // 2
    out_len = int(math.ceil(len(x) * up / down))
    y = np.zeros(out_len, dtype=np.float64)

    k = np.arange(out_len)
    # The filter's own delay, in upsampled samples, taken off the input index
    # so the output lines up with the input instead of starting with silence.
    idx = k * down + half
    phases = idx % up
    heads = idx // up

    for p in range(up):
        # h[p::up] reversed is the subfilter in convolution order; np.convolve
        # handles the edges, and x is padded so heads can reach past the end.
        sub = h[p::up]
        sel = phases == p
        if not np.any(sel):
            continue
        conv = np.convolve(x, sub)
        q = heads[sel]
        valid = q < len(conv)
        out_idx = k[sel][valid]
        y[out_idx] = conv[q[valid]]

    return y


def normalize(x: np.ndarray, peak_dbfs: float) -> tuple[np.ndarray, float]:
    """Scale so the loudest sample sits at peak_dbfs. Returns the gain used."""
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    if peak == 0.0:
        return x, 1.0
    target = 10.0 ** (peak_dbfs / 20.0)
    gain = target / peak
    return x * gain, gain


def to_int16(x: np.ndarray) -> np.ndarray:
    """Quantize to int16, clamping rather than wrapping on the way."""
    scaled = np.rint(x * 32767.0)
    return np.clip(scaled, -32768, 32767).astype(np.int16)


def format_value(v: int, style: str) -> str:
    """
    One sample, as short as it can be written and still be an int16_t.

    Hex is signed - -0x1f40, not the two's complement pattern - because 0xe0c0
    in a braced initializer is 57536, a narrowing error rather than the
    negative number it looks like.
    """
    if style == "hex":
        return f"-0x{-v:x}" if v < 0 else f"0x{v:x}"
    return str(v)


def format_array(samples: np.ndarray, style: str) -> str:
    lines = []
    for start in range(0, len(samples), VALUES_PER_LINE):
        chunk = samples[start : start + VALUES_PER_LINE]
        lines.append("    " + ",".join(format_value(int(v), style) for v in chunk) + ",")
    return "\n".join(lines)


def banner(src: Path, digest: str, args, in_rate: int, in_len: int, out_len: int, gain: float) -> str:
    return f"""/*
 * Generated by tools/wav2cpp.py - do not edit.
 *
 *   source     {src.name} ({digest[:16]})
 *   input      {in_rate}Hz, {in_len} samples, {in_len / in_rate:.3f}s
 *   output     {args.rate}Hz, {out_len} samples, {out_len / args.rate:.3f}s
 *   normalized peak to {args.peak_dbfs:g} dBFS (gain x{gain:.4f})
 *
 * Regenerate with:
 *   tools/wav2cpp.py {src} --rate {args.rate} --name {args.name} \\
 *       --out-dir {args.out_dir} --namespace {args.namespace} --format {args.format}
 */"""


def emit(args, samples: np.ndarray, src: Path, digest: str, in_rate: int, in_len: int, gain: float) -> None:
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    head = banner(src, digest, args, in_rate, in_len, len(samples), gain)
    ns_open = f"namespace {args.namespace} {{" if args.namespace else ""
    ns_close = f"}}// namespace {args.namespace}" if args.namespace else ""

    header = f"""{head}

#pragma once

#include <cstddef>
#include <cstdint>

{ns_open}

/** @brief  Sample rate the data was resampled to, in Hz. */
constexpr uint32_t {args.name}_sample_rate = {args.rate};

/** @brief  Number of samples in {args.name}. */
constexpr size_t {args.name}_len = {len(samples)};

/** @brief  Mono 16 bit PCM, signed, in flash. */
extern const int16_t {args.name}[{args.name}_len];

{ns_close}
"""

    # The definition has to say extern too: a const at namespace scope has
    # internal linkage in C++ by default, and the header's declaration would
    # then find nothing to bind to at link time.
    source = f"""{head}

#include "{args.name}.h"

{ns_open}

extern const int16_t {args.name}[{args.name}_len] = {{
{format_array(samples, args.format)}
}};

{ns_close}
"""

    (out_dir / f"{args.name}.h").write_text(header)
    (out_dir / f"{args.name}.cpp").write_text(source)


def main() -> int:
    p = argparse.ArgumentParser(description="Embed an audio file as a C++ int16 array.")
    p.add_argument("wav", type=Path, help="input file: WAV directly, anything else through ffmpeg")
    p.add_argument("--name", help="C++ identifier for the array (default: the file stem)")
    p.add_argument("--rate", type=int, default=32000, help="output sample rate in Hz (default: 32000)")
    p.add_argument("--out-dir", default=".", help="directory to write <name>.h and <name>.cpp into")
    p.add_argument("--namespace", default="", help="C++ namespace to wrap the data in")
    p.add_argument("--peak-dbfs", type=float, default=-1.0, help="normalization target (default: -1.0)")
    p.add_argument("--no-normalize", action="store_true", help="leave the level alone")
    p.add_argument("--taps-per-phase", type=int, default=DEFAULT_TAPS_PER_PHASE,
                   help=f"resampler filter length per phase (default: {DEFAULT_TAPS_PER_PHASE})")
    p.add_argument("--format", choices=("hex", "dec"), default="hex",
                   help="how samples are written (default: hex)")
    args = p.parse_args()

    if not args.wav.is_file():
        raise SystemExit(f"{args.wav}: no such file")
    if args.rate <= 0:
        raise SystemExit("--rate must be positive")
    if args.name is None:
        args.name = args.wav.stem.replace("-", "_").replace(" ", "_")
    if not args.name.isidentifier():
        raise SystemExit(f"{args.name!r} is not a usable C++ identifier")

    digest = hashlib.sha256(args.wav.read_bytes()).hexdigest()

    audio, in_rate = read_audio(args.wav)
    in_len = len(audio)
    if in_len == 0:
        raise SystemExit(f"{args.wav}: no audio in file")

    audio = resample(audio, in_rate, args.rate, args.taps_per_phase)

    gain = 1.0
    if not args.no_normalize:
        audio, gain = normalize(audio, args.peak_dbfs)

    clipped = int(np.sum(np.abs(audio) > 1.0))
    if clipped:
        print(f"warning: {clipped} samples clipped", file=sys.stderr)

    samples = to_int16(audio)
    emit(args, samples, args.wav, digest, in_rate, in_len, gain)

    print(f"{args.name}: {len(samples)} samples at {args.rate}Hz, "
          f"{len(samples) * 2 / 1024:.1f}kB -> {args.out_dir}/{args.name}.{{h,cpp}}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
