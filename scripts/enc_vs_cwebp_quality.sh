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

sizes_str=${SIZES:-"256 512"}
qs_str=${QS:-"30 40 50 60 70 80"}
mode=${MODE:-bpred}
ours_flags=${OURS_FLAGS:-""}
jobs=${JOBS:-1}

if [ "$#" -lt 1 ]; then
	echo "Usage: $0 <img1.{jpg,png}> [img2 ...]" >&2
	echo "Env overrides:" >&2
	echo "  SIZES=\"256 512\"   (resize max dimension)" >&2
	echo "  QS=\"30 40 ...\"     (quality sweep)" >&2
	echo "  MODE=bpred|i16|dc    (our encoder mode)" >&2
        echo "  OURS_FLAGS=\"...\"     (extra flags for ./encoder, e.g. --loopfilter)" >&2
    echo "  JOBS=1              (parallel jobs; >1 enables multicore)" >&2
	exit 2
fi

tmpdir=$(mk_artifact_tmpdir)
results_tsv="$tmpdir/results.tsv"

process_one_image_size() {
    local src=$1
    local s=$2

    if [ ! -f "$src" ]; then
        die "missing input: $src"
    fi

    local src_base
    src_base=$(basename "$src")
    local src_stem=${src_base%.*}

    # Unique job id to avoid filename collisions across parallel jobs.
    local id
    id=$(printf '%s|%s' "$src" "$s" | shasum -a 256 | awk '{print $1}')

    local derived_png="$tmpdir/${src_stem}_${s}_${id}.png"
    local ref_ppm="$tmpdir/${src_stem}_${s}_${id}.ref.ppm"

    # Preserve aspect ratio; constrain max dimension to s.
    if [ "$have_magick" -eq 1 ]; then
        magick "$src" -auto-orient -resize "${s}x${s}>" -strip "$derived_png"
    else
        sips -Z "$s" -s format png "$src" --out "$derived_png" >/dev/null
    fi
    ./build/enc_png2ppm "$derived_png" "$ref_ppm" >/dev/null

    for q in $qs_str; do
        local ours_webp="$tmpdir/${src_stem}_${s}_${id}_ours_q${q}.webp"
        local ours_ppm="$tmpdir/${src_stem}_${s}_${id}_ours_q${q}.ppm"
        local lib_webp="$tmpdir/${src_stem}_${s}_${id}_lib_q${q}.webp"
        local lib_ppm="$tmpdir/${src_stem}_${s}_${id}_lib_q${q}.ppm"

        # OURS_FLAGS can be used to enable experimental knobs (e.g. --loopfilter).
        ./encoder --q "$q" --mode "$mode" $ours_flags "$derived_png" "$ours_webp" >/dev/null
        "$CWEBP" -quiet -q "$q" "$derived_png" -o "$lib_webp" >/dev/null 2>&1

        local ours_bytes
        local lib_bytes
        ours_bytes=$(wc -c < "$ours_webp" | tr -d ' ')
        lib_bytes=$(wc -c < "$lib_webp" | tr -d ' ')

        "$DWEBP" -quiet "$ours_webp" -ppm -o "$ours_ppm" >/dev/null 2>&1
        "$DWEBP" -quiet "$lib_webp" -ppm -o "$lib_ppm" >/dev/null 2>&1

        local ours_metrics
        local lib_metrics
        ours_metrics=$(./build/enc_quality_metrics "$ref_ppm" "$ours_ppm")
        lib_metrics=$(./build/enc_quality_metrics "$ref_ppm" "$lib_ppm")

        local ours_psnr
        local ours_ssim
        local lib_psnr
        local lib_ssim
        ours_psnr=$(printf '%s\n' "$ours_metrics" | tr ' ' '\n' | awk -F= '$1=="psnr_rgb"{print $2}')
        ours_ssim=$(printf '%s\n' "$ours_metrics" | tr ' ' '\n' | awk -F= '$1=="ssim_y"{print $2}')
        lib_psnr=$(printf '%s\n' "$lib_metrics" | tr ' ' '\n' | awk -F= '$1=="psnr_rgb"{print $2}')
        lib_ssim=$(printf '%s\n' "$lib_metrics" | tr ' ' '\n' | awk -F= '$1=="ssim_y"{print $2}')

        echo -e "$src_base\t${s}\tours\t${q}\t${ours_bytes}\t${ours_psnr}\t${ours_ssim}"
        echo -e "$src_base\t${s}\tlib\t${q}\t${lib_bytes}\t${lib_psnr}\t${lib_ssim}"
    done
}

