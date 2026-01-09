#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

. "$ROOT_DIR/scripts/common.sh"
require_libwebp_webpinfo

DECODER=./decoder

if [ ! -x "$DECODER" ]; then
  echo "error: $DECODER not found or not executable; run 'make' first" >&2
  exit 2
fi


fail=0
count=0

check_glob() {
  pattern=$1
  for f in $pattern; do
    [ -e "$f" ] || continue
    count=$((count+1))

    mine=$($DECODER -info "$f" | awk '/^  Part\. [0-9]+ length:/{print $2, $4}')
    oracle=$($WEBPINFO -bitstream_info "$f" | awk '/^  Part\. [0-9]+ length:/{print $2, $4}')

    if [ "$mine" != "$oracle" ]; then
      echo "MISMATCH: $f" >&2
      echo "  mine:" >&2
      printf "    %s\n" "$mine" >&2
      echo "  oracle:" >&2
      printf "    %s\n" "$oracle" >&2
      fail=1
    fi
  done
}

check_glob "images/webp/*.webp"
check_glob "images/testimages/webp/*.webp"

if [ "$count" -eq 0 ]; then
  echo "error: no files matched images/webp/*.webp or images/testimages/webp/*.webp" >&2
  exit 2
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: partition lines mismatched webpinfo -bitstream_info" >&2
  exit 1
fi

echo "OK: $count files matched webpinfo for all 'Part. i length' lines"
