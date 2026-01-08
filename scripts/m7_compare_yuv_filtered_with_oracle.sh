#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

DECODER=./decoder
DWEBP=../../libwebp/examples/dwebp
FFMPEG=ffmpeg

if [[ ! -x "$DECODER" ]]; then
  echo "error: $DECODER not found; run 'make' first" >&2
  exit 2
fi

use_oracle=""
if [[ -x "$DWEBP" ]]; then
  use_oracle="dwebp"
elif command -v "$FFMPEG" >/dev/null 2>&1; then
  use_oracle="ffmpeg"
else
  echo "error: neither $DWEBP nor ffmpeg found; install libwebp tools or ffmpeg" >&2
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

  ours="$tmpdir/ours.i420"
  oracle="$tmpdir/oracle.i420"

  if ! "$DECODER" -yuvf "$f" "$ours" >/dev/null 2>&1; then
    echo "FAIL: decoder -yuvf failed: $f" >&2
    "$DECODER" -info "$f" >&2 || true
    exit 1
  fi

  if [[ "$use_oracle" == "dwebp" ]]; then
    if ! "$DWEBP" -quiet -yuv "$f" -o "$oracle" >/dev/null 2>&1; then
      echo "FAIL: dwebp -yuv failed: $f" >&2
      "$DWEBP" -yuv "$f" -o "$oracle" >&2 || true
      exit 1
    fi
  else
    if ! "$FFMPEG" -v error -i "$f" -f rawvideo -pix_fmt yuv420p -frames:v 1 "$oracle" >/dev/null 2>&1; then
      echo "FAIL: ffmpeg decode failed: $f" >&2
      exit 1
    fi
  fi

  if ! cmp -s "$ours" "$oracle"; then
    echo "FAIL: YUV mismatch: $f" >&2
    echo "  ours:   $(sha256sum "$ours" | awk '{print $1}')" >&2
    echo "  oracle: $(sha256sum "$oracle" | awk '{print $1}')" >&2
    exit 1
  fi

done

echo "OK: decoder -yuvf matches oracle ($use_oracle, filtered) for $count files";
