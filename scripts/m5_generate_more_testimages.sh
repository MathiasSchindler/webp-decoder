#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ROOT_DIR=$(pwd)
. "$ROOT_DIR/scripts/common.sh"
require_libwebp_cwebp
require_libwebp_dwebp
require_libwebp_webpinfo

:

mkdir -p images/generated/ppm images/generated/webp

# Build the tiny PPM generator.
cc -std=c11 -O2 -Wall -Wextra -Wpedantic tools/gen_ppm.c -o build/gen_ppm

# Generate a small, diverse set of PPMs.
# We focus on sizes that change macroblock layout and edge prediction behavior.
patterns=(solid rgbgrad checker noise diag)
sizes=(
  "16 16" "17 17" "31 31" "32 32" "33 33"
  "63 63" "64 64" "65 65"
  "127 127" "128 128" "129 129"
  "400 416" "416 384" "384 416"
)

for pat in "${patterns[@]}"; do
  i=0
  for sz in "${sizes[@]}"; do
    read -r w h <<<"$sz"
    for q in 10 50 90 100; do
      name="gen_${pat}_${w}x${h}_q${q}"
      ppm="images/generated/ppm/${name}.ppm"
      webp="images/generated/webp/${name}.webp"

      # Use a deterministic seed per file.
      seed=$(( (w * 1000003 + h * 9176 + q * 101 + i * 7) & 0xffffffff ))

      ./build/gen_ppm "$pat" "$w" "$h" "$ppm" "$seed"

      # Keyframe-only, no alpha.
      "$CWEBP" -quiet -q "$q" "$ppm" -o "$webp"

      # Sanity: oracle can decode it.
      "$DWEBP" "$webp" -o /dev/null >/dev/null

      i=$((i+1))
    done
  done

done

echo "OK: generated $(ls -1 images/generated/webp/*.webp | wc -l) WebPs under images/generated/webp/"

echo

echo "Tip: run 'scripts/m5_scan_outliers.sh' after including these in your scans."
