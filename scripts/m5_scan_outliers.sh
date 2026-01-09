#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ROOT_DIR=$(pwd)
ARTIFACT_DIR="$ROOT_DIR/build/test-artifacts/m5_scan_outliers"
mkdir -p "$ARTIFACT_DIR"

DECODER=./decoder

if [[ ! -x "$DECODER" ]]; then
  echo "error: $DECODER not found; run 'make' first" >&2
  exit 2
fi

shopt -s nullglob
files=(images/webp/*.webp images/testimages/webp/*.webp)
if compgen -G "images/generated/webp/*.webp" > /dev/null; then
  files+=(images/generated/webp/*.webp)
fi

if (( ${#files[@]} == 0 )); then
  echo "error: no .webp files found under images/webp or images/testimages/webp" >&2
  exit 2
fi

tmp=$(mktemp "$ARTIFACT_DIR/tmp.XXXXXX")
trap 'rm -f "$tmp"' EXIT

# Emit: file<TAB>p0_used<TAB>p0_size<TAB>tok_used<TAB>tok_size<TAB>p0_over<TAB>tok_over<TAB>p0_over_b<TAB>tok_over_b<TAB>absmax<TAB>nonzero
for f in "${files[@]}"; do
  out="$($DECODER -info "$f" 2>/dev/null || true)"
  if [[ -z "$out" ]]; then
    echo "FAIL: decoder produced no output for $f" >&2
    exit 1
  fi

  p0_line=$(printf "%s\n" "$out" | grep -E '^  Part0 bytes used: ' | tail -n 1 || true)
  tok_line=$(printf "%s\n" "$out" | grep -E '^  Token bytes used: ' | tail -n 1 || true)
  p0o_line=$(printf "%s\n" "$out" | grep -E '^  Part0 overread: ' | tail -n 1 || true)
  toko_line=$(printf "%s\n" "$out" | grep -E '^  Token overread: ' | tail -n 1 || true)
  p0ob_line=$(printf "%s\n" "$out" | grep -E '^  Part0 overread b: ' | tail -n 1 || true)
  tokob_line=$(printf "%s\n" "$out" | grep -E '^  Token overread b: ' | tail -n 1 || true)
  abs_line=$(printf "%s\n" "$out" | grep -E '^  Coeff abs max: ' | tail -n 1 || true)
  nz_line=$(printf "%s\n" "$out" | grep -E '^  Coeff nonzero: ' | tail -n 1 || true)

  if [[ -z "$p0_line" || -z "$tok_line" || -z "$p0o_line" || -z "$toko_line" || -z "$p0ob_line" || -z "$tokob_line" || -z "$abs_line" || -z "$nz_line" ]]; then
    echo "FAIL: missing expected M5 stats for $f" >&2
    exit 1
  fi

  p0_used=$(echo "$p0_line" | awk -F'[: /]+' '{print $5}')
  p0_size=$(echo "$p0_line" | awk -F'[: /]+' '{print $6}')
  tok_used=$(echo "$tok_line" | awk -F'[: /]+' '{print $5}')
  tok_size=$(echo "$tok_line" | awk -F'[: /]+' '{print $6}')
  p0_over=$(echo "$p0o_line" | awk -F': *' '{print $2}')
  tok_over=$(echo "$toko_line" | awk -F': *' '{print $2}')
  p0_over_b=$(echo "$p0ob_line" | awk -F': *' '{print $2}')
  tok_over_b=$(echo "$tokob_line" | awk -F': *' '{print $2}')
  absmax=$(echo "$abs_line" | awk -F': *' '{print $2}')
  nonzero=$(echo "$nz_line" | awk -F': *' '{print $2}')

  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$f" "$p0_used" "$p0_size" "$tok_used" "$tok_size" "$p0_over" "$tok_over" "$p0_over_b" "$tok_over_b" "$absmax" "$nonzero" >> "$tmp"
done

count=$(wc -l < "$tmp" | tr -d ' ')
echo "OK: scanned $count files"

echo

echo "Tight padding (slack <= 2 bytes):"
awk -F'\t' '{p0s=$3-$2; toks=$5-$4; if (p0s<=2 || toks<=2) {printf("  %s  part0_slack=%d token_slack=%d\n", $1, p0s, toks)}}' "$tmp" | sort || true

echo

echo "Bool-decoder overread (refill past end):"
awk -F'\t' '{
  p0s=$3-$2;
  toks=$5-$4;
  if ($6=="Yes" || $7=="Yes") {
    printf("  %s  part0=%s (%sB, slack=%d) token=%s (%sB, slack=%d)\n", $1, $6, $8, p0s, $7, $9, toks)
  }
}' "$tmp" | sort || true

echo

echo "Top 10 by Coeff abs max:"
sort -t$'\t' -k10,10nr "$tmp" | head -n 10 | awk -F'\t' '{printf("  %s  abs_max=%s nonzero=%s\n", $1, $10, $11)}'

echo

echo "Top 10 by Coeff nonzero:"
sort -t$'\t' -k11,11nr "$tmp" | head -n 10 | awk -F'\t' '{printf("  %s  nonzero=%s abs_max=%s\n", $1, $11, $10)}'
