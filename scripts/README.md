# scripts/

Milestone helper scripts used to validate our implementation against the libwebp oracle tools and the repo’s golden outputs.

Conventions:

- Script names start with `m<N>_` where `<N>` is the milestone number from [plandec.md](../plandec.md).
- Scripts should be runnable from the repo root: `/home/mathias/webp-stuff/decoder`.
- Scripts intentionally avoid extra dependencies; they use POSIX-ish shell tools (`sh`, `awk`, `sed`, `grep`, `sha256sum`).
- Temporary outputs should go under `build/test-artifacts/<script-name>/` and be cleaned up on success.

## Running everything

Most scripts rely on libwebp’s oracle tools (`webpinfo`, `dwebp`, sometimes `cwebp`).

You can point scripts at your libwebp build via:

- `LIBWEBP_BIN_DIR=/path/to/libwebp/examples`

To run the full decoder+encoder gate suite in a stable order:

```sh
make test
```

This invokes `scripts/run_all.sh`.

The current decoder WebP corpus used by the later byte-exact gates is:
`images/webp/*.webp`, `images/testimages/webp/*.webp`,
`images/generated/webp/*.webp`, `images/commons/*.webp`,
`images/commons/generated-webp/*.webp`, and `images/examples/*.webp`.

For an auditable snapshot of decoder feature/quality parity against libwebp,
including the local corpus inventory and unsupported-feature matrix, run:

```sh
scripts/decoder_quality_parity_report.py
```

## Profiling

- `profile_decode_stages.py`
  - Profiles our decoder modes over the generated Commons WebP corpus when present, otherwise `images/**/*.webp`.
  - Records cumulative stages (`-info`, `-yuv`, `-yuvf`, `-ppm`, `-png`), optional internal `-profile_stages` timers, derived loopfilter/RGB/output deltas, system-libwebp RGB/YUV core timings, direct core comparison rows, tool versions, command templates, corpus metadata, caveats, and CSV timings.
  - Also benchmarks whole-pipeline PPM output against the same generated system `libwebp` API helper, `ffmpeg`, and ImageMagick when available.
  - Copies the decoder under test into the artifact directory by default so a concurrent rebuild cannot change the profiled binary mid-run.
  - Run via `make profile-decode-stages` or directly with `python3 scripts/profile_decode_stages.py --runs 5`.
  - Current local Commons profile artifact: `build/profile/opt2-integrated-comparison-20260612T1420/` (28 generated Commons WebPs, 5 runs). Use its CSVs for stage/core deltas; do not treat local MP/s as portable. Current medians: ours `-ppm` 167.50 MP/s and `-png` 146.15 MP/s; system-libwebp RGB+PPM helper is 215.64 MP/s. Remaining costs are mainly core entropy, reconstruction, and loopfilter rather than RGB formatting or PPM writes.

- `benchmark_quality_size_matrix.py`
  - Generates a WebP corpus from `images/commons/*.jpg` across configurable quality values and target megapixel buckets, then benchmarks our `-ppm` output against a generated system-libwebp RGB+PPM helper.
  - Writes `quality_size_per_file.csv`, aggregated `quality_size_matrix.csv`, `quality_size_heatmap.svg/html`, `quality_size_summary.md/html`, and by-quality/by-size summary CSVs.
  - Pass `--baseline-matrix path/to/quality_size_matrix.csv` to add `quality_size_regression_delta.csv` and a red/green `quality_size_regression_heatmap.svg/html` showing current-vs-baseline changes in our advantage and MP/s.
  - Example: `scripts/benchmark_quality_size_matrix.py --out-dir build/profile/quality-size-matrix --qualities 0,10,20,30,40,50,60,70,80,90,100 --megapixels 0.25,0.5,1,2,4,8,16,32`.

- `profile_micro.py`
  - Runs the decoder's opt-in `-profile_micro` mode over the generated Commons/q80 stress inputs and combines the per-file CSV rows.
  - Captures entropy/token sub-timers, coefficient/block classes, reconstruction/prediction/IDCT classes, loopfilter edge categories, and PPM output sub-timers.
  - Example: `scripts/profile_micro.py --out-dir build/profile/microprofile --runs 1 --max-files 8`.

## Milestone 1 (container parsing)

- `m1_compare_info_with_webpinfo.sh`
  - Runs `./build/decoder -info` and `../../libwebp/examples/webpinfo` over all `images/webp/*.webp`.
  - Compares:
    - RIFF total file size
    - `VP8 ` chunk offset and length (in `webpinfo` convention)

- `m1_verify_png_out_matches_dwebp.sh`
  - Validates the repo’s `images/png-out/*.png` are exactly what the oracle decoder produces.
  - For each `images/webp/*.webp`, decodes to a temp PNG via `dwebp` and compares `sha256sum` against the matching file in `images/png-out/`.

## Milestone 2 (VP8 key-frame header)

