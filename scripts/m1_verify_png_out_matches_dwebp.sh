#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

DWEBP=../../libwebp/examples/dwebp

if [ ! -x "$DWEBP" ]; then
  echo "error: $DWEBP not found or not executable" >&2
  exit 2
fi

tmp_dir=${TMPDIR:-/tmp}/webp_decoder_m1_png_verify
mkdir -p "$tmp_dir"

fail=0
count=0

for f in images/webp/*.webp; do
  [ -e "$f" ] || continue
  count=$((count+1))

  base=$(basename "$f" .webp)
  expected="images/png-out/${base}.png"
  out_png="$tmp_dir/${base}.png"

  if [ ! -f "$expected" ]; then
    echo "MISSING ORACLE PNG: $expected" >&2
    fail=1
    continue
  fi

  "$DWEBP" "$f" -o "$out_png" >/dev/null 2>&1

  h1=$(sha256sum "$out_png" | awk '{print $1}')
  h2=$(sha256sum "$expected" | awk '{print $1}')

  if [ "$h1" != "$h2" ]; then
    echo "MISMATCH: $f" >&2
    echo "  decoded:  $out_png ($h1)" >&2
    echo "  expected: $expected ($h2)" >&2
    fail=1
  fi

done

if [ "$count" -eq 0 ]; then
  echo "error: no files matched images/webp/*.webp" >&2
  exit 2
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: png-out does not match dwebp for one or more files" >&2
  exit 1
fi

echo "OK: $count files matched png-out == dwebp decode"
