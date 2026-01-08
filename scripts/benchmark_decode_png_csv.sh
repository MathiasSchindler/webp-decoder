#!/usr/bin/env bash
set -euo pipefail

# Benchmarks WebP->PNG decode time across all .webp files under ./images.
# Produces a CSV with one row per file:
#   file,dwebp_us,decoder_nolibc_ultra_us
#
# Usage:
#   scripts/benchmark_decode_png_csv.sh out.csv
#
# Env vars:
#   DWEBP   Path to dwebp (default: "$HOME/libwebp/examples/dwebp")
#   ULTRA   Path to decoder_nolibc_ultra (default: "./decoder_nolibc_ultra")
#   RUNS    Runs per decoder per file; script records the minimum (default: 3)
#
# Notes:
# - Uses Python perf_counter_ns for microsecond precision when available.
# - Falls back to date +%s%N if python3 is unavailable.

OUT_CSV=${1:-bench_decode_png.csv}
DWEBP=${DWEBP:-"$HOME/libwebp/examples/dwebp"}
ULTRA=${ULTRA:-"./decoder_nolibc_ultra"}
RUNS=${RUNS:-3}

PYTHON=${PYTHON:-python3}
HAVE_PY=0
if command -v "$PYTHON" >/dev/null 2>&1; then
  HAVE_PY=1
fi
if [[ ! -x "$DWEBP" ]]; then
  echo "error: dwebp not found/executable: $DWEBP" >&2
  exit 2
fi
if [[ ! -x "$ULTRA" ]]; then
  echo "error: decoder_nolibc_ultra not found/executable: $ULTRA" >&2
  echo "hint: run: make nolibc_ultra" >&2
  exit 2
fi
if [[ ! -d images ]]; then
  echo "error: ./images directory not found (run from repo root)" >&2
  exit 2
fi

csv_escape() {
  # Escape double-quotes for RFC4180-ish CSV and wrap in quotes.
  local s=$1
  s=${s//\"/\"\"}
  printf '"%s"' "$s"
}

# Run a command RUNS times and return the minimum wall time in seconds.
# Returns microseconds as an integer.
# Prints "ERR" on failure.
run_min_us() {
  local tmpdir=$1
  shift

  local best=""
  local i
  for ((i=1; i<=RUNS; i++)); do
    local t_us=""
    if [[ $HAVE_PY -eq 1 ]]; then
      if ! t_us=$("$PYTHON" - "$@" <<'PY'
import subprocess
import sys
import time

cmd = sys.argv[1:]
t0 = time.perf_counter_ns()
r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
t1 = time.perf_counter_ns()
if r.returncode != 0:
    sys.exit(r.returncode)
dt_us = (t1 - t0) // 1000
print(dt_us)
PY
      ); then
        echo "ERR"
        return 0
      fi
    else
      # Fallback: nanosecond timestamps. (Less accurate than perf_counter_ns, but higher resolution than /usr/bin/time.)
      local t0 t1
      t0=$(date +%s%N)
      if ! "$@" >/dev/null 2>&1; then
        echo "ERR"
        return 0
      fi
      t1=$(date +%s%N)
      t_us=$(( (t1 - t0) / 1000 ))
    fi

    if [[ -z "$best" ]]; then
      best=$t_us
    else
      if (( t_us < best )); then
        best=$t_us
      fi
    fi
  done

  echo "$best"
}

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

{
  echo "file,dwebp_us,decoder_nolibc_ultra_us"

  i=0
  while IFS= read -r -d '' f; do
    i=$((i+1))

    # Use unique output paths to avoid accidental reuse.
    out_d="$tmpdir/dwebp_$i.png"
    out_u="$tmpdir/ultra_$i.png"

    # dwebp: output format inferred by extension.
    t_d=$(run_min_us "$tmpdir" "$DWEBP" "$f" -o "$out_d" -quiet)
    t_u=$(run_min_us "$tmpdir" "$ULTRA" "$f" "$out_u")

    printf "%s,%s,%s\n" "$(csv_escape "$f")" "$t_d" "$t_u"
  done < <(find images -type f -name '*.webp' -print0 | sort -z)
} > "$OUT_CSV"

echo "wrote $OUT_CSV" >&2