- `m2_compare_vp8hdr_with_webpinfo.sh`
  - Compares the VP8 key-frame header fields printed by `./build/decoder -info` against `webpinfo -bitstream_info` for all `images/webp/*.webp`.
  - Checks: Key frame / Profile / Display / Part. 0 length / Width+Height + X/Y scale.

## Milestone 3 (VP8 basic frame header)

- `m3_compare_framehdr_basic_with_webpinfo.sh`
  - Compares additional VP8 frame header fields printed by `./build/decoder -info` against `webpinfo -bitstream_info`.
  - Checks: Color space, Clamp type, segmentation enabled, loop-filter basics, partition count, base Q and quant deltas.

## Milestone 4 (VP8 partition size table)

- `m4_compare_all_partitions_with_webpinfo.sh`
  - Compares all `Part. <i> length:` lines printed by `./build/decoder -info` against `webpinfo -bitstream_info`.
  - This becomes meaningful once we have files with `Total partitions > 1`.

- `m4_scan_total_partitions.sh`
  - Scans the decoder WebP corpus and reports whether any files have `Total partitions > 1`.

## Milestone 5 (macroblock syntax + coefficient tokens)

- `m5_coeff_hash_smoke.sh`
  - Runs `./build/decoder -info` over the decoder WebP corpus and asserts we print a numeric `Coeff hash` line for every file.
  - This is a smoke test to ensure macroblock parsing + token decoding stays bounded and deterministic.

- `m5_compare_decode_ok_with_dwebp.sh`
  - Ensures both our decoder (`./build/decoder -info`) and the oracle (`dwebp`) successfully decode every file in the decoder WebP corpus.
  - This is a basic behavioral match against `dwebp` for “does it decode?” at the Milestone 5 (non-pixel) stage.

- `m5_scan_outliers.sh`
  - Scans the decoder WebP corpus and prints potentially interesting outliers:
    - very tight partition padding (slack <= 2 bytes)
    - top files by `Coeff abs max` and `Coeff nonzero`

- `m5_generate_more_testimages.sh`
  - Generates additional deterministic test images (PPM) and encodes them to WebP via `cwebp`.
  - Outputs:
    - `images/generated/ppm/*.ppm`
    - `images/generated/webp/*.webp`
  - Intended to broaden coverage around macroblock edges and to help investigate token-partition overread behavior.

## Milestone 6 (inverse transforms + intra prediction → YUV)

- `m6_compare_yuv_with_dwebp.sh`
  - Runs `./build/decoder -yuv` over the decoder WebP corpus and compares the raw I420 output against `dwebp -yuv -nofilter`.
  - Uses `-nofilter` so the oracle output is pre-loopfilter (we implement the in-loop filter in Milestone 7).

## Milestone 7 (in-loop filter)

- `m7_compare_yuv_filtered_with_oracle.sh`
  - Runs `./build/decoder -yuvf` over the decoder WebP corpus and compares raw I420 output against `dwebp -yuv`.

## Milestone 8 (RGB/PNG output)

- `m8_compare_ppm_with_dwebp.sh`
  - Runs `./build/decoder -ppm` over the decoder WebP corpus and compares PPM bytes against `dwebp -ppm`.

- `m8_compare_png_with_ppm.sh`
  - Runs `./build/decoder -png` over the decoder WebP corpus and validates RGB bytes against the already-validated PPM path.

---

## Encoder milestone helpers

These scripts gate the incremental encoder work described in [planenc.md](../planenc.md).

### Encoder M9 (I16 mode decisions + in-loop recon)

- `enc_m09_dcenc_check.sh`
  - Baseline: DC_PRED (Y+UV) with in-loop reconstruction.
  - Gates decoded RGB hashes for `images/png-in/*.png` across a small quality set.

- `enc_m09_modeenc_check.sh`
  - Adds per-macroblock I16 (luma) + UV (chroma) mode selection among DC/V/H/TM using SAD.
  - Gates both decoded RGB hashes and a raw mode-map hash (file contains y_modes then uv_modes).

### Encoder M9 (B_PRED 4x4 luma)

- `enc_m09_bpredenc_check.sh`
  - Encodes all macroblocks with `ymode=B_PRED` (4x4 luma intra), choosing per-subblock b_modes by SAD.
  - Chooses UV per macroblock among DC/V/H/TM by SAD.
  - Gates decoded RGB hashes and a raw mode-map hash (file contains y_modes then uv_modes then b_modes).

### Encoder quality guardrails

- `enc_quality_check.sh`
  - PSNR/SSIM regression guardrail on `images/png-in/*.png`.
  - Supports baseline refresh via `--update`.

- `enc_butteraugli_check.sh`
  - Butteraugli + output-size regression guardrail on `images/png-in/*.png` using `butteraugli_nolibc_png`.
  - Encodes with a fixed profile (`--q 75 --mode bpred-rdo --loopfilter --token-probs adaptive`).
  - Supports baseline refresh via `--update`.
