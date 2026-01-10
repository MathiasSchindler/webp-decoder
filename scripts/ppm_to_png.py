#!/usr/bin/env python3
"""Convert binary PPM (P6) to PNG, optionally writing a diff visualization.

Dependency-free helper for inspecting build artifacts such as those produced by
scripts/enc_vs_cwebp_quality.sh.

Usage:
  python3 scripts/ppm_to_png.py in.ppm out.png
  python3 scripts/ppm_to_png.py --diff ref.ppm test.ppm out.png [--scale 8]

Diff mode outputs an 8-bit grayscale image where each pixel is:
  v = clip(scale * |Y_test - Y_ref|)
with Y computed as (77R + 150G + 29B) >> 8.
"""

from __future__ import annotations

import argparse
import binascii
import struct
import zlib


def read_ppm_p6(path: str) -> tuple[int, int, bytes]:
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
            raise ValueError(f"{path}: truncated RGB data")
        return width, height, data


def _png_chunk(typ: bytes, data: bytes) -> bytes:
    crc = binascii.crc32(typ)
    crc = binascii.crc32(data, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + typ + data + struct.pack(">I", crc)


def write_png_rgb(path: str, width: int, height: int, rgb: bytes) -> None:
    # PNG scanlines: filter byte (0) + RGB bytes
    stride = width * 3
    raw = bytearray()
    off = 0
    for _y in range(height):
        raw.append(0)
        raw.extend(rgb[off : off + stride])
        off += stride

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    idat = zlib.compress(bytes(raw), level=6)

    out = bytearray()
    out.extend(b"\x89PNG\r\n\x1a\n")
    out.extend(_png_chunk(b"IHDR", ihdr))
    out.extend(_png_chunk(b"IDAT", idat))
    out.extend(_png_chunk(b"IEND", b""))

    with open(path, "wb") as fp:
        fp.write(out)


def write_png_gray(path: str, width: int, height: int, gray: bytes) -> None:
    stride = width
    raw = bytearray()
    off = 0
    for _y in range(height):
        raw.append(0)
        raw.extend(gray[off : off + stride])
        off += stride

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)  # 8-bit grayscale
    idat = zlib.compress(bytes(raw), level=6)

    out = bytearray()
    out.extend(b"\x89PNG\r\n\x1a\n")
    out.extend(_png_chunk(b"IHDR", ihdr))
    out.extend(_png_chunk(b"IDAT", idat))
    out.extend(_png_chunk(b"IEND", b""))

    with open(path, "wb") as fp:
        fp.write(out)


def diff_luma_gray(ref_rgb: bytes, test_rgb: bytes, scale: int) -> bytes:
    if len(ref_rgb) != len(test_rgb):
        raise ValueError("diff: size mismatch")

    out = bytearray(len(ref_rgb) // 3)
    j = 0
    for i in range(0, len(ref_rgb), 3):
        rr, rg, rb = ref_rgb[i], ref_rgb[i + 1], ref_rgb[i + 2]
        tr, tg, tb = test_rgb[i], test_rgb[i + 1], test_rgb[i + 2]
        y_ref = (77 * rr + 150 * rg + 29 * rb) >> 8
        y_tst = (77 * tr + 150 * tg + 29 * tb) >> 8
        v = abs(y_tst - y_ref) * scale
        out[j] = 255 if v > 255 else v
        j += 1
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--diff", action="store_true", help="write luma diff heatmap")
    ap.add_argument("--scale", type=int, default=8, help="diff amplification")
    ap.add_argument("paths", nargs="+", help="convert: in.ppm out.png | diff: ref.ppm test.ppm out.png")
    args = ap.parse_args()

    if not args.diff:
        if len(args.paths) != 2:
            raise SystemExit("convert mode requires: in.ppm out.png")
        in_ppm, out_png = args.paths
        w, h, rgb = read_ppm_p6(in_ppm)
        write_png_rgb(out_png, w, h, rgb)
        return 0

    if len(args.paths) != 3:
        raise SystemExit("diff mode requires: ref.ppm test.ppm out.png")
    ref_ppm, test_ppm, out_png = args.paths
    w, h, ref_rgb = read_ppm_p6(ref_ppm)
    w2, h2, test_rgb = read_ppm_p6(test_ppm)
    if (w, h) != (w2, h2):
        raise SystemExit("dimension mismatch")

    gray = diff_luma_gray(ref_rgb, test_rgb, args.scale)
    write_png_gray(out_png, w, h, gray)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
