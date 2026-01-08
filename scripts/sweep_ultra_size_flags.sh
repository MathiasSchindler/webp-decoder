#!/usr/bin/env bash
set -euo pipefail

# Sweep GCC flags for nolibc_ultra to reduce binary size without changing output.
#
# Default oracle: images/commons/penguin-q80.webp -> /tmp/penguin_sweep.png
# Compares SHA-256 against baseline build.
#
# Usage:
#   scripts/sweep_ultra_size_flags.sh
#   WEBP=images/commons/penguin-q20.webp scripts/sweep_ultra_size_flags.sh

WEBP=${WEBP:-images/commons/penguin-q80.webp}
OUTPNG=${OUTPNG:-/tmp/penguin_sweep.png}

if [[ ! -f "$WEBP" ]]; then
  echo "error: WEBP not found: $WEBP" >&2
  exit 2
fi

need() {
  command -v "$1" >/dev/null 2>&1 || { echo "error: missing required tool: $1" >&2; exit 2; }
}
need sha256sum
need stat

build_and_hash() {
  local extra=${1:-}
  rm -rf build/nolibc_ultra decoder_nolibc_ultra "$OUTPNG" || true
  if ! make -s nolibc_ultra ULTRA_EXTRA_CFLAGS="$extra" >/dev/null; then
    echo "BUILD_FAIL"
    return 0
  fi
  if ! ./decoder_nolibc_ultra "$WEBP" "$OUTPNG" >/dev/null 2>&1; then
    echo "RUN_FAIL"
    return 0
  fi
  local h
  h=$(sha256sum "$OUTPNG" | awk '{print $1}')
  local sz
  sz=$(stat -c '%s' decoder_nolibc_ultra)
  echo "$sz $h"
}

baseline=$(build_and_hash "")
if [[ "$baseline" == BUILD_FAIL* || "$baseline" == RUN_FAIL* ]]; then
  echo "error: baseline build/run failed: $baseline" >&2
  exit 2
fi
base_sz=${baseline%% *}
base_hash=${baseline#* }

printf "baseline_bytes=%s baseline_sha256=%s\n" "$base_sz" "$base_hash" >&2

# Candidate flags: only things that should not affect numerical output, just code size.
# (Some may be unsupported on your GCC; those will be skipped.)
CANDIDATES=(
  "-fno-ipa-cp"
  "-fno-ipa-sra"
  "-fno-ipa-ra"
  "-fno-ipa-modref"
  "-fno-tree-slp-vectorize"
  "-fno-tree-vectorize"
  "-fno-guess-branch-probability"
  "-fno-code-hoisting"
  "-fno-schedule-insns -fno-schedule-insns2"
  "-fno-tree-dominator-opts"
  "-fno-tree-loop-distribute-patterns"
)

best_flags=""
best_sz=$base_sz

# Greedy hill-climb: try adding each candidate, keep it if it shrinks and keeps hash.
changed=1
while [[ $changed -eq 1 ]]; do
  changed=0
  for cand in "${CANDIDATES[@]}"; do
    # Skip if already included.
    if [[ " $best_flags " == *" $cand "* ]]; then
      continue
    fi

    trial="$best_flags $cand"
    trial=${trial# }

    res=$(build_and_hash "$trial")
    if [[ "$res" == BUILD_FAIL* ]]; then
      printf "skip (build fail): %s\n" "$cand" >&2
      continue
    fi
    if [[ "$res" == RUN_FAIL* ]]; then
      printf "skip (run fail): %s\n" "$cand" >&2
      continue
    fi

    t_sz=${res%% *}
    t_hash=${res#* }

    if [[ "$t_hash" != "$base_hash" ]]; then
      printf "skip (hash mismatch): %s\n" "$cand" >&2
      continue
    fi

    if (( t_sz < best_sz )); then
      printf "keep: %s  (%s -> %s bytes)\n" "$cand" "$best_sz" "$t_sz" >&2
      best_sz=$t_sz
      best_flags="$trial"
      changed=1
    else
      printf "no gain: %s  (%s bytes)\n" "$cand" "$t_sz" >&2
    fi
  done
done

echo "" >&2
echo "BEST_BYTES=$best_sz" >&2
echo "BEST_FLAGS=$best_flags" >&2

# Also print a one-line machine-readable summary to stdout.
printf "%s,%s\n" "$best_sz" "${best_flags//,/ }"
