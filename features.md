# Missing basic encoder features (lossy-only) — implementation plan

Date: 2026-01-09

This document proposes the next **basic functionality milestones** for the lossy VP8 (keyframe-only) encoder. We explicitly **stay lossy** (no VP8L).

Guiding constraints

- C11, no external libraries.
- Deterministic output (the repo’s core workflow is manifest-gated regressions).
- Prefer incremental “milestone” modules and `scripts/enc_mXX_*.sh` gates.
- Primary correctness oracle remains: `webpinfo`, `dwebp`, and our decoder.

Non-goals (for now)

- Inter frames / animation.
- VP8L lossless mode.
- Bit-identical output to libwebp’s `cwebp`.

---

## F1 — Alpha handling (without encoding alpha)

Problem

- Input PNG can be RGBA. Today, alpha is effectively ignored in the lossy pipeline.
- This produces surprising results for transparent pixels.

Goal

- Provide **explicit, deterministic alpha policy** in the encoder CLI.

Proposed behavior

- Default: **composite RGBA over an opaque background** before RGB→YUV.
- New CLI flags (examples):
  - `--bg <r,g,b>` or `--bg <#RRGGBB>` (default: `#000000` or `#FFFFFF`, pick one and document)
  - `--alpha error` to fail if input has non-opaque alpha
  - `--alpha ignore` (current behavior) still available for debugging

Implementation steps

1. Extend PNG loader API to expose alpha presence cheaply (it already loads RGBA).
2. Add a small compositing pass in the encoder front-end:
   - If RGBA: `rgb = (a*src + (255-a)*bg) / 255` per channel, integer math.
   - Keep fully deterministic rounding.
3. Add CLI parsing and usage text.

Testing

- Add synthetic images under `images/png-in/` (tiny, stable):
  - fully opaque RGBA
  - fully transparent RGBA
  - mixed alpha edges
- Add a new gate:
  - `scripts/enc_m11_alpha_policy_manifest.sh`: encode these inputs with each policy and emit:
    - `webpinfo -bitstream_info` summary
    - decoded PNG hash via `./decoder -png` (or PPM hash) so the *visible* result is pinned.
  - `scripts/enc_m11_alpha_policy_check.sh`: compares to expected.

---

## F2 — Segmentation (quality deltas) (maps to planenc M11)

Problem

- Currently: no segmentation. Every macroblock uses the same qindex.
- Segmentation is a core VP8 feature and a large lever for improving quality for the same average q.

Goal

- Implement segmentation headers + per-macroblock segment id.
- Start with a minimal deterministic strategy; refine later.

Phase 1: bitstream support (no heuristics)

- Implement VP8 segmentation header fields:
  - `segmentation_enabled = 1`
  - `update_mb_segmentation_map = 1`
  - `update_segment_feature_data = 1`
  - `segment_feature_mode = 0` (deltas)
  - deltas for quant (and later filter) for up to 4 segments
- Emit segmentation map (segment id per macroblock) in the uncompressed header as required.

Phase 2: a simple deterministic map

- Example baseline mapping (deterministic):
  - Segment 0: default q
  - Segment 1: q - 10 for “high-detail” blocks
  - Segment 2: q + 10 for “flat” blocks
  - Segment 3: unused (0)
- Classify blocks via a cheap metric over the source luma (or residual energy).

Testing

- `scripts/enc_m11_seg_manifest.sh`:
  - encode a small set of images with `--segmentation` enabled
  - extract:
    - `webpinfo -bitstream_info` segmentation flags + q deltas
    - deterministic hash of the segmentation map bytes (or a textual dump)
- `scripts/enc_m11_seg_check.sh` compares to expected.
- Cross-check:
  - decode via `dwebp` and our `decoder` to ensure it remains valid.

---

## F3 — Multiple token partitions (maps to planenc M12)

Problem

- Today: token partitions = 1.
- Multiple partitions are important for spec coverage and are a prerequisite for some encoder tuning.

Goal

- Support `num_token_partitions` > 0 (2/4/8 partitions) and correct partition size headers.

Implementation steps

1. Define an API in token writer to “open partition”, append tokens, close partition.
2. Split token stream deterministically:
   - simplest: round-robin macroblocks across partitions
   - or by row groups
3. Emit partition sizes in the VP8 header and ensure bool encoders flush correctly.

Testing

- Add `scripts/enc_m12_partitions_manifest.sh`:
  - encode a small corpus with `--partitions 1/2/4/8`
  - verify with `webpinfo -bitstream_info` that `Total partitions` matches
  - hash the output WebPs
- Add a decode roundtrip gate (our decoder + `dwebp`) for each partition count.

---

## F4 — Probability updates (token + mode) (basic quality lever)

Problem

- Static/default probabilities are valid but leave quality on the table.

Goal

- Implement deterministic probability adaptation and write the prob update flags + values.

Implementation steps

1. Gather symbol histograms during tokenization.
2. Implement VP8 probability calculation rules (as per RFC/libwebp behavior).
3. Emit update flags and prob values.

Testing

- Add a manifest gate that pins:
  - number of updated probabilities
  - a hash of the serialized probability table
  - `webpinfo` sanity checks

---

## F5 — Smarter mode decision (RD-ish) (primary quality lever)

Problem

- Current mode selection is SAD-driven and deterministic.
- cwebp’s advantage is largely better RD optimization and tuning.

Goal

- Keep determinism but add a simple RD objective.

Implementation sketch

- For each candidate mode:
  - estimate distortion: SSE between reconstructed block and source
  - estimate rate: number of non-zero coeffs + token cost proxy
  - choose minimal `J = D + λR`, with λ derived deterministically from qindex

Testing

- Use a **quality metric gate** rather than exact bitstream equivalence:
  - For a small fixed corpus, require PSNR to not regress beyond a small epsilon.
  - Store a pinned manifest (PSNR per image) under `scripts/enc_mXX_metrics_expected.txt`.

---

## Rollout strategy

- Land features in small PR-sized steps.
- Each feature adds a dedicated `scripts/enc_mXX_*` gate.
- When correctness changes outputs, update expected manifests deliberately and keep determinism afterward.
