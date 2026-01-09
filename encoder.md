# Encoder quality improvement plan (ours vs libwebp)

Date: 2026-01-09

Goal: systematically improve our encoder’s **rate–distortion** performance, measured by our in-repo **PSNR/SSIM** tool, and compared against **libwebp (cwebp)**.

We start from this baseline (256px, QS=40/60/80, MODE=bpred):

```
== Summary (ours - lib) ==
Size-matched: positive Δ means ours better quality
SSIM-matched: bytes ratio < 1 means ours smaller
- antpilla.jpg @256px: ΔPSNR=-4.857 dB  ΔSSIM=-0.02554  bytes_ratio@SSIM=1.588
- crane.jpg @256px:    ΔPSNR=-3.447 dB  ΔSSIM=-0.01112  bytes_ratio@SSIM=1.835
- penguin.jpg @256px:  ΔPSNR=-6.200 dB  ΔSSIM=-0.05333  bytes_ratio@SSIM=1.668

Overall: ΔPSNR=-4.835 dB  ΔSSIM=-0.02999  bytes_ratio@SSIM=1.697
```

## Principles

- Make changes **one lever at a time**.
- Always keep:
  - `make test` green (correctness + determinism + no-regression gate)
  - `encoder_nolibc_ultra` parity (already enforced)
- Measure improvements using a fixed harness and write down results.

## Tooling (what we already have)

- Regression guardrail (our own baseline):
  - `./scripts/enc_quality_check.sh` (uses `scripts/enc_quality_expected.txt`)
- Ad-hoc comparison vs libwebp:
  - `LIBWEBP_BIN_DIR=$HOME/libwebp/examples ./scripts/enc_vs_cwebp_quality.sh ...`

Recommended “standard comparison” command while iterating:

```sh
LIBWEBP_BIN_DIR="$HOME/libwebp/examples" \
  SIZES="256" \
  QS="40 60 80" \
  MODE=bpred \
  ./scripts/enc_vs_cwebp_quality.sh \
  images/commons/penguin.jpg images/commons/crane.jpg images/commons/antpilla.jpg \
  | tail -n 30
```

When you believe you’ve improved quality, run a broader sweep:

```sh
LIBWEBP_BIN_DIR="$HOME/libwebp/examples" \
  SIZES="256 512 1024" \
  QS="10 20 30 40 50 60 70 80 90" \
  MODE=bpred \
  ./scripts/enc_vs_cwebp_quality.sh images/commons/penguin.jpg images/commons/crane.jpg images/commons/antpilla.jpg
```

## Success criteria

We want to move these metrics in the right direction:

- **Same size → better quality**: ΔSSIM and ΔPSNR should approach 0 and become positive.
- **Same quality → smaller files**: `bytes_ratio@SSIM` should approach 1 and go **below 1**.

## Step-by-step implementation plan

### Step 0 — Make comparisons apples-to-apples (1 day)

1) Confirm we’re comparing the same “features”:
   - Run comparisons with our loopfilter both off and on.
   - Command:
     - `MODE=bpred` baseline (already done)
     - Add a loopfilter variant by running:
       - our side: `./encoder --loopfilter ...`
       - (if needed, extend `scripts/enc_vs_cwebp_quality.sh` to optionally pass `--loopfilter`)

Implementation touchpoints:
- Comparison harness: `scripts/enc_vs_cwebp_quality.sh` (supports `OURS_FLAGS`, e.g. `OURS_FLAGS="--loopfilter"`).
- Encoder CLI flag: `src/encoder_main.c` (`--loopfilter|--lf`).

2) Decide a standard for comparisons for the week:
   - Recommend: keep `MODE=bpred` fixed, and always enable `--loopfilter` during quality comparisons.

Deliverable:
- A documented “standard comparison command” to re-run after each change.

### Step 1 — Loop filter defaults + better parameter mapping (fast win)

Hypothesis: our images look blocky/harsh relative to libwebp; loop filtering is a large part of baseline VP8 visual quality.

1) Ensure loopfilter is easy to enable and has deterministic parameters.
   - If `--loopfilter` is optional today, keep it optional for correctness gates.
   - For *quality comparisons*, prefer always enabling it.

2) Improve qindex → filter params mapping.
   - Target: increase SSIM especially at lower bitrates.
   - Implementation approach:
     - Start with a simple mapping table keyed by qindex buckets.
     - Tune `filter_level` and `sharpness` first; keep deltas simple.

