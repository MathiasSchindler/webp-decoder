# Encoder plan / status (PNG → lossy WebP VP8)

Date: 2026-01-08

This file is a *living* incremental plan for adding a **lossy WebP encoder** (VP8 key-frame) to this repository.

Design philosophy (same as the decoder work):

- One eye on the specs: `rfc9649.txt` (WebP container) + `rfc6386.txt` (VP8)
- One eye on oracle behavior: libwebp tools (`cwebp`, `dwebp`, `webpinfo`)
- **Incremental steps** with **gated tests**; no “big bang” encoder.

Important note up front: a practical encoder will not be bit-identical to libwebp unless we intentionally clone libwebp’s heuristics. Our oracle strategy is therefore:

- **Validity/conformance**: output must parse (`webpinfo`) and decode (`dwebp`, our decoder).
- **Round-trip correctness**: decoded pixels must match expectation for synthetic test vectors (where the “correct” reconstruction is unambiguous).
- **Regression-stable quality metrics**: PSNR/SSIM on a small corpus for non-trivial images.
- **Field-level comparisons** where meaningful: header fields, loopfilter flags, segmentation flags, partition sizes, etc.

---

## Initial scope (keep it narrow)

Target a first working encoder that can:

- Input: **PNG** (start with 8-bit RGB/RGBA; alpha ignored initially)
- Output: **simple lossy WebP** (`RIFF`/`WEBP` + one `VP8 ` chunk)
- Encode: **VP8 key frames only**, a single frame
- Start with:
  - no segmentation
  - no multiple token partitions (use `Total partitions: 1`)
  - simplest mode decisions (even “always DC_PRED” initially)

Out of scope for v0:

- `VP8X` / metadata chunks (`ICCP`/`EXIF`/`XMP`)
- Alpha (`ALPH`) and transparent PNG handling
- Animation
- Lossless (`VP8L`)
- Inter frames

---

## Tooling contracts (encoder CLI)

Proposed minimal CLI (can evolve):

- `./encoder -info in.png` prints PNG summary and what will be encoded
- `./encoder -q <0..100> in.png out.webp` encodes lossy VP8
- `./encoder -dump_frame in.png out.txt` dumps deterministic encoder decisions (modes, q, filter params, partition sizes)

Also useful:

- `./encoder -selftest` runs a tiny synthetic suite

(We can also fold into the existing `decoder` binary later, but keeping an `encoder` binary initially helps keep dependency surfaces small.)

---

## Repo layout (encoder-only modules)

To keep encoder work cleanly separated from the decoder, encoder code should live under its own module tree.

Proposed layout:

- `src/enc-m00_png/` — PNG input decoding (encoder input side)
- `src/enc-m01_riff/` — WebP RIFF/container writing (RFC 9649)
- `src/enc-m02_vp8_bitwriter/` — bit writer + VP8 boolean entropy encoder
- `src/enc-m03_vp8_headers/` — VP8 uncompressed/header fields (key-frame)
- `src/enc-m04_yuv/` — RGB→YUV420 + padding/cropping helpers
- `src/enc-m05_intra/` — intra prediction + forward transforms
- `src/enc-m06_quant/` — quantization tables + `-q` mapping
- `src/enc-m07_tokens/` — tokenization + entropy coding into partitions
- `src/enc-m08_filter/` — loopfilter param selection
- `src/enc-m09_seg/` — segmentation (optional)

Shared helpers can still live in `src/common/` (endian helpers, small alloc, etc.), but the intent is:

- No shared VP8-specific implementation files between encoder and decoder.
- If we reuse anything, it’s narrow primitives, not codec logic.

---

## Milestone status

| # | Milestone | Status | Primary module |
|---:|---|---|---|
| 0 | PNG reader (minimal subset) | Not started | `src/enc-m00_png/` |
| 1 | WebP container writer | Not started | `src/enc-m01_riff/` |
| 2 | VP8 bit/bool writer primitives | Not started | `src/enc-m02_vp8_bitwriter/` |
| 3 | Minimal VP8 key frame (solid 16×16) | Not started | `src/enc-m03_vp8_headers/`, `src/enc-m07_tokens/` |
| 4 | Arbitrary dimensions + padding/cropping | Not started | `src/enc-m04_yuv/` |
| 5 | RGB→YUV420 (deterministic) | Not started | `src/enc-m04_yuv/` |
| 6 | Intra predict (DC only) + forward transform | Not started | `src/enc-m05_intra/` |
| 7 | Quantization + `-q` mapping | Not started | `src/enc-m06_quant/` |
| 8 | Token encoding (non-zero coeffs) | Not started | `src/enc-m07_tokens/` |
| 9 | Mode decisions (simple SAD chooser) | Not started | `src/enc-m05_intra/` |
| 10 | Loopfilter params | Not started | `src/enc-m08_filter/` |
| 11 | Segmentation (optional) | Not started | `src/enc-m09_seg/` |
| 12 | Token partitions > 1 | Not started | `src/enc-m07_tokens/` |
| 13 | `VP8X` + metadata chunks | Not started | `src/enc-m01_riff/` |
| 14 | Alpha (`ALPH`) | Not started | (new module) |
| 15 | Lossless (`VP8L`) | Not started | (new module) |
| 16 | Hardening/fuzzing | Not started | cross-cutting |

