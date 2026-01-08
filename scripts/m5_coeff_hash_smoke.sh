#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

DECODER=./decoder

if [[ ! -x "$DECODER" ]]; then
  echo "error: $DECODER not found; run 'make' first" >&2
  exit 1
fi

count=0

shopt -s nullglob
files=(images/webp/*.webp images/testimages/webp/*.webp)

if (( ${#files[@]} == 0 )); then
  echo "error: no .webp files found under images/webp or images/testimages/webp" >&2
  exit 1
fi

for f in "${files[@]}"; do
  line="$($DECODER -info "$f" | grep -E '^  Coeff hash:       [0-9]+' || true)"
  if [[ -z "$line" ]]; then
    echo "FAIL: missing coeff hash for $f" >&2
    exit 1
  fi
  count=$((count + 1))
done

echo "OK: decoded coeff hash for $count files"
