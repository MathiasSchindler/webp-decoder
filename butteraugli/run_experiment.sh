#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
EXP_DIR="${ROOT_DIR}/butteraugli"
OUT_DIR="${EXP_DIR}/out"
WEBP_DIR="${OUT_DIR}/webp"
PNG_DIR="${OUT_DIR}/png"
LOG_DIR="${OUT_DIR}/logs"
RES_DIR="${OUT_DIR}/results"

usage() {
  cat <<'EOF'
Usage: butteraugli/run_experiment.sh [options]

Options:
  --input <path>         Input image (jpg/png). Default: images/05924.jpg
  --max-dim <N>          Downscale so max(width,height) <= N. Default: 512
  --qualities <csv|A..B>  Quality list (comma-separated) or inclusive range A..B. Default: 0..100
  --our-modes <csv>      Our encoder modes. Default: bpred-rdo
  --skip-cwebp           Skip libwebp cwebp encodes.
  --cwebp <path>         Path to cwebp binary (overrides LIBWEBP_BIN_DIR).
  --fallback-dwebp       If our decoder fails, try libwebp dwebp for decoding.
  -h|--help              Show help.

Env vars:
  LIBWEBP_BIN_DIR        Directory containing cwebp/dwebp (e.g. ~/libwebp/examples)
  CWEBP_ARGS             Extra args appended to cwebp invocation
EOF
}

INPUT_REL="images/05924.jpg"
MAX_DIM=512
QUALITIES_CSV="0..100"
OUR_MODES_CSV="bpred-rdo"
SKIP_CWEBP=0
CWEBP_OVERRIDE=""
FALLBACK_DWEBP=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input) INPUT_REL="$2"; shift 2;;
    --max-dim) MAX_DIM="$2"; shift 2;;
    --qualities) QUALITIES_CSV="$2"; shift 2;;
    --our-modes) OUR_MODES_CSV="$2"; shift 2;;
    --skip-cwebp) SKIP_CWEBP=1; shift 1;;
    --cwebp) CWEBP_OVERRIDE="$2"; shift 2;;
    --fallback-dwebp) FALLBACK_DWEBP=1; shift 1;;
    -h|--help) usage; exit 0;;
    *) echo "error: unknown arg: $1" >&2; usage >&2; exit 2;;
  esac
done

mkdir -p "$WEBP_DIR" "$PNG_DIR" "$LOG_DIR" "$RES_DIR"

ENCODER_BIN="${ROOT_DIR}/encoder"
DECODER_BIN="${ROOT_DIR}/decoder"
BA_BIN="${ROOT_DIR}/butteraugli_nolibc_png"

for bin in "$ENCODER_BIN" "$DECODER_BIN" "$BA_BIN"; do
  if [[ ! -x "$bin" ]]; then
    echo "error: missing executable: $bin" >&2
    echo "hint: run 'make' in repo root" >&2
    exit 2
  fi
done

