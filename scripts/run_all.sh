#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

cores() {
	local n
	n=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
	if [[ -n "${n:-}" ]]; then
		echo "$n"
		return 0
	fi
	n=$(sysctl -n hw.ncpu 2>/dev/null || true)
	if [[ -n "${n:-}" ]]; then
		echo "$n"
		return 0
	fi
	echo 4
}

JOBS=${TEST_JOBS:-$(cores)}
if [[ "$JOBS" -lt 1 ]]; then JOBS=1; fi

LOG_DIR="build/test-artifacts/_run_all_logs"
mkdir -p "$LOG_DIR"

run_gates_parallel() {
	local title="$1"; shift
	local -a gates=("$@")

	echo "== $title =="

	printf '%s\0' "${gates[@]}" | xargs -0 -n 1 -P "$JOBS" -I {} bash -c '
		set -euo pipefail
		gate="$1"
		name=$(basename "$gate" .sh)
		log="$2/$name.log"
		if ! "$gate" >"$log" 2>&1; then
			echo "FAIL: $gate" >&2
			cat "$log" >&2 || true
			exit 1
		fi
	' _ "{}" "$LOG_DIR"
}

# Fail fast if core binaries aren't built.
./scripts/smoke_binaries.sh >/dev/null

# Oracle tools are required for most gates.
./scripts/smoke_oracle_tools.sh >/dev/null

echo "== Decoder gates =="
run_gates_parallel "Decoder gates" \
	./scripts/m1_compare_info_with_webpinfo.sh \
	./scripts/m1_verify_png_out_matches_dwebp.sh \
	./scripts/m2_compare_vp8hdr_with_webpinfo.sh \
	./scripts/m3_compare_framehdr_basic_with_webpinfo.sh \
	./scripts/m4_compare_all_partitions_with_webpinfo.sh \
	./scripts/m5_coeff_hash_smoke.sh \
	./scripts/m5_compare_decode_ok_with_dwebp.sh \
	./scripts/m6_compare_yuv_with_dwebp.sh \
	./scripts/m7_compare_yuv_filtered_with_oracle.sh \
	./scripts/m8_compare_ppm_with_dwebp.sh \
	./scripts/m8_compare_png_with_ppm.sh

echo

run_gates_parallel "Encoder gates" \
	./scripts/enc_m00_png_check.sh \
	./scripts/enc_m01_webp_container_smoke.sh \
	./scripts/enc_m02_bool_selftest.sh \
	./scripts/enc_m03_miniframe_check.sh \
	./scripts/enc_m04_miniframe_check.sh \
	./scripts/enc_m05_yuv_check.sh \
	./scripts/enc_m06_intra_check.sh \
	./scripts/enc_m07_quant_check.sh \
	./scripts/enc_m08_tokens_check.sh \
	./scripts/enc_m09_dcenc_check.sh \
	./scripts/enc_m09_modeenc_check.sh \
	./scripts/enc_m09_bpredenc_check.sh \
	./scripts/enc_m10_loopfilter_check.sh \
	./scripts/enc_ultra_parity_check.sh \
	./scripts/enc_quality_check.sh

echo

echo "OK: all gates passed"
