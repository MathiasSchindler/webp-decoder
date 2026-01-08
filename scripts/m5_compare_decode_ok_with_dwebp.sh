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
  echo "error: $DWEBP not found or not executable" >&2
  exit 2
fi

shopt -s nullglob
files=(images/webp/*.webp images/testimages/webp/*.webp)

if (( ${#files[@]} == 0 )); then
  echo "error: no .webp files found under images/webp or images/testimages/webp" >&2
  exit 2
fi

count=0
for f in "${files[@]}"; do
  count=$((count + 1))

  # Our current milestone-5 implementation doesn't output pixels, but it should
  # successfully parse/decode macroblock syntax + coefficient tokens.
  if ! "$DECODER" -info "$f" >/dev/null 2>&1; then
    echo "FAIL: decoder -info failed: $f" >&2
    "$DECODER" -info "$f" >&2 || true
    exit 1
  fi

  # Oracle: ensure libwebp can decode it too.
  if ! "$DWEBP" "$f" -o /dev/null >/dev/null 2>&1; then
    echo "FAIL: dwebp failed: $f" >&2
    "$DWEBP" "$f" -o /dev/null >&2 || true
    exit 1
  fi

done

echo "OK: decoder and dwebp decoded $count files";
