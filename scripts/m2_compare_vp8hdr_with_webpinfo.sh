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

  mine_key=$($DECODER -info "$f" | awk -F': *' '/^  Key frame:/{print $2}')
  mine_prof=$($DECODER -info "$f" | awk -F': *' '/^  Profile:/{print $2}')
  mine_disp=$($DECODER -info "$f" | awk -F': *' '/^  Display:/{print $2}')
  mine_p0=$($DECODER -info "$f" | awk -F': *' '/^  Part\. 0 length:/{print $2}')
  mine_w=$($DECODER -info "$f" | awk -F': *' '/^  Width:/{print $2}' | tail -n 1)
  mine_xs=$($DECODER -info "$f" | awk -F': *' '/^  X scale:/{print $2}')
  mine_h=$($DECODER -info "$f" | awk -F': *' '/^  Height:/{print $2}' | tail -n 1)
  mine_ys=$($DECODER -info "$f" | awk -F': *' '/^  Y scale:/{print $2}')

  oracle=$($WEBPINFO -bitstream_info "$f")
  o_key=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Key frame:/{print $2}')
  o_prof=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Profile:/{print $2}')
  o_disp=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Display:/{print $2}')
  o_p0=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Part\. 0 length:/{print $2}')
  o_w=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Width:/{print $2}' | tail -n 1)
  o_xs=$(printf "%s\n" "$oracle" | awk -F': *' '/^  X scale:/{print $2}')
  o_h=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Height:/{print $2}' | tail -n 1)
  o_ys=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Y scale:/{print $2}')

  ok=1
  if [ "$mine_key" != "$o_key" ]; then ok=0; fi
  if [ "$mine_prof" != "$o_prof" ]; then ok=0; fi
  if [ "$mine_disp" != "$o_disp" ]; then ok=0; fi
  if [ "$mine_p0" != "$o_p0" ]; then ok=0; fi
  if [ "$mine_w" != "$o_w" ]; then ok=0; fi
  if [ "$mine_xs" != "$o_xs" ]; then ok=0; fi
  if [ "$mine_h" != "$o_h" ]; then ok=0; fi
  if [ "$mine_ys" != "$o_ys" ]; then ok=0; fi

  if [ "$ok" -ne 1 ]; then
    echo "MISMATCH: $f" >&2
    echo "  Key frame mine=$mine_key oracle=$o_key" >&2
    echo "  Profile   mine=$mine_prof oracle=$o_prof" >&2
    echo "  Display   mine=$mine_disp oracle=$o_disp" >&2
    echo "  Part0Len  mine=$mine_p0 oracle=$o_p0" >&2
    echo "  Width     mine=$mine_w oracle=$o_w" >&2
    echo "  X scale   mine=$mine_xs oracle=$o_xs" >&2
    echo "  Height    mine=$mine_h oracle=$o_h" >&2
    echo "  Y scale   mine=$mine_ys oracle=$o_ys" >&2
    fail=1
  fi

done

if [ "$count" -eq 0 ]; then
  echo "error: no files matched images/webp/*.webp" >&2
  exit 2
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: vp8 header fields mismatched webpinfo -bitstream_info" >&2
  exit 1
fi

echo "OK: $count files matched webpinfo -bitstream_info (key/profile/display/part0/w/h/scales)"