Implementation touchpoints:
- qindex→params mapping: `src/enc-m08_filter/enc_loopfilter.c` (`enc_vp8_loopfilter_from_qindex`).
- Header plumbing into VP8 stream: `src/encoder_main.c` uses `enc_vp8_build_keyframe_*_ex(..., &lf, ...)`.
- Bitstream writer consumes params here: `src/enc-m07_tokens/enc_vp8_tokens.c` (writes simple/level/sharpness/lfdelta).

3) Measure:
   - Re-run the standard comparison command and record the new summary.

4) Safety:
   - Run `make test`.
   - If output changes are expected, update *quality baseline* only when you are confident the change is an improvement:
     - `./scripts/enc_quality_check.sh --update`

Deliverable:
- Improved SSIM at same size with loopfilter enabled.

### Step 2 — Fix intra prediction selection (high impact)

Hypothesis: we choose suboptimal prediction modes, increasing residual energy and wasting bits.

1) Implement a deterministic per-macroblock mode decision using SSE.
   - For each macroblock:
     - evaluate a small set of candidate predictors
     - compute SSE of residual vs original
     - pick the best predictor
   - Start with 16x16 only (DC / V / H / TM), then refine 4x4 bpred.

2) Make sure prediction uses reconstructed neighbors (true intra).

3) Measure:
   - Standard comparison command.
   - If SSIM improves but filesize grows, proceed to Step 3 (rate term).

4) Safety:
   - Run `make test`.
   - Keep ultra parity (should stay green if you don’t use libc).

Deliverable:
- Significant improvement in ΔSSIM and ΔPSNR at size-matched points.

Implementation touchpoints:
- Encode entrypoints (in-loop recon):
   - `src/enc-m08_recon/enc_recon.c`
      - `enc_vp8_encode_dc_pred_inloop()`
      - `enc_vp8_encode_i16x16_sad_inloop()` / `enc_vp8_encode_i16x16_uv_sad_inloop()`
      - `enc_vp8_encode_bpred_uv_sad_inloop()`
- Mode selection code paths:
   - I16 mode selection loop (SAD): `enc_vp8_encode_i16x16_uv_sad_inloop()`
   - B_PRED 4x4 mode selection (SAD): `enc_vp8_encode_bpred_uv_sad_inloop()`

### Step 3 — Add a lightweight rate term (RDO-lite)

Hypothesis: pure SSE picks modes that look good but spend too many bits; we need to trade off distortion vs rate.

1) Add a cost function:

- $J = D + \lambda \cdot R$

Where:
- $D$ is SSE (or SATD if you add it later)
- $R$ is a proxy for bit-cost

2) Implement the simplest bit-cost estimate first:
   - Option A: count tokens / non-zeros as a proxy for rate
   - Option B: do a “dry-run” encode of candidate modes into a scratch bitwriter and take actual bitcount

3) Tune $\lambda$ as a function of qindex (or quality).

4) Measure:
   - Look for bytes_ratio@SSIM decreasing toward 1.

Deliverable:
- Better “same quality → smaller files” outcome.

### Step 4 — Quantization improvements (tables + heuristics)

Hypothesis: our quantization is not allocating bits where the human eye (and SSIM) benefits.

1) Improve per-block / per-plane quant handling:
   - Luma vs chroma quant differences
   - DC vs AC quant weighting

2) Add simple dead-zone / rounding tweaks (must remain deterministic).

3) Measure:
   - SSIM (primary) and PSNR (secondary)
   - Track bytes_ratio@SSIM

Deliverable:
- More SSIM for same bytes, especially on natural images.

### Step 5 — Probability tables / token coding efficiency

Hypothesis: our entropy coding is not as efficient as libwebp; even if prediction is good, we spend more bits.

1) Add better probability adaptation (still deterministic).
   - For a first iteration: per-frame fixed tables derived from simple counts.

2) Ensure bitwriter correctness remains fully test-gated.

Deliverable:
- Smaller files at the same SSIM.

## How to iterate (tight loop)

For each step/change:

1) Implement one focused change.
2) Run: `make test`.
3) Run the standard comparison command and paste the summary into this doc under “Progress log”.
4) If it’s an improvement, consider updating our quality regression baseline:
   - `./scripts/enc_quality_check.sh --update`

## Progress log

- 2026-01-09 baseline (256px QS 40/60/80 MODE=bpred):
  - Overall: ΔPSNR=-4.835 dB  ΔSSIM=-0.02999  bytes_ratio@SSIM=1.697

- 2026-01-09 loopfilter enabled (256px QS 40/60/80 MODE=bpred, `OURS_FLAGS="--loopfilter"`):
   - Overall: ΔPSNR=-4.773 dB  ΔSSIM=-0.02659  bytes_ratio@SSIM=1.697

(append new entries here as we improve)