## Test data layout (repo contract)

Use existing data folders:

- `images/png-in/` as encoder input
- `images/webp/` as *reference* outputs from libwebp (not bit-identical target)
- Add new folder(s) if needed:
  - `images/webp-enc/` for our encoder outputs
  - `images/metrics/` for PSNR/SSIM manifests

---

## Oracle tools

- `../../libwebp/examples/cwebp` (encode oracle)
- `../../libwebp/examples/dwebp` (decode oracle)
- `../../libwebp/examples/webpinfo` (container/bitstream inspection)

Additionally, use our own decoder as a cross-check:

- `./decoder -png out.webp out.png` should succeed for encoded files

---

## Milestones (incremental and test-gated)

### M0 — Scaffolding + PNG reader (`src/enc-m00_png/`)

Goal: reliably load PNG input into RGB(A) buffers.

Implement:

- Minimal PNG decoder for inputs (likely reuse existing PNG logic patterns, but this time *reading* PNG)
  - Keep it tiny: support only what we need at first
  - Recommended initial subset: PNG 8-bit RGB/RGBA, no interlace, no palette

Tests:

- For each `images/png-in/*.png`, decode to RGB and hash bytes
- Cross-check with a known-good decoder (e.g. `python3 -c` via Pillow if available, or `ffmpeg`)

Deliverable artifact:

- Deterministic `rgb.sha256` manifest for PNG inputs

### M1 — WebP container writer (RFC 9649) (`src/enc-m01_riff/`)

Goal: write a correct RIFF/WebP container with a placeholder `VP8 ` chunk.

Implement:

- RIFF header + size fields
- `WEBP` signature
- One `VP8 ` chunk with payload bytes
- Correct padding to even sizes

Tests:

- `webpinfo out.webp` parses cleanly and lists exactly one `VP8 ` chunk
- Size fields match actual file length

### M2 — VP8 bit writer + boolean entropy encoder (`src/enc-m02_vp8_bitwriter/`)

Goal: implement the VP8 bit writer + boolean entropy *encoder*.

Implement:

- A bounded bit writer
- VP8 bool encoder per RFC 6386 (inverse of the decoder’s bool decoder)

Tests:

- Unit tests that write bits and re-read using our bool decoder
- Deterministic golden outputs for tiny synthetic sequences

### M3 — Minimal VP8 bitstream that decodes (solid-color 16×16)

Goal: produce a valid VP8 key frame that decodes to a constant image.

Strategy:

- Start with a **single macroblock** (16×16) and then generalize.
- Use the simplest prediction choices and zero AC coefficients.

Primary modules: `src/enc-m03_vp8_headers/`, `src/enc-m07_tokens/`

Implement (VP8 essentials only):

- VP8 frame tag (key frame)
- Key-frame start code
- Width/height
- Minimal header fields (no segmentation, no prob updates)
- Token encoding for “all blocks EOB” (DC only, AC all zero)

Tests:

- `dwebp -ppm out.webp` succeeds
- Decode with our decoder and compare to libwebp `dwebp` output bytes
- For a synthetic solid input PNG (single RGB color), require exact pixel match after round-trip (because the reconstructed frame should be exactly constant)

Deliverable artifact:

- A tiny known-good WebP produced by our encoder whose decoded pixels are exactly known

### M4 — Generalize to arbitrary dimensions (still simple modes)

Primary module: `src/enc-m04_yuv/`

Goal: handle arbitrary width/height with macroblock padding/cropping rules.

Implement:

- Macroblock grid sizing, edge padding rules
- Write visible width/height; ensure decoded output crops correctly

Tests:

- Encode a few sizes: 1×1, 15×15, 16×16, 17×17, 31×7, etc.
- `dwebp` decode success and correct decoded dimensions

### M5 — RGB → YUV420 conversion (spec-consistent)

