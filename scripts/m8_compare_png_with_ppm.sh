#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

DECODER=./decoder

if [[ ! -x "$DECODER" ]]; then
  echo "error: $DECODER not found; run 'make' first" >&2
  exit 2
fi

shopt -s nullglob
files=(images/webp/*.webp images/testimages/webp/*.webp images/generated/webp/*.webp)

if (( ${#files[@]} == 0 )); then
  echo "error: no .webp files found under images/webp, images/testimages/webp, or images/generated/webp" >&2
  exit 2
fi

FILES="$(printf '%s\n' "${files[@]}")" DECODER="$DECODER" python3 - <<'PY'
import os
import struct
import subprocess
import tempfile
from pathlib import Path


def read_ppm(path: str):
  data = Path(path).read_bytes()
  if not data.startswith(b"P6\n"):
    raise RuntimeError("not a P6 ppm")
  i = 3
  j = data.find(b"\n", i)
  w, h = map(int, data[i:j].split())
  i = j + 1
  j = data.find(b"\n", i)
  maxv = int(data[i:j])
  if maxv != 255:
    raise RuntimeError("unexpected maxval")
  i = j + 1
  pix = data[i:i + w * h * 3]
  if len(pix) != w * h * 3:
    raise RuntimeError("truncated ppm")
  return w, h, pix


def parse_png_rgb8_uncompressed(path: str):
  b = Path(path).read_bytes()
  sig = bytes([0x89, ord('P'), ord('N'), ord('G'), 0x0D, 0x0A, 0x1A, 0x0A])
  if len(b) < 8 or b[:8] != sig:
    raise RuntimeError("bad png signature")
  p = 8
  width = height = None
  idat = bytearray()

  def u32be(off: int) -> int:
    return struct.unpack(">I", b[off:off+4])[0]

  while p + 8 <= len(b):
    ln = u32be(p)
    typ = b[p+4:p+8]
    p += 8
    if p + ln + 4 > len(b):
      raise RuntimeError("truncated chunk")
    dat = b[p:p+ln]
    p += ln
    p += 4  # crc
    if typ == b"IHDR":
      if ln != 13:
        raise RuntimeError("bad IHDR")
      width = struct.unpack(">I", dat[0:4])[0]
      height = struct.unpack(">I", dat[4:8])[0]
      bit_depth = dat[8]
      color_type = dat[9]
      comp = dat[10]
      flt = dat[11]
      interlace = dat[12]
      if bit_depth != 8 or color_type != 2 or comp != 0 or flt != 0 or interlace != 0:
        raise RuntimeError("unsupported png format")
    elif typ == b"IDAT":
      idat += dat
    elif typ == b"IEND":
      break

  if width is None or height is None:
    raise RuntimeError("missing IHDR")
  if len(idat) < 6:
    raise RuntimeError("missing IDAT")

  # zlib stream (expect 0x78 0x01 + stored deflate blocks)
  if idat[0] != 0x78 or idat[1] != 0x01:
    raise RuntimeError("unexpected zlib header")
  q = 2
  out = bytearray()
  while True:
    if q >= len(idat):
      raise RuntimeError("truncated deflate")
    hdr = idat[q]
    q += 1
    bfinal = hdr & 1
    btype = (hdr >> 1) & 3
    if btype != 0:
      raise RuntimeError("expected stored deflate blocks")
    if q + 4 > len(idat):
      raise RuntimeError("truncated stored header")
    ln = idat[q] | (idat[q+1] << 8)
    nln = idat[q+2] | (idat[q+3] << 8)
    q += 4
    if ((ln ^ 0xFFFF) & 0xFFFF) != nln:
      raise RuntimeError("bad stored nlen")
    if q + ln > len(idat):
      raise RuntimeError("truncated stored payload")
    out += idat[q:q+ln]
    q += ln
    if bfinal:
      break

  expected = height * (1 + width * 3)
  if len(out) != expected:
    raise RuntimeError(f"unexpected raw size: {len(out)} != {expected}")

  pix = bytearray(width * height * 3)
  src = 0
  dst = 0
  for _y in range(height):
    flt = out[src]
    src += 1
    if flt != 0:
      raise RuntimeError("unsupported filter")
    pix[dst:dst + width * 3] = out[src:src + width * 3]
    src += width * 3
    dst += width * 3

  return width, height, bytes(pix)


decoder = os.environ.get("DECODER", "./decoder")
files = os.environ.get("FILES", "").splitlines()
if not files:
  raise SystemExit("no files provided")

with tempfile.TemporaryDirectory() as td:
  td = Path(td)
  for i, f in enumerate(files, 1):
    ppm = td / "out.ppm"
    png = td / "out.png"
    try:
      subprocess.run([decoder, "-ppm", f, str(ppm)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
      print(f"FAIL: decoder -ppm failed: {f}")
      raise SystemExit(1)
    try:
      subprocess.run([decoder, "-png", f, str(png)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
      print(f"FAIL: decoder -png failed: {f}")
      raise SystemExit(1)

    w1, h1, ppm_pix = read_ppm(str(ppm))
    w2, h2, png_pix = parse_png_rgb8_uncompressed(str(png))
    if (w1, h1) != (w2, h2):
      print(f"FAIL: SIZE_MISMATCH {w1}x{h1} vs {w2}x{h2}: {f}")
      raise SystemExit(1)
    if ppm_pix != png_pix:
      mx = 0
      for a, b in zip(ppm_pix, png_pix):
        d = a - b
        if d < 0:
          d = -d
        if d > mx:
          mx = d
      print(f"FAIL: PIXEL_MISMATCH max_abs_diff={mx}: {f}")
      raise SystemExit(1)

print(f"OK: decoder -png matches decoder -ppm RGB bytes for {len(files)} files")
PY
