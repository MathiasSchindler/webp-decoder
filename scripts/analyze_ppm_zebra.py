#!/usr/bin/env python3
"""Analyze potential vertical 'zebra' artifacts in PPM outputs.

Given a tmp artifact directory produced by scripts/enc_vs_cwebp_quality.sh,
this script compares per-column mean luma of `*_ours_q*.ppm` against `*.ref.ppm`
(and optionally `*_lib_q*.ppm`) and reports whether a strong 16px periodic bias
exists (macroblock-aligned vertical banding).

This is intentionally dependency-free (no numpy/pillow).
"""

from __future__ import annotations

import argparse
import math
import os
from typing import Iterable, Tuple


def read_ppm_p6(path: str) -> Tuple[int, int, bytes]:
    with open(path, "rb") as fp:
        magic = fp.readline().strip()
        if magic != b"P6":
            raise ValueError(f"{path}: expected P6, got {magic!r}")

        tokens: list[bytes] = []
        while len(tokens) < 3:
            line = fp.readline()
            if not line:
                raise ValueError(f"{path}: EOF in header")
            line = line.strip()
            if not line or line.startswith(b"#"):
                continue
            tokens.extend(line.split())

        width, height, maxv = map(int, tokens[:3])
        if maxv != 255:
            raise ValueError(f"{path}: maxv={maxv}, expected 255")

        data = fp.read(width * height * 3)
        if len(data) != width * height * 3:
            raise ValueError(f"{path}: truncated RGB data ({len(data)} bytes)")

        return width, height, data


def col_mean_luma(width: int, height: int, rgb: bytes) -> list[float]:
    """Return per-column mean luma (approx Y') in [0..255]."""

    # Y ≈ (77 R + 150 G + 29 B) >> 8
    sums = [0] * width
    idx = 0
    for _y in range(height):
        for x in range(width):
            r = rgb[idx]
            g = rgb[idx + 1]
            b = rgb[idx + 2]
            idx += 3
            yv = (77 * r + 150 * g + 29 * b) >> 8
            sums[x] += yv
    return [s / height for s in sums]


def col_mean_rgb(width: int, height: int, rgb: bytes) -> tuple[list[float], list[float], list[float]]:
    """Return per-column mean R,G,B in [0..255]."""
    acc_r = [0] * width
    acc_g = [0] * width
    acc_b = [0] * width
    idx = 0
    for _y in range(height):
        for x in range(width):
            r = rgb[idx]
            g = rgb[idx + 1]
            b = rgb[idx + 2]
            idx += 3
            acc_r[x] += r
            acc_g[x] += g
            acc_b[x] += b
    return ([v / height for v in acc_r], [v / height for v in acc_g], [v / height for v in acc_b])


def col_mean_ycbcr(width: int, height: int, rgb: bytes) -> tuple[list[float], list[float], list[float]]:
    """Return per-column mean Y,Cb,Cr in [0..255] (approx BT.601 full-range)."""
    acc_y = [0] * width
    acc_cb = [0] * width
    acc_cr = [0] * width
    idx = 0
    for _y in range(height):
        for x in range(width):
            r = rgb[idx]
            g = rgb[idx + 1]
            b = rgb[idx + 2]
            idx += 3
            yv = (77 * r + 150 * g + 29 * b) >> 8
            cb = (((-43 * r - 85 * g + 128 * b) >> 8) + 128)
            cr = (((128 * r - 107 * g - 21 * b) >> 8) + 128)
            acc_y[x] += yv
            acc_cb[x] += cb
            acc_cr[x] += cr
    return ([v / height for v in acc_y], [v / height for v in acc_cb], [v / height for v in acc_cr])


def stats(values: Iterable[float]) -> Tuple[float, float, float, float]:
    v = list(values)
    mean = sum(v) / len(v)
    rms = math.sqrt(sum((x - mean) * (x - mean) for x in v) / len(v))
    return mean, rms, min(v), max(v)


