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

for f in images/webp/*.webp; do
  [ -e "$f" ] || continue
  count=$((count+1))

  mine_cs=$($DECODER -info "$f" | awk -F': *' '/^  Color space:/{print $2}')
  mine_ct=$($DECODER -info "$f" | awk -F': *' '/^  Clamp type:/{print $2}')
  mine_seg=$($DECODER -info "$f" | awk -F': *' '/^  Use segment:/{print $2}')
  mine_sf=$($DECODER -info "$f" | awk -F': *' '/^  Simple filter:/{print $2}')
  mine_lvl=$($DECODER -info "$f" | awk -F': *' '/^  Level:/{print $2}')
  mine_sh=$($DECODER -info "$f" | awk -F': *' '/^  Sharpness:/{print $2}')
  mine_lfd=$($DECODER -info "$f" | awk -F': *' '/^  Use lf delta:/{print $2}')
  mine_tp=$($DECODER -info "$f" | awk -F': *' '/^  Total partitions:/{print $2}')
  mine_bq=$($DECODER -info "$f" | awk -F': *' '/^  Base Q:/{print $2}')
  mine_y1=$($DECODER -info "$f" | awk -F': *' '/^  DQ Y1 DC:/{print $2}')
  mine_y2dc=$($DECODER -info "$f" | awk -F': *' '/^  DQ Y2 DC:/{print $2}')
  mine_y2ac=$($DECODER -info "$f" | awk -F': *' '/^  DQ Y2 AC:/{print $2}')
  mine_uvdc=$($DECODER -info "$f" | awk -F': *' '/^  DQ UV DC:/{print $2}')
  mine_uvac=$($DECODER -info "$f" | awk -F': *' '/^  DQ UV AC:/{print $2}')

  oracle=$($WEBPINFO -bitstream_info "$f")
  o_cs=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Color space:/{print $2}')
  o_ct=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Clamp type:/{print $2}')
  o_seg=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Use segment:/{print $2}')
  o_sf=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Simple filter:/{print $2}')
  o_lvl=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Level:/{print $2}')
  o_sh=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Sharpness:/{print $2}')
  o_lfd=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Use lf delta:/{print $2}')
  o_tp=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Total partitions:/{print $2}')
  o_bq=$(printf "%s\n" "$oracle" | awk -F': *' '/^  Base Q:/{print $2}')
  o_y1=$(printf "%s\n" "$oracle" | awk -F': *' '/^  DQ Y1 DC:/{print $2}')
  o_y2dc=$(printf "%s\n" "$oracle" | awk -F': *' '/^  DQ Y2 DC:/{print $2}')
  o_y2ac=$(printf "%s\n" "$oracle" | awk -F': *' '/^  DQ Y2 AC:/{print $2}')
  o_uvdc=$(printf "%s\n" "$oracle" | awk -F': *' '/^  DQ UV DC:/{print $2}')
  o_uvac=$(printf "%s\n" "$oracle" | awk -F': *' '/^  DQ UV AC:/{print $2}')

  ok=1
  [ "$mine_cs" = "$o_cs" ] || ok=0
  [ "$mine_ct" = "$o_ct" ] || ok=0
  [ "$mine_seg" = "$o_seg" ] || ok=0
  [ "$mine_sf" = "$o_sf" ] || ok=0
  [ "$mine_lvl" = "$o_lvl" ] || ok=0
  [ "$mine_sh" = "$o_sh" ] || ok=0
  [ "$mine_lfd" = "$o_lfd" ] || ok=0
  [ "$mine_tp" = "$o_tp" ] || ok=0
  [ "$mine_bq" = "$o_bq" ] || ok=0
  [ "$mine_y1" = "$o_y1" ] || ok=0
  [ "$mine_y2dc" = "$o_y2dc" ] || ok=0
  [ "$mine_y2ac" = "$o_y2ac" ] || ok=0
  [ "$mine_uvdc" = "$o_uvdc" ] || ok=0
  [ "$mine_uvac" = "$o_uvac" ] || ok=0

  if [ "$ok" -ne 1 ]; then
    echo "MISMATCH: $f" >&2
    echo "  Color space mine=$mine_cs oracle=$o_cs" >&2
    echo "  Clamp type  mine=$mine_ct oracle=$o_ct" >&2
    echo "  Use segment mine=$mine_seg oracle=$o_seg" >&2
    echo "  Simple filt mine=$mine_sf oracle=$o_sf" >&2
    echo "  Level       mine=$mine_lvl oracle=$o_lvl" >&2
    echo "  Sharpness   mine=$mine_sh oracle=$o_sh" >&2
    echo "  Use lf dlt  mine=$mine_lfd oracle=$o_lfd" >&2
    echo "  Partitions  mine=$mine_tp oracle=$o_tp" >&2
    echo "  Base Q      mine=$mine_bq oracle=$o_bq" >&2
    echo "  DQ Y1 DC    mine=$mine_y1 oracle=$o_y1" >&2
    echo "  DQ Y2 DC    mine=$mine_y2dc oracle=$o_y2dc" >&2
    echo "  DQ Y2 AC    mine=$mine_y2ac oracle=$o_y2ac" >&2
    echo "  DQ UV DC    mine=$mine_uvdc oracle=$o_uvdc" >&2
    echo "  DQ UV AC    mine=$mine_uvac oracle=$o_uvac" >&2
    fail=1
  fi

done

if [ "$count" -eq 0 ]; then
  echo "error: no files matched images/webp/*.webp" >&2
  exit 2
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: basic frame header fields mismatched webpinfo -bitstream_info" >&2
  exit 1
fi

echo "OK: $count files matched webpinfo -bitstream_info (color/clamp/seg/filter/partitions/quant deltas)"
