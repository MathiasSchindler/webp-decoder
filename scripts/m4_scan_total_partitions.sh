#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

. "$ROOT_DIR/scripts/common.sh"
require_libwebp_webpinfo

max=0
count=0
gt1=0

scan_glob() {
  pattern=$1
  for f in $pattern; do
    [ -e "$f" ] || continue
    count=$((count+1))

    tp=$($WEBPINFO -bitstream_info "$f" 2>/dev/null | awk -F': ' '/Total partitions:/{print $2; exit}')
    [ -n "${tp:-}" ] || tp=0

    if [ "$tp" -gt "$max" ]; then
      max=$tp
    fi
    if [ "$tp" -gt 1 ]; then
      gt1=$((gt1+1))
      echo "$tp $f"
    fi
  done
}

scan_glob "images/webp/*.webp"
scan_glob "images/testimages/webp/*.webp"

if [ "$count" -eq 0 ]; then
  echo "error: no .webp files found under images/webp or images/testimages/webp" >&2
  exit 2
fi

if [ "$gt1" -eq 0 ]; then
  echo "OK: scanned $count files; max Total partitions: $max (no files with > 1)"
else
  echo "NOTE: found $gt1 files with Total partitions > 1 (max: $max)"
fi
