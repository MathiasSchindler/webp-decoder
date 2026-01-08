# scripts/

Milestone helper scripts used to validate our implementation against the libwebp oracle tools and the repo’s golden outputs.

Conventions:

- Script names start with `m<N>_` where `<N>` is the milestone number from [plan.md](../plan.md).
- Scripts should be runnable from the repo root: `/home/mathias/webp-stuff/decoder`.
- Scripts intentionally avoid extra dependencies; they use POSIX-ish shell tools (`sh`, `awk`, `sed`, `grep`, `sha256sum`).

## Milestone 1 (container parsing)

- `m1_compare_info_with_webpinfo.sh`
  - Runs `./decoder -info` and `../../libwebp/examples/webpinfo` over all `images/webp/*.webp`.
  - Compares:
    - RIFF total file size
    - `VP8 ` chunk offset and length (in `webpinfo` convention)

- `m1_verify_png_out_matches_dwebp.sh`
  - Validates the repo’s `images/png-out/*.png` are exactly what the oracle decoder produces.
  - For each `images/webp/*.webp`, decodes to a temp PNG via `dwebp` and compares `sha256sum` against the matching file in `images/png-out/`.

## Milestone 2 (VP8 key-frame header)

- `m2_compare_vp8hdr_with_webpinfo.sh`
  - Compares the VP8 key-frame header fields printed by `./decoder -info` against `webpinfo -bitstream_info` for all `images/webp/*.webp`.
  - Checks: Key frame / Profile / Display / Part. 0 length / Width+Height + X/Y scale.

## Milestone 3 (VP8 basic frame header)

- `m3_compare_framehdr_basic_with_webpinfo.sh`
  - Compares additional VP8 frame header fields printed by `./decoder -info` against `webpinfo -bitstream_info`.
  - Checks: Color space, Clamp type, segmentation enabled, loop-filter basics, partition count, base Q and quant deltas.

## Milestone 4 (VP8 partition size table)

- `m4_compare_all_partitions_with_webpinfo.sh`
  - Compares all `Part. <i> length:` lines printed by `./decoder -info` against `webpinfo -bitstream_info`.
  - This becomes meaningful once we have files with `Total partitions > 1`.

- `m4_scan_total_partitions.sh`
  - Scans both `images/webp/*.webp` and `images/testimages/webp/*.webp` and reports whether any files have `Total partitions > 1`.

## Milestone 5 (macroblock syntax + coefficient tokens)

- `m5_coeff_hash_smoke.sh`
  - Runs `./decoder -info` over both corpora and asserts we print a numeric `Coeff hash` line for every file.
  - This is a smoke test to ensure macroblock parsing + token decoding stays bounded and deterministic.

- `m5_compare_decode_ok_with_dwebp.sh`
  - Ensures both our decoder (`./decoder -info`) and the oracle (`dwebp`) successfully decode every file in both corpora.
  - This is a basic behavioral match against `dwebp` for “does it decode?” at the Milestone 5 (non-pixel) stage.

- `m5_scan_outliers.sh`
  - Scans both corpora and prints potentially interesting outliers:
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
  - Runs `./decoder -yuv` over the corpora and compares the raw I420 output against `dwebp -yuv -nofilter`.
  - Uses `-nofilter` so the oracle output is pre-loopfilter (we implement the in-loop filter in Milestone 7).