INPUT_PATH="$INPUT_REL"
if [[ "$INPUT_PATH" != /* ]]; then
  INPUT_PATH="${ROOT_DIR}/${INPUT_PATH}"
fi
if [[ ! -f "$INPUT_PATH" ]]; then
  echo "error: input not found: $INPUT_PATH" >&2
  exit 2
fi

# Resolve cwebp/dwebp
CWEBP_BIN=""
DWEBP_BIN=""
if [[ -n "$CWEBP_OVERRIDE" ]]; then
  CWEBP_BIN="$CWEBP_OVERRIDE"
elif [[ -n "${LIBWEBP_BIN_DIR:-}" ]]; then
  CWEBP_BIN="${LIBWEBP_BIN_DIR}/cwebp"
  DWEBP_BIN="${LIBWEBP_BIN_DIR}/dwebp"
else
  CWEBP_BIN="$(command -v cwebp || true)"
  DWEBP_BIN="$(command -v dwebp || true)"
  if [[ -z "$CWEBP_BIN" ]]; then
    CWEBP_BIN="${HOME}/libwebp/examples/cwebp"
    DWEBP_BIN="${HOME}/libwebp/examples/dwebp"
  fi
fi

if [[ $SKIP_CWEBP -eq 0 ]]; then
  if [[ ! -x "$CWEBP_BIN" ]]; then
    echo "warn: cwebp not found/executable ($CWEBP_BIN); skipping cwebp" >&2
    SKIP_CWEBP=1
  fi
fi

if [[ $FALLBACK_DWEBP -eq 1 ]]; then
  if [[ ! -x "$DWEBP_BIN" ]]; then
    echo "error: --fallback-dwebp requested but dwebp not found/executable ($DWEBP_BIN)" >&2
    exit 2
  fi
fi

REF_PNG="${OUT_DIR}/reference.png"

# Convert input -> reference PNG (optionally downscale)
# Use ImageMagick (magick/convert) if available, else ffmpeg.
if command -v magick >/dev/null 2>&1; then
  magick "$INPUT_PATH" -auto-orient -resize "${MAX_DIM}x${MAX_DIM}>" -strip -colorspace sRGB "PNG24:${REF_PNG}"
elif command -v convert >/dev/null 2>&1; then
  convert "$INPUT_PATH" -auto-orient -resize "${MAX_DIM}x${MAX_DIM}>" -strip -colorspace sRGB "PNG24:${REF_PNG}"
elif command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -y -hide_banner -loglevel error -i "$INPUT_PATH" -vf "scale='min(${MAX_DIM},iw)':'min(${MAX_DIM},ih)':force_original_aspect_ratio=decrease" "$REF_PNG"
else
  echo "error: need one of: magick|convert|ffmpeg for image conversion" >&2
  exit 2
fi

RESULTS_CSV="${RES_DIR}/results.csv"
: >"$RESULTS_CSV"
echo "encoder,quality,mode,webp_path,webp_bytes,decoded_png_path,decoded_png_bytes,butteraugli" >>"$RESULTS_CSV"

qual_list_from_spec() {
  local spec="$1"
  if [[ "$spec" =~ ^[0-9]+\.\.[0-9]+$ ]]; then
    local a b
    a="${spec%%..*}"
    b="${spec##*..}"
    seq "$a" "$b"
    return 0
  fi
  # comma-separated list
  echo "$spec" | tr ',' '\n'
}

score_pair() {
  local encoder_name="$1"
  local quality="$2"
  local mode="$3"
  local webp_path="$4"
  local png_path="$5"

  local webp_bytes decoded_bytes score_raw score
  webp_bytes="$(stat -c %s "$webp_path")"
  decoded_bytes="$(stat -c %s "$png_path")"

  score_raw="$($BA_BIN "$REF_PNG" "$png_path" 2>>"${LOG_DIR}/butteraugli.err" || true)"
  # Tool prints a single float; be defensive and extract the first float-ish token.
  score="$(printf '%s\n' "$score_raw" | awk 'match($0, /[0-9]+(\.[0-9]+)?/){print substr($0, RSTART, RLENGTH); exit}')"
  if [[ -z "$score" ]]; then
    score="nan"
  fi

  printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$encoder_name" "$quality" "$mode" \
    "$webp_path" "$webp_bytes" "$png_path" "$decoded_bytes" "$score" \
    >>"$RESULTS_CSV"
}

decode_webp_to_png() {
  local webp_path="$1"
  local png_path="$2"

  if "$DECODER_BIN" -png "$webp_path" "$png_path" >"${LOG_DIR}/decode.stdout" 2>>"${LOG_DIR}/decode.stderr"; then
    return 0
  fi

  if [[ $FALLBACK_DWEBP -eq 1 ]]; then
    "$DWEBP_BIN" "$webp_path" -o "$png_path" -quiet >>"${LOG_DIR}/decode.stdout" 2>>"${LOG_DIR}/decode.stderr"
    return 0
  fi

  return 1
}

# Our encoder sweep
mapfile -t QUALITIES < <(qual_list_from_spec "$QUALITIES_CSV")
IFS=',' read -r -a OUR_MODES <<<"$OUR_MODES_CSV"

total_jobs=$(( ${#QUALITIES[@]} * ${#OUR_MODES[@]} ))
if [[ $SKIP_CWEBP -eq 0 ]]; then
  total_jobs=$(( total_jobs + ${#QUALITIES[@]} ))
fi
job_i=0
progress() {
  job_i=$((job_i + 1))
  printf '[%d/%d] %s\n' "$job_i" "$total_jobs" "$1" >&2
}

for q in "${QUALITIES[@]}"; do
  for mode in "${OUR_MODES[@]}"; do
    tag="our_q${q}_mode_${mode}"
    out_webp="${WEBP_DIR}/${tag}.webp"
    out_png="${PNG_DIR}/${tag}.png"

    progress "our: q=${q} mode=${mode}"
    "$ENCODER_BIN" --q "$q" --mode "$mode" "$REF_PNG" "$out_webp" >"${LOG_DIR}/${tag}.enc.stdout" 2>"${LOG_DIR}/${tag}.enc.stderr"

    if decode_webp_to_png "$out_webp" "$out_png"; then
      score_pair "our" "$q" "$mode" "$out_webp" "$out_png"
    else
      echo "warn: decode failed for $out_webp (our decoder)." >&2
    fi
  done
done

# cwebp sweep
if [[ $SKIP_CWEBP -eq 0 ]]; then
  for q in "${QUALITIES[@]}"; do
    tag="cwebp_q${q}"
    out_webp="${WEBP_DIR}/${tag}.webp"
    out_png="${PNG_DIR}/${tag}.png"

    progress "cwebp: q=${q}"
    # Keep container minimal (avoid metadata). Allow user to append tuning via CWEBP_ARGS.
    "$CWEBP_BIN" -q "$q" -metadata none ${CWEBP_ARGS:-} "$REF_PNG" -o "$out_webp" >"${LOG_DIR}/${tag}.enc.stdout" 2>"${LOG_DIR}/${tag}.enc.stderr"

    if decode_webp_to_png "$out_webp" "$out_png"; then
      score_pair "cwebp" "$q" "-" "$out_webp" "$out_png"
    else
      echo "warn: decode failed for $out_webp (try --fallback-dwebp)." >&2
    fi
  done
fi

# Sort by score (numeric), best first.
SORTED_CSV="${RES_DIR}/results.sorted.csv"
{
  head -n 1 "$RESULTS_CSV"
  tail -n +2 "$RESULTS_CSV" |
    awk -F, 'BEGIN{OFS=","} { s=$8; if (s=="nan" || s=="") s=1e9; print $1,$2,$3,$4,$5,$6,$7,$8,s }' |
    sort -t, -k9,9g |
    cut -d, -f1-8
} >"$SORTED_CSV"

SUMMARY_TXT="${RES_DIR}/summary.txt"
{
  echo "reference_png=${REF_PNG}"
  echo "qualities=${QUALITIES_CSV}"
  echo "our_modes=${OUR_MODES_CSV}"
  if [[ $SKIP_CWEBP -eq 0 ]]; then
    echo "cwebp=${CWEBP_BIN}"
  else
    echo "cwebp=SKIPPED"
  fi
  echo
  echo "Top results (best butteraugli first):"
  head -n 11 "$SORTED_CSV" | column -t -s, || true
} >"$SUMMARY_TXT"

# Plot SVG
PLOT_SVG="${RES_DIR}/plot.svg"
if command -v python3 >/dev/null 2>&1; then
  python3 "${EXP_DIR}/plot_results.py" --input "$RESULTS_CSV" --output "$PLOT_SVG" --title "Butteraugli vs WebP size" || true
else
  echo "warn: python3 not found; skipping SVG plot" >&2
fi

echo "Wrote: $SORTED_CSV"
echo "Wrote: $SUMMARY_TXT"
if [[ -f "$PLOT_SVG" ]]; then
  echo "Wrote: $PLOT_SVG"
fi
