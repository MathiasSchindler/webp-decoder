#!/usr/bin/env bash
set -euo pipefail

# Build a minimal source release archive.
#
# Includes:
# - All sources required to build decoder/encoder (normal + ultra)
# - SSIM/PSNR tooling (enc_png2ppm + enc_quality_metrics) and the harness script
#
# Excludes:
# - All *.md files
# - All test images (images/)
# - All scripts except the SSIM/PSNR harness and its shared helpers

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

OUT_DIR=${OUT_DIR:-"$ROOT_DIR/dist"}
mkdir -p "$OUT_DIR"

DATE_UTC=$(date -u +%Y%m%d)
GIT_REV=nogit
if command -v git >/dev/null 2>&1 && git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	GIT_REV=$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo nogit)
fi

PKG_NAME="webp-decoder-release-${DATE_UTC}-${GIT_REV}"
STAGE_DIR=$(mktemp -d)
trap 'rm -rf "$STAGE_DIR"' EXIT

DEST="$STAGE_DIR/$PKG_NAME"
mkdir -p "$DEST"

cat >"$DEST/RELEASE_README.txt" <<EOF
webp-decoder source release

Included:
- src/: decoder + encoder sources (normal + ultra)
- Makefile
- tools/enc_png2ppm.c and tools/enc_quality_metrics.c (PSNR/SSIM toolchain)
- scripts/common.sh and scripts/enc_vs_cwebp_quality.sh (PSNR/SSIM harness)

Excluded:
- All *.md files
- images/ (all corpora / test images)
- Most scripts/ (gates, benchmarks, etc.)

Build:
  make all ultra

Build SSIM/PSNR tooling:
  make enc_png2ppm enc_quality_metrics

Run SSIM/PSNR harness (requires libwebp cwebp/dwebp available; see scripts/common.sh):
  LIBWEBP_BIN_DIR=/path/to/libwebp/examples ./scripts/enc_vs_cwebp_quality.sh path/to/image.jpg
EOF

# Selection policy:
# - include src/** unconditionally
# - include tools/enc_{png2ppm,quality_metrics}.c and src/quality/**
# - include scripts/{common.sh,enc_vs_cwebp_quality.sh}
# - include Makefile, LICENSE, and RFC txt files
# - exclude *.md and images/**

allow_script() {
	case "$1" in
		scripts/common.sh|scripts/enc_vs_cwebp_quality.sh) return 0 ;;
		*) return 1 ;;
	esac
}

allow_tool() {
	case "$1" in
		tools/enc_png2ppm.c|tools/enc_quality_metrics.c) return 0 ;;
		*) return 1 ;;
	esac
}

should_include() {
	local f="$1"

	case "$f" in
		*.md) return 1 ;;
		images/*) return 1 ;;
		build/*) return 1 ;;
	esac

	case "$f" in
		Makefile|LICENSE|rfc*.txt) return 0 ;;
		src/*) return 0 ;;
		tools/*)
			allow_tool "$f" && return 0
			# src/quality/** is already covered via src/**.
			return 1
			;;
		scripts/*)
			allow_script "$f" && return 0
			return 1
			;;
		*)
			return 1
			;;
	esac
}

if ! command -v git >/dev/null 2>&1 || ! git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	echo "error: this script expects to run inside a git checkout (uses git ls-files)" >&2
	exit 2
fi

# Copy selected tracked files.
while IFS= read -r -d '' f; do
	if should_include "$f"; then
		mkdir -p "$DEST/$(dirname -- "$f")"
		cp -p "$ROOT_DIR/$f" "$DEST/$f"
	fi
done < <(git -C "$ROOT_DIR" ls-files -z)

# Create archive.
ARCHIVE_PATH="$OUT_DIR/${PKG_NAME}.tar.gz"
tar -C "$STAGE_DIR" -czf "$ARCHIVE_PATH" "$PKG_NAME"

# Helpful output.
echo "OK: wrote $ARCHIVE_PATH" >&2
