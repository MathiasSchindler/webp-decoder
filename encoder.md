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

## Start here (TL;DR)

- If you’re new: start with the default `--mode bpred` (this is the baseline and the only “supported” path).
- We intentionally keep exactly one experimental alternative: `--mode bpred-rdo` (no other bpred variants).
- Quick compare (prints both Overalls):

```sh
OURS_FLAGS="--loopfilter" ./scripts/enc_bpred_rdo_tune.sh
OURS_FLAGS="--loopfilter" ./scripts/enc_bpred_rdo_tune.sh images/commons-hq
```

- Fast local tuning sweep for `bpred-rdo` (no libwebp):

```sh
python3 scripts/enc_bpred_rdo_lambda_sweep.py images/commons-hq \
   --sizes 256 --qs 40 60 80 \
   --mul 1 2 3 4 6 8 --div 1 2 3 4 -j 4
```

- After any change: run `make test` and record the `Overall:` lines below.

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

Fast inner-loop (runs both modes and prints both Overalls):

```sh
./scripts/enc_bpred_rdo_tune.sh images/commons/crane.jpg
```

Broader (still small) micro-corpus:

```sh
./scripts/enc_bpred_rdo_tune.sh
```
4) If it’s an improvement, consider updating our quality regression baseline:
   - `./scripts/enc_quality_check.sh --update`

## Progress log

- 2026-01-09 baseline (256px QS 40/60/80 MODE=bpred):
  - Overall: ΔPSNR=-4.835 dB  ΔSSIM=-0.02999  bytes_ratio@SSIM=1.697

- 2026-01-09 loopfilter enabled (256px QS 40/60/80 MODE=bpred, `OURS_FLAGS="--loopfilter"`):
   - Overall: ΔPSNR=-4.773 dB  ΔSSIM=-0.02659  bytes_ratio@SSIM=1.697

- 2026-01-10 standard comparison rerun (macOS `sips` resize fallback, 256px QS 40/60/80 MODE=bpred, `OURS_FLAGS="--loopfilter"`):
   - Overall: ΔPSNR=-5.645 dB  ΔSSIM=-0.02894  bytes_ratio@SSIM=1.694

- 2026-01-10 loopfilter param mapping v1 (qindex→level/sharpness):
   - Implemented a new deterministic piecewise mapping in [src/enc-m08_filter/enc_loopfilter.c](src/enc-m08_filter/enc_loopfilter.c).
   - Updated the pinned header-field gate in [scripts/enc_m10_loopfilter_expected.txt](scripts/enc_m10_loopfilter_expected.txt).
   - Sanity metrics on the in-repo synthetic corpus (30 images, q=75, `LOOPFILTER=1`), comparing *new mapping* vs *previous mapping*:
      - Mean ΔPSNR_RGB: -0.011795 dB
      - Mean ΔSSIM_Y:   -0.000018
   - Conclusion: essentially neutral on this corpus; needs tuning on natural images via `enc_vs_cwebp_quality.sh`.

- 2026-01-10 Step 2 (bpred mode selection):
   - Tried switching B_PRED (4x4) and UV (8x8) selection from SAD → pixel-domain SSE in [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c).
   - Result: `make test` caught regressions via `enc_quality_check.sh` (so this is **not** enabled by default).
   - Follow-up: added an **experimental** mode `--mode bpred-rdo` that is quantization-aware:
      - For each candidate predictor mode, it simulates `ftransform → quantize → dequant → inverse → reconstruct` and scores SSE vs original.
      - Default `--mode bpred` remains unchanged and all tests stay green.
   - Files:
      - [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c) (new `enc_vp8_encode_bpred_uv_rdo_inloop()`)
      - [src/encoder_main.c](src/encoder_main.c) (new `--mode bpred-rdo`)
      - [src/enc-m08_recon/enc_recon.h](src/enc-m08_recon/enc_recon.h) (new entrypoint)
   - Quick spot-check at q=75 on a few `images/png-in/*` (baseline `bpred` vs `bpred-rdo`):
      - blockcheck2_16x16_255_000_000_000_255_000.png: PSNR_RGB 7.58 → 10.50, SSIM_Y 0.386 → 0.998
      - checker_16x16_017_034_051_254_253_252.png:     PSNR_RGB 31.65 → 33.56, SSIM_Y 0.99951 → 0.99993
      - hgrad_16x16_255_000_000_000_255_000.png:       PSNR_RGB 36.69 → 37.43, SSIM_Y 0.99282 → 0.99669
   - Next: run the standard `enc_vs_cwebp_quality.sh` harness with `MODE=bpred` and our `--mode bpred-rdo` to see if this is a real win on natural images.

