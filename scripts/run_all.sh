#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

# Fail fast if core binaries aren't built.
./scripts/smoke_binaries.sh >/dev/null

# Oracle tools are required for most gates.
./scripts/smoke_oracle_tools.sh >/dev/null

echo "== Decoder gates =="
./scripts/m1_compare_info_with_webpinfo.sh
./scripts/m1_verify_png_out_matches_dwebp.sh
./scripts/m2_compare_vp8hdr_with_webpinfo.sh
./scripts/m3_compare_framehdr_basic_with_webpinfo.sh
./scripts/m4_compare_all_partitions_with_webpinfo.sh
./scripts/m5_coeff_hash_smoke.sh
./scripts/m5_compare_decode_ok_with_dwebp.sh
./scripts/m6_compare_yuv_with_dwebp.sh
./scripts/m7_compare_yuv_filtered_with_oracle.sh
./scripts/m8_compare_ppm_with_dwebp.sh
./scripts/m8_compare_png_with_ppm.sh

echo

echo "== Encoder gates =="
./scripts/enc_m00_png_check.sh
./scripts/enc_m01_webp_container_smoke.sh
./scripts/enc_m02_bool_selftest.sh
./scripts/enc_m03_miniframe_check.sh
./scripts/enc_m04_miniframe_check.sh
./scripts/enc_m05_yuv_check.sh
./scripts/enc_m06_intra_check.sh
./scripts/enc_m07_quant_check.sh
./scripts/enc_m08_tokens_check.sh
./scripts/enc_m09_dcenc_check.sh
./scripts/enc_m09_modeenc_check.sh
./scripts/enc_m09_bpredenc_check.sh
./scripts/enc_m10_loopfilter_check.sh
./scripts/enc_ultra_parity_check.sh
./scripts/enc_quality_check.sh

echo

echo "OK: all gates passed"
