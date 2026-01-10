#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

cd "$ROOT_DIR"

require_libwebp_cwebp
require_libwebp_dwebp

have_magick=0
have_sips=0
if command -v magick >/dev/null 2>&1; then
	have_magick=1
elif command -v sips >/dev/null 2>&1; then
	have_sips=1
else
	die "Neither ImageMagick 'magick' nor macOS 'sips' found (needed for resize/format conversion)"
fi

make -s all enc_png2ppm enc_quality_metrics >/dev/null

size=${SIZE:-512}
mode=${MODE:-bpred-rdo}
ours_flags=${OURS_FLAGS:-""}
qmin=${QMIN:-1}
qmax=${QMAX:-100}
skip_bad_q=${SKIP_BAD_Q:-0}

case "$qmin" in (''|*[!0-9]*) die "QMIN must be an integer" ;; esac
case "$qmax" in (''|*[!0-9]*) die "QMAX must be an integer" ;; esac
if [ "$qmin" -lt 0 ] || [ "$qmin" -gt 100 ] || [ "$qmax" -lt 0 ] || [ "$qmax" -gt 100 ] || [ "$qmin" -gt "$qmax" ]; then
	die "QMIN/QMAX must satisfy 0 <= QMIN <= QMAX <= 100"
fi

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <img.{jpg,png}>" >&2
	echo "Env overrides:" >&2
	echo "  SIZE=512                 (resize max dimension)" >&2
	echo "  MODE=bpred|bpred-rdo|i16|dc  (our encoder mode)" >&2
	echo "  OURS_FLAGS=\"...\"          (extra flags for ./encoder, e.g. --loopfilter)" >&2
	echo "  QMIN=1 QMAX=100          (quality sweep range)" >&2
		echo "  SKIP_BAD_Q=0|1           (continue on decode/metrics failure; records nan)" >&2
	echo "  LIBWEBP_BIN_DIR=/opt/homebrew/bin  (or your libwebp examples dir)" >&2
	exit 2
fi

src=$1
[ -f "$src" ] || die "missing input: $src"

src_base=$(basename "$src")
src_stem=${src_base%.*}

tmpdir=$(mk_artifact_tmpdir)
results_tsv="$tmpdir/${src_stem}_${size}px_rdcurve.tsv"

derived_png="$tmpdir/${src_stem}_${size}.png"
ref_ppm="$tmpdir/${src_stem}_${size}.ref.ppm"

# Preserve aspect ratio; constrain max dimension to SIZE.
if [ "$have_magick" -eq 1 ]; then
	magick "$src" -auto-orient -resize "${size}x${size}>" -strip "$derived_png"
else
	sips -Z "$size" -s format png "$src" --out "$derived_png" >/dev/null
fi

./build/enc_png2ppm "$derived_png" "$ref_ppm" >/dev/null

{
	echo -e "image\tsize\tencoder\tq\tbytes\tpsnr_rgb\tssim_y"
	for q in $(seq "$qmin" "$qmax"); do
		ours_webp="$tmpdir/${src_stem}_${size}_ours_q${q}.webp"
		lib_webp="$tmpdir/${src_stem}_${size}_lib_q${q}.webp"

		./encoder --q "$q" --mode "$mode" $ours_flags "$derived_png" "$ours_webp" >/dev/null
		[ -s "$ours_webp" ] || die "encoder produced no output: $ours_webp"
		"$CWEBP" -quiet -q "$q" "$derived_png" -o "$lib_webp" >/dev/null 2>&1
		[ -s "$lib_webp" ] || die "cwebp produced no output: $lib_webp"

		ours_bytes=$(wc -c < "$ours_webp" | tr -d ' ')
		lib_bytes=$(wc -c < "$lib_webp" | tr -d ' ')

		# Avoid writing decoded PPMs to disk: stream dwebp output into enc_quality_metrics.
		# Using a pipe (not process substitution) means failures surface reliably via `set -o pipefail`.
		if ! ours_metrics=$(
			"$DWEBP" -quiet "$ours_webp" -ppm -o /dev/stdout 2>/dev/null | ./build/enc_quality_metrics "$ref_ppm" -
		); then
			if [ "$skip_bad_q" -eq 1 ]; then
				note "warn: decode/metrics failed for ours q=$q (mode=$mode); recording nan"
				ours_metrics="psnr_rgb=nan ssim_y=nan"
			else
				die "decode/metrics failed for ours q=$q (mode=$mode)"
			fi
		fi
		if ! lib_metrics=$(
			"$DWEBP" -quiet "$lib_webp" -ppm -o /dev/stdout 2>/dev/null | ./build/enc_quality_metrics "$ref_ppm" -
		); then
			if [ "$skip_bad_q" -eq 1 ]; then
				note "warn: decode/metrics failed for lib q=$q; recording nan"
				lib_metrics="psnr_rgb=nan ssim_y=nan"
			else
				die "decode/metrics failed for lib q=$q"
			fi
		fi

		ours_psnr=$(printf '%s\n' "$ours_metrics" | tr ' ' '\n' | awk -F= '$1=="psnr_rgb"{print $2}')
		ours_ssim=$(printf '%s\n' "$ours_metrics" | tr ' ' '\n' | awk -F= '$1=="ssim_y"{print $2}')
		lib_psnr=$(printf '%s\n' "$lib_metrics" | tr ' ' '\n' | awk -F= '$1=="psnr_rgb"{print $2}')
		lib_ssim=$(printf '%s\n' "$lib_metrics" | tr ' ' '\n' | awk -F= '$1=="ssim_y"{print $2}')

		echo -e "$src_base\t${size}\tours\t${q}\t${ours_bytes}\t${ours_psnr}\t${ours_ssim}"
		echo -e "$src_base\t${size}\tlib\t${q}\t${lib_bytes}\t${lib_psnr}\t${lib_ssim}"

		# We only need size + metrics; delete per-q encoded images.
		rm -f "$ours_webp" "$lib_webp"
	done
} >"$results_tsv"

# We only keep the numeric results + plots.
rm -f "$derived_png" "$ref_ppm"

python3 "$ROOT_DIR/scripts/plot_rdcurve_svg.py" "$results_tsv" --title "$src_base @${size}px (q=${qmin}..${qmax}, mode=$mode, flags=$ours_flags)" >/dev/null

echo "OK: wrote $results_tsv" >&2
echo "OK: wrote ${results_tsv%.tsv}_ssim.svg" >&2
echo "OK: wrote ${results_tsv%.tsv}_psnr.svg" >&2