Primary module: `src/enc-m04_yuv/`

Goal: convert input RGB to YUV420 with stable, defined rounding.

Implement:

- Rec.601-ish RGB→YUV conversion (consistent with decoder expectations)
- Downsample chroma to 4:2:0 deterministically

Tests:

- Synthetic patterns with known averages (e.g. checkerboards) to validate chroma downsample correctness
- Compare our YUV conversion to libwebp’s decoded YUV for carefully chosen simple cases (where prediction/residual are zero)

### M6 — Intra prediction (DC only) + forward transforms

Primary module: `src/enc-m05_intra/`

Goal: for each block, compute residual = (input - predictor), transform, quantize, and entropy-code.

Implement:

- Intra predictors: start with DC_PRED only
- Forward transforms (DCT/WHT)

Tests:

- Round-trip: encode → decode (with `dwebp`) resembles input
- Very small corpus smoke, focusing on determinism and decode success

### M7 — Quantization + `-q` mapping

Primary module: `src/enc-m06_quant/`

Goal: introduce quantization without mixing in mode decisions.

Implement:

- A simple, explicit mapping from `-q` to VP8 quant indices
- Start with a single global quant (no segmentation, no deltas)

Tests:

- Monotonic behavior: lowering `-q` should not increase output size
- Add PSNR calculation and enforce a lower bound for a small curated corpus

### M8 — Token encoding for non-zero coeffs

Primary module: `src/enc-m07_tokens/`

Goal: support real images (not just “all EOB”).

Implement:

- Tokenization and entropy coding for coefficient blocks
- Still keep `Total partitions: 1`

Tests:

- Round-trip: encode → decode works across a curated subset of `images/png-in/`

### M9 — Mode decisions (still simple, deterministic)

Primary module: `src/enc-m05_intra/`

Goal: improve quality without exploding complexity.

Implement (incremental):

- Add additional luma modes (V_PRED, H_PRED, TM_PRED, then B_PRED)
- Use simple heuristics:
  - choose best mode by SAD on predictor
  - optionally include a rate proxy (estimated token cost)

Tests:

- Deterministic mode dump (`-dump_frame`) for a fixed corpus
- PSNR/SSIM improves or at least does not regress compared to “always DC_PRED” baseline

### M10 — Loop filter parameter selection

Primary module: `src/enc-m08_filter/`

Goal: set loop filter fields sensibly.

Implement:

- Basic filter settings derived from quality/quant
- Start with a simple fixed mapping, then refine

Tests:

- `webpinfo -bitstream_info` shows sane filter params
- Visual and metric checks for blocking

### M11 — Segmentation (optional but powerful)

Primary module: `src/enc-m09_seg/`

Goal: support segmentation to allocate bits where needed.

Implement:

- At first: two segments (flat vs detailed), chosen by variance threshold
- Segment-level quant deltas

Tests:

- Deterministic segmentation map dumps
- No decode failures; metric improvements on mixed-detail images

### M12 — Token partitions > 1

Goal: support multiple token partitions for performance / spec completeness.

Implement:

- Partition splitting and proper size table writing

Tests:

- Ensure `webpinfo` reports `Total partitions` > 1
- Decode success and stable output

### M13 — Container extensions and alpha (future)

- `VP8X` parsing and writing
- `ALPH` encoding for RGBA inputs (decide compression strategy)

### M16 — Robustness and hardening

Goal: make the encoder safe and deterministic.

- Validate all input sizes and avoid integer overflow
- Add fuzz-ish tests for PNG loader
- Ensure encoder never writes malformed containers on invalid input

---

## Proposed scripts (mirroring decoder workflow)

- `scripts/e0_png_decode_smoke.sh` — PNG read sanity + hashes
- `scripts/e1_webp_container_smoke.sh` — container structure checks via `webpinfo`
- `scripts/e2_encode_solid_roundtrip.sh` — exact round-trip for constant images
- `scripts/e3_encode_corpus_metrics.sh` — PSNR/SSIM manifests and regression checks

---

## Open questions (we should decide early)

These decisions shape the plan:

1) Do we want a *minimal* encoder (small, spec-correct) or a *useful* encoder (reasonable quality)?
2) Target quality interface:
   - libwebp-like `-q` mapping, or raw VP8 quant indices exposed?
3) For early milestones, should we encode only 16×16 / macroblock-aligned images first, or go straight to arbitrary dimensions?

If you answer (1) with “minimal first”, I’d start M2 with a fixed 16×16 solid-color encoder that is trivially verifiable, and only then generalize.
