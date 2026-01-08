#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

DECODER=./decoder
WEBPINFO=../../libwebp/examples/webpinfo

if [ ! -x "$DECODER" ]; then
  echo "error: $DECODER not found or not executable; run 'make' first" >&2
  exit 2
fi

if [ ! -x "$WEBPINFO" ]; then
  echo "error: $WEBPINFO not found or not executable" >&2
  exit 2
fi

fail=0
count=0

for f in images/webp/*.webp; do
  [ -e "$f" ] || continue
  count=$((count+1))

  # Our tool output
  mine_riff_total=$($DECODER -info "$f" | awk -F'[(), ]+' '/^RIFF size:/{print $(NF-1)}')
  mine_vp8_off=$($DECODER -info "$f" | awk '/^Chunk VP8/{print $5}')
  mine_vp8_len=$($DECODER -info "$f" | awk '/^Chunk VP8/{print $7}')

  # Oracle output
  oracle_riff_total=$($WEBPINFO "$f" | awk '/File size:/{print $3}')
  oracle_vp8_off=$($WEBPINFO "$f" | awk '/^Chunk VP8/{print $5}')
  oracle_vp8_len=$($WEBPINFO "$f" | awk '/^Chunk VP8/{print $7}')

  ok=1
  if [ "$mine_riff_total" != "$oracle_riff_total" ]; then ok=0; fi
  if [ "$mine_vp8_off" != "$oracle_vp8_off" ]; then ok=0; fi
  if [ "$mine_vp8_len" != "$oracle_vp8_len" ]; then ok=0; fi

  if [ "$ok" -ne 1 ]; then
    echo "MISMATCH: $f" >&2
    echo "  riff_total mine=$mine_riff_total oracle=$oracle_riff_total" >&2
    echo "  vp8_off    mine=$mine_vp8_off oracle=$oracle_vp8_off" >&2
    echo "  vp8_len    mine=$mine_vp8_len oracle=$oracle_vp8_len" >&2
    fail=1
  fi

done

if [ "$count" -eq 0 ]; then
  echo "error: no files matched images/webp/*.webp" >&2
  exit 2
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: info output mismatched webpinfo for one or more files" >&2
  exit 1
fi

echo "OK: $count files matched webpinfo (RIFF total size + VP8 chunk offset/length)"