def phase_means(values: list[float], period: int) -> Tuple[list[float], float]:
    acc = [0.0] * period
    cnt = [0] * period
    for i, x in enumerate(values):
        p = i % period
        acc[p] += x
        cnt[p] += 1
    means = [acc[p] / cnt[p] for p in range(period)]
    m = sum(means) / period
    phase_rms = math.sqrt(sum((x - m) * (x - m) for x in means) / period)
    return means, phase_rms


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tmpdir", help="enc_vs_cwebp_quality tmp.* directory")
    ap.add_argument("--stem", default="commons-00018", help="image stem (no size suffix)")
    ap.add_argument("--size", type=int, default=1024, help="size (as in *_<size>_...) ")
    ap.add_argument("--q", type=int, default=30, help="quality q (as in *_q<q>.ppm)")
    ap.add_argument("--period", type=int, default=16, help="period to test (16=macroblock)")
    args = ap.parse_args()

    stem = f"{args.stem}_{args.size}"
    ref_path = os.path.join(args.tmpdir, f"{stem}.ref.ppm")
    ours_path = os.path.join(args.tmpdir, f"{stem}_ours_q{args.q}.ppm")
    lib_path = os.path.join(args.tmpdir, f"{stem}_lib_q{args.q}.ppm")

    for p in (ref_path, ours_path):
        if not os.path.exists(p):
            raise SystemExit(f"missing: {p}")

    w, h, ref_rgb = read_ppm_p6(ref_path)
    w2, h2, ours_rgb = read_ppm_p6(ours_path)
    if (w, h) != (w2, h2):
        raise SystemExit("dimension mismatch between ref and ours")

    ref_y = col_mean_luma(w, h, ref_rgb)
    ours_y = col_mean_luma(w, h, ours_rgb)
    d_ours = [ours_y[i] - ref_y[i] for i in range(w)]

    print(f"== {args.tmpdir} ==")
    print(f"image: {stem} q={args.q}  size={w}x{h}")

    mean, rms, vmin, vmax = stats(d_ours)
    ph, pr = phase_means(d_ours, args.period)
    print(f"ours - ref (col-mean luma): mean={mean:+.3f} rms={rms:.3f} min={vmin:+.3f} max={vmax:+.3f}")
    print(f"periodic bias: period={args.period} phase_rms={pr:.4f}")
    print("phase means (x % period):")
    print(" ".join(f"{x:+.3f}" for x in ph))

    ref_r, ref_g, ref_b = col_mean_rgb(w, h, ref_rgb)
    ours_r, ours_g, ours_b = col_mean_rgb(w, h, ours_rgb)
    dr = [ours_r[i] - ref_r[i] for i in range(w)]
    dg = [ours_g[i] - ref_g[i] for i in range(w)]
    db = [ours_b[i] - ref_b[i] for i in range(w)]
    my, mrms, _, _ = stats(dr)
    print(f"ours - ref (col-mean R):    mean={my:+.3f} rms={mrms:.3f}")
    my, mrms, _, _ = stats(dg)
    print(f"ours - ref (col-mean G):    mean={my:+.3f} rms={mrms:.3f}")
    my, mrms, _, _ = stats(db)
    print(f"ours - ref (col-mean B):    mean={my:+.3f} rms={mrms:.3f}")

    ref_y2, ref_cb, ref_cr = col_mean_ycbcr(w, h, ref_rgb)
    ours_y2, ours_cb, ours_cr = col_mean_ycbcr(w, h, ours_rgb)
    dcb = [ours_cb[i] - ref_cb[i] for i in range(w)]
    dcr = [ours_cr[i] - ref_cr[i] for i in range(w)]
    my, mrms, _, _ = stats(dcb)
    ph, pr = phase_means(dcb, args.period)
    print(f"ours - ref (col-mean Cb):   mean={my:+.3f} rms={mrms:.3f}  phase_rms={pr:.4f}")
    my, mrms, _, _ = stats(dcr)
    ph, pr = phase_means(dcr, args.period)
    print(f"ours - ref (col-mean Cr):   mean={my:+.3f} rms={mrms:.3f}  phase_rms={pr:.4f}")

    if os.path.exists(lib_path):
        w3, h3, lib_rgb = read_ppm_p6(lib_path)
        if (w, h) == (w3, h3):
            lib_y = col_mean_luma(w, h, lib_rgb)
            d_lib = [lib_y[i] - ref_y[i] for i in range(w)]
            mean, rms, vmin, vmax = stats(d_lib)
            ph, pr = phase_means(d_lib, args.period)
            print()
            print(f"lib - ref (col-mean luma): mean={mean:+.3f} rms={rms:.3f} min={vmin:+.3f} max={vmax:+.3f}")
            print(f"lib periodic bias: period={args.period} phase_rms={pr:.4f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