- 2026-01-10 `bpred-rdo` vs libwebp on natural images (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
   - Overall: ΔPSNR=-14.236 dB  ΔSSIM=-0.14422  bytes_ratio@SSIM=2.334
   - Conclusion: not competitive yet; this is distortion-only mode choice and needs an explicit rate term ($J=D+\lambda R$).

- 2026-01-10 `bpred-rdo` RDO-lite v1 (added a simple rate proxy + $\lambda$(qindex), 256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
   - Overall: ΔPSNR=-12.796 dB  ΔSSIM=-0.11254  bytes_ratio@SSIM=2.022
   - Notes: mode scoring now uses $J=D+\lambda R$, where $R$ is a cheap proxy (non-zero quantized coeff count, DC weighted) and $\lambda$ grows with qindex.
   - Conclusion: directionally better (smaller `bytes_ratio@SSIM`, less negative ΔPSNR/ΔSSIM), but still far from competitive.

- 2026-01-10 `bpred-rdo` RDO-lite v2 (tuned $\lambda$ schedule, 256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
   - Overall: ΔPSNR=-12.715 dB  ΔSSIM=-0.10821  bytes_ratio@SSIM=1.994
   - Notes: same $R$ proxy as v1; increased rate weight ($\lambda$) to reduce over-spending.
   - Conclusion: small but real improvement vs v1 (bytes ratio drops below 2.0; ΔPSNR/ΔSSIM slightly closer to 0).

- 2026-01-10 `bpred-rdo` tuning knobs (runtime $\lambda$ scaling):
   - Added two flags to scale the $\lambda$(qindex) schedule without recompiling:
      - `--bpred-rdo-lambda-mul N`
      - `--bpred-rdo-lambda-div N`
   - Design note: we intentionally keep only two intra strategies (`bpred` and experimental `bpred-rdo`) to avoid mode sprawl.
   - Current defaults (only used when `--mode bpred-rdo`): `mul=6`, `div=1`.
   - Files:
      - [src/encoder_main.c](src/encoder_main.c)
      - [src/enc-m08_recon/enc_recon.h](src/enc-m08_recon/enc_recon.h)
      - [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c)

- 2026-01-10 `bpred-rdo` tuning harness + new photo corpus:
   - Added a small, photo-heavy in-repo corpus: [images/commons-hq/README.md](images/commons-hq/README.md)
   - Added fast iteration harnesses:
      - `scripts/enc_bpred_rdo_local_fast.sh` (local-only: ours encode + ours decode + metrics)
      - `scripts/enc_bpred_rdo_tune.sh` (vs libwebp: wraps `enc_vs_cwebp_quality.sh`)

- 2026-01-10 `bpred-rdo` $\lambda$ scaling sweep result (commons-hq, 256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
   - Baseline (`--bpred-rdo-lambda-mul 1 --bpred-rdo-lambda-div 1`):
      - Overall: ΔPSNR=-13.433 dB  ΔSSIM=-0.11806  bytes_ratio@SSIM=1.953
   - Tuned (`--bpred-rdo-lambda-mul 2 --bpred-rdo-lambda-div 1`):
      - Overall: ΔPSNR=-13.403 dB  ΔSSIM=-0.11161  bytes_ratio@SSIM=1.902

- 2026-01-10 `bpred-rdo` official bounded $\lambda$ sweep (commons-hq local-fast, SIZES=256 QS 40/60/80, mul={1,2,3,4,6,8}, div={1,2,3,4}):
   - Best setting from the sweep: `--bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1`
   - Validation vs libwebp on commons-hq (256px QS 40/60/80, `OURS_FLAGS="--loopfilter --bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1"`):
      - Overall: ΔPSNR=-13.042 dB  ΔSSIM=-0.10818  bytes_ratio@SSIM=1.884
   - Follow-up: set this as the built-in default for `bpred-rdo` to keep experimentation contained (no new modes).

- 2026-01-10 `bpred-rdo` $\lambda$ scaling on default mixed corpus (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
   - Baseline (`--bpred-rdo-lambda-mul 1 --bpred-rdo-lambda-div 1`):
      - Overall: ΔPSNR=-13.018 dB  ΔSSIM=-0.10562  bytes_ratio@SSIM=1.959
   - Tuned (`--bpred-rdo-lambda-mul 2 --bpred-rdo-lambda-div 1`):
      - Overall: ΔPSNR=-12.449 dB  ΔSSIM=-0.10459  bytes_ratio@SSIM=1.904

- 2026-01-10 `bpred-rdo` RDO-lite v3 (improved $R$ proxy: include coefficient magnitude, not just nnz):
   - Change: updated the cheap rate proxy to better correlate with actual token cost by using a small log2-like magnitude binning.
   - Files:
      - [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c)
   - Results vs libwebp (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `bpred-rdo` defaults at the time mul=2/div=1):
      - Commons HQ (12 photos): Overall: ΔPSNR=-8.532 dB  ΔSSIM=-0.05651  bytes_ratio@SSIM=1.759
      - Default micro corpus (5 mixed): Overall: ΔPSNR=-7.924 dB  ΔSSIM=-0.04283  bytes_ratio@SSIM=1.774
   - Conclusion: large improvement in `bpred-rdo` quality/size vs earlier v2, while keeping default `bpred` unchanged.

- 2026-01-10 `bpred-rdo` RDO-lite v4 (rate proxy: add small mode signaling costs):
   - Change: add tiny fixed costs for UV mode signaling and 4x4 bpred mode signaling, and include them in $R$ alongside the coefficient-magnitude proxy.
   - Results vs libwebp (256px QS 40/60/80):
      - Commons HQ (12 photos, `OURS_FLAGS="--loopfilter --bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1"`): Overall: ΔPSNR=-4.848 dB  ΔSSIM=-0.02093  bytes_ratio@SSIM=1.547
      - Default micro corpus (5 mixed, `OURS_FLAGS="--loopfilter --bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1"`): Overall: ΔPSNR=-4.451 dB  ΔSSIM=-0.01824  bytes_ratio@SSIM=1.532
   - Notes:
      - This is still one experimental mode (`bpred-rdo`), no new mode sprawl.
      - `make test` remains green.

- 2026-01-10 `bpred-rdo` bounded $\lambda$ sweep after v3 $R$ proxy (commons-hq local-fast, SIZES=256 QS 40/60/80, mul={1,2,3,4,6,8}, div={1,2,3,4}):
   - Sweep winner on commons-hq: `--bpred-rdo-lambda-mul 8 --bpred-rdo-lambda-div 3`
   - Validation vs libwebp on commons-hq:
      - Overall: ΔPSNR=-8.810 dB  ΔSSIM=-0.05535  bytes_ratio@SSIM=1.755
   - Note: on the default mixed corpus, `mul=6/div=1` is still better overall, so we keep `mul=6/div=1` as the default for now.

- 2026-01-10 `bpred-rdo` combined-corpus default (commons-hq + micro, bounded sweep mul={1,2,3,4,6,8}, div={1,2,3,4}):
   - Goal: pick **one** reasonable default for `bpred-rdo` without mode sprawl or per-corpus “recommended flags”.
   - Sweep winner (local-fast): `--bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1`
   - Set this as the built-in default for `bpred-rdo`.

(append new entries here as we improve)
