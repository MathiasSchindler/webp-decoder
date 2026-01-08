#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

DECODER=./decoder
DWEBP=../../libwebp/examples/dwebp

if [[ ! -x "$DECODER" ]]; then
  echo "error: $DECODER not found; run 'make' first" >&2
  exit 2
fi

if [[ ! -x "$DWEBP" ]]; then
  echo "error: $DWEBP not found/executable; build libwebp tools" >&2
  exit 2
fi

shopt -s nullglob
files=(images/webp/*.webp images/testimages/webp/*.webp images/generated/webp/*.webp)

if (( ${#files[@]} == 0 )); then
  echo "error: no .webp files found under images/webp, images/testimages/webp, or images/generated/webp" >&2
  exit 2
fi

tmpdir="$(mktemp -d)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

count=0
for f in "${files[@]}"; do
  count=$((count + 1))

  ours="$tmpdir/ours.ppm"
  oracle="$tmpdir/oracle.ppm"

  if ! "$DECODER" -ppm "$f" "$ours" >/dev/null 2>&1; then
    echo "FAIL: decoder -ppm failed: $f" >&2
    "$DECODER" -info "$f" >&2 || true
    exit 1
  fi

  if ! "$DWEBP" -quiet "$f" -ppm -o "$oracle" >/dev/null 2>&1; then
    echo "FAIL: dwebp -ppm failed: $f" >&2
    "$DWEBP" "$f" -ppm -o "$oracle" >&2 || true
    exit 1
  fi

  if ! cmp -s "$ours" "$oracle"; then
    OUT=$(OURS="$ours" ORACLE="$oracle" python3 - <<'PY'
import sys
from pathlib import Path
import os

def read_ppm(path: str):
  data = Path(path).read_bytes()
  if not data.startswith(b"P6\n"):
    raise SystemExit("not a P6 ppm")
  i = 3
  j = data.find(b"\n", i)
  w, h = map(int, data[i:j].split())
  i = j + 1
  j = data.find(b"\n", i)
  maxv = int(data[i:j])
  if maxv != 255:
    raise SystemExit("unexpected maxval")
  i = j + 1
  pix = data[i:i + w * h * 3]
  if len(pix) != w * h * 3:
    raise SystemExit("truncated ppm")
  return w, h, pix

w1, h1, a = read_ppm(os.environ["OURS"])
w2, h2, b = read_ppm(os.environ["ORACLE"])
if (w1, h1) != (w2, h2) or len(a) != len(b):
  print("SIZE_MISMATCH")
  sys.exit(2)

mx = 0
for x, y in zip(a, b):
  d = x - y
  if d < 0:
    d = -d
  if d > mx:
    mx = d
print(mx)
PY
    )
    echo "FAIL: PPM mismatch (max_abs_diff=$OUT): $f" >&2
    exit 1
  fi

done

echo "OK: decoder -ppm matches dwebp -ppm byte-identically for $count files";