{
	echo -e "image\tsize\tencoder\tq\tbytes\tpsnr_rgb\tssim_y"

    if [ "$jobs" -le 1 ]; then
        for src in "$@"; do
            for s in $sizes_str; do
                process_one_image_size "$src" "$s"
            done
        done
    else
        # Parallel mode: split work by (image,size) job and write per-job TSV fragments.
        rows_dir="$tmpdir/rows"
        mkdir -p "$rows_dir"

        export tmpdir qs_str mode ours_flags have_magick have_sips CWEBP DWEBP rows_dir
        export -f process_one_image_size die

        # Build NUL-separated arg list: src\0size\0src\0size\0...
        {
            for src in "$@"; do
                for s in $sizes_str; do
                    printf '%s\0%s\0' "$src" "$s"
                done
            done
        } | xargs -0 -n2 -P "$jobs" bash -c '
            src="$1"; s="$2";
            id=$(printf "%s|%s" "$src" "$s" | shasum -a 256 | awk "{print \$1}");
            out="$rows_dir/rows_${id}.tsv";
            process_one_image_size "$src" "$s" >"$out"
        ' _

        # Merge results.
        cat "$rows_dir"/rows_*.tsv
    fi
} > "$results_tsv"

python3 - "$results_tsv" <<'PY'
import math
import sys

path = sys.argv[1]

def f(x):
    try:
        return float(x)
    except ValueError:
        if x == "inf":
            return math.inf
        if x == "nan":
            return math.nan
        raise

PSNR_CAP = 99.0

def clamp_psnr(x: float) -> float:
    # PSNR can be "inf" for perfect reconstructions (common on synthetic/solid inputs).
    # Using inf directly can yield NaN when we compute (inf - inf). Clamp to a high,
    # representative ceiling so deltas remain well-defined.
    if math.isinf(x):
        return PSNR_CAP
    if math.isnan(x):
        return math.nan
    return min(x, PSNR_CAP)

rows = []
with open(path, "r", encoding="utf-8") as fp:
    header = fp.readline().rstrip("\n").split("\t")
    idx = {k:i for i,k in enumerate(header)}
    for line in fp:
        parts = line.rstrip("\n").split("\t")
        rows.append({
            "image": parts[idx["image"]],
            "size": int(parts[idx["size"]]),
            "encoder": parts[idx["encoder"]],
            "q": int(parts[idx["q"]]),
            "bytes": int(parts[idx["bytes"]]),
            "psnr": f(parts[idx["psnr_rgb"]]),
            "ssim": f(parts[idx["ssim_y"]]),
        })

from collections import defaultdict
g = defaultdict(list)
for r in rows:
    g[(r["image"], r["size"])].append(r)

def closest_by(items, key, target):
    best = None
    best_d = None
    for it in items:
        d = abs(it[key] - target)
        if best is None or d < best_d:
            best = it
            best_d = d
    return best

def summarize_pairwise(group_rows):
    ours = [r for r in group_rows if r["encoder"] == "ours"]
    lib = [r for r in group_rows if r["encoder"] == "lib"]
    ours.sort(key=lambda r: r["bytes"])
    lib.sort(key=lambda r: r["bytes"])

    # Size-matched: for each ours point, pick closest lib size.
    size_pairs = []
    for o in ours:
        l = closest_by(lib, "bytes", o["bytes"])
        size_pairs.append((o, l))

    # Quality-matched: for each ours point, pick closest lib SSIM.
    qual_pairs = []
    for o in ours:
        l = closest_by(lib, "ssim", o["ssim"])
        qual_pairs.append((o, l))

    def avg(xs):
        return sum(xs) / len(xs) if xs else 0.0

    d_psnr = [clamp_psnr(o["psnr"]) - clamp_psnr(l["psnr"]) for o,l in size_pairs]
    d_ssim = [o["ssim"] - l["ssim"] for o,l in size_pairs]
    ratio_bytes = [o["bytes"] / l["bytes"] for o,l in qual_pairs]

    return {
        "n": len(ours),
        "avg_d_psnr_at_size": avg(d_psnr),
        "avg_d_ssim_at_size": avg(d_ssim),
        "avg_bytes_ratio_at_ssim": avg(ratio_bytes),
    }

print("\n== Summary (ours - lib) ==")
print("Size-matched: positive Δ means ours better quality")
print("SSIM-matched: bytes ratio < 1 means ours smaller")

all_dpsnr = []
all_dssim = []
all_ratio = []

for (image, size), group_rows in sorted(g.items()):
    s = summarize_pairwise(group_rows)
    print(f"- {image} @{size}px: ΔPSNR={s['avg_d_psnr_at_size']:+.3f} dB  ΔSSIM={s['avg_d_ssim_at_size']:+.5f}  bytes_ratio@SSIM={s['avg_bytes_ratio_at_ssim']:.3f}")
    all_dpsnr.append(s["avg_d_psnr_at_size"])
    all_dssim.append(s["avg_d_ssim_at_size"])
    all_ratio.append(s["avg_bytes_ratio_at_ssim"])

if all_dpsnr:
    print(
        f"\nOverall: ΔPSNR={sum(all_dpsnr)/len(all_dpsnr):+.3f} dB  "
        f"ΔSSIM={sum(all_dssim)/len(all_dssim):+.5f}  "
        f"bytes_ratio@SSIM={sum(all_ratio)/len(all_ratio):.3f}"
    )

print(f"\nArtifacts: {path}")
PY

echo "OK: wrote $results_tsv" >&2
