#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/common.sh"

cd "$ROOT_DIR"

have_magick=0
have_sips=0
if command -v magick >/dev/null 2>&1; then
    have_magick=1
elif command -v sips >/dev/null 2>&1; then
    have_sips=1
else
    die "Neither ImageMagick 'magick' nor macOS 'sips' found (needed for resize/format conversion)"
fi

skip_build=${SKIP_BUILD:-0}
if [ "$skip_build" -eq 0 ]; then
	make -s all enc_png2ppm enc_quality_metrics >/dev/null
fi

sizes_str=${SIZES:-"256"}
qs_str=${QS:-"60"}
mode=${MODE:-"bpred-rdo"}
ours_flags=${OURS_FLAGS:-"--loopfilter"}

if [ "$#" -lt 1 ]; then
	echo "Usage: $0 <img1.{jpg,png}> [img2 ...]" >&2
	echo "Env overrides:" >&2
		echo "  SKIP_BUILD=1         (skip the initial make; useful for parallel sweeps)" >&2
	echo "  SIZES=\"256\"          (resize max dimension)" >&2
	echo "  QS=\"40 60 80\"        (quality sweep)" >&2
	echo "  MODE=bpred-rdo|bpred   (our encoder mode)" >&2
	echo "  OURS_FLAGS=\"...\"      (extra flags for ./encoder, e.g. --loopfilter)" >&2
	exit 2
fi

tmpdir=$(mk_artifact_tmpdir)
results_tsv="$tmpdir/results.tsv"

{
	echo -e "image\tsize\tmode\tq\tbytes\tpsnr_rgb\tssim_y"

	for src in "$@"; do
		if [ ! -f "$src" ]; then
			die "missing input: $src"
		fi

		src_base=$(basename "$src")
		src_stem=${src_base%.*}

		for s in $sizes_str; do
			derived_png="$tmpdir/${src_stem}_${s}.png"
			ref_ppm="$tmpdir/${src_stem}_${s}.ref.ppm"

			if [ "$have_magick" -eq 1 ]; then
				magick "$src" -auto-orient -resize "${s}x${s}>" -strip "$derived_png"
			else
				sips -Z "$s" -s format png "$src" --out "$derived_png" >/dev/null
			fi

			./build/enc_png2ppm "$derived_png" "$ref_ppm" >/dev/null

			for q in $qs_str; do
				ours_webp="$tmpdir/${src_stem}_${s}_${mode}_q${q}.webp"
				ours_ppm="$tmpdir/${src_stem}_${s}_${mode}_q${q}.ppm"

				./encoder --q "$q" --mode "$mode" $ours_flags "$derived_png" "$ours_webp" >/dev/null
				./decoder -ppm "$ours_webp" "$ours_ppm" >/dev/null

				ours_bytes=$(wc -c < "$ours_webp" | tr -d ' ')
				ours_metrics=$(./build/enc_quality_metrics "$ref_ppm" "$ours_ppm")
				ours_psnr=$(printf '%s\n' "$ours_metrics" | tr ' ' '\n' | awk -F= '$1=="psnr_rgb"{print $2}')
				ours_ssim=$(printf '%s\n' "$ours_metrics" | tr ' ' '\n' | awk -F= '$1=="ssim_y"{print $2}')

				echo -e "$src_base\t${s}\t${mode}\t${q}\t${ours_bytes}\t${ours_psnr}\t${ours_ssim}"
			done
		done
	done
} > "$results_tsv"

python3 - "$results_tsv" <<'PY'
import math
import sys

path = sys.argv[1]

rows = []
with open(path, 'r', encoding='utf-8') as f:
    hdr = f.readline().rstrip('\n').split('\t')
    idx = {k:i for i,k in enumerate(hdr)}
    for line in f:
        parts = line.rstrip('\n').split('\t')
        rows.append({
            'image': parts[idx['image']],
            'size': int(parts[idx['size']]),
            'mode': parts[idx['mode']],
            'q': int(parts[idx['q']]),
            'bytes': int(parts[idx['bytes']]),
            'psnr': float(parts[idx['psnr_rgb']]),
            'ssim': float(parts[idx['ssim_y']]),
        })

if not rows:
    sys.exit(0)

# Simple aggregate: mean PSNR/SSIM and mean bytes.
mean_psnr = sum(r['psnr'] for r in rows) / len(rows)
mean_ssim = sum(r['ssim'] for r in rows) / len(rows)
mean_bytes = sum(r['bytes'] for r in rows) / len(rows)

print(f"Artifacts: {path}")
print(f"Overall (mean): PSNR_RGB={mean_psnr:.3f}  SSIM_Y={mean_ssim:.6f}  bytes={mean_bytes:.1f}")
PY

cat "$results_tsv"
