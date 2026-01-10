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

- Default encoder mode is now `--mode bpred-rdo`.
- Default token probabilities mode is now `--token-probs adaptive`.
- Use `--mode bpred` when you want the simple baseline/reference path.
- Use `--token-probs default` when you need the old bitstream behavior (e.g. `encoder_nolibc_ultra` parity).

Note: older log entries below sometimes mention the then-default `--mode bpred`.
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
          - our side (explicit baseline mode): `./encoder --mode bpred --loopfilter ...`
       - (if needed, extend `scripts/enc_vs_cwebp_quality.sh` to optionally pass `--loopfilter`)

Implementation touchpoints:
- Comparison harness: `scripts/enc_vs_cwebp_quality.sh` (supports `OURS_FLAGS`, e.g. `OURS_FLAGS="--loopfilter"`).
- Encoder CLI flag: `src/encoder_main.c` (`--loopfilter|--lf`).

2) Decide a standard for comparisons for the week:
   - Recommend: keep `MODE=bpred` fixed (set it explicitly; don’t rely on encoder defaults), and always enable `--loopfilter` during quality comparisons.

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

Optional tuning knobs (for quick A/B experiments; defaults are unchanged unless you set them):
- `ENC_ADAPTIVE_PRIOR_STRENGTH=<N>` (default 64)
- `ENC_ADAPTIVE_MIN_TOTAL=<N>` (default 0; set to 32 to match the old “min_total=32” cutoff)

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
      - At the time, the default mode was still `--mode bpred` and all tests stayed green (today default is `--mode bpred-rdo`).
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
   - Current defaults (only used when `--mode bpred-rdo`): `mul=10`, `div=1`, `rate=entropy`, `quant=ac-deadzone`, `ac-deadzone=70`.
   - 2026-01-10 follow-up: retune the default $\lambda$ scaling at larger photo sizes (SIZES=1024, QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `MODE=bpred-rdo`):
      - Commons HQ baseline (mul=8, div=1):  Overall: ΔPSNR=-0.636 dB  ΔSSIM=-0.00716  bytes_ratio@SSIM=1.164
      - Commons HQ tuned (mul=10, div=1):    Overall: ΔPSNR=-0.700 dB  ΔSSIM=-0.00717  bytes_ratio@SSIM=1.163
      - Commons baseline (mul=8, div=1):     Overall: ΔPSNR=-0.539 dB  ΔSSIM=-0.00446  bytes_ratio@SSIM=1.112
      - Commons tuned (mul=10, div=1):       Overall: ΔPSNR=-0.460 dB  ΔSSIM=-0.00445  bytes_ratio@SSIM=1.112
      - Conclusion: `mul=10 div=1` is neutral-to-slightly-better on photo corpora without regressing the general `images/commons` set; make it the new default.
   - Files:
      - [src/encoder_main.c](src/encoder_main.c)
      - [src/enc-m08_recon/enc_recon.h](src/enc-m08_recon/enc_recon.h)
      - [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c)

- 2026-01-10 `bpred-rdo` tuning knob: experimental quant tweak (AC deadzone):
   - New flag (only affects `--mode bpred-rdo`): `--bpred-rdo-quant <default|ac-deadzone>`.
   - Intent: encourage more zero AC coefficients during RDO evaluation (rate win) at some distortion cost.
   - New flag: `--bpred-rdo-ac-deadzone N` (threshold percent of step; higher => stronger deadzone).
   - Results vs libwebp, 3-image set (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `MODE=bpred-rdo`):
      - Default quant:          Overall: ΔPSNR=-0.778 dB  ΔSSIM=-0.00630  bytes_ratio@SSIM=1.320
      - AC deadzone @75%:       Overall: ΔPSNR=-0.541 dB  ΔSSIM=-0.00875  bytes_ratio@SSIM=1.117
      - AC deadzone @60%:       Overall: ΔPSNR=-0.313 dB  ΔSSIM=-0.00292  bytes_ratio@SSIM=1.206
   - Validation vs libwebp, commons-hq (12 photos, 256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `MODE=bpred-rdo`):
      - Default quant:          Overall: ΔPSNR=-0.622 dB  ΔSSIM=-0.00635  bytes_ratio@SSIM=1.281
      - AC deadzone @60%:       Overall: ΔPSNR=-0.537 dB  ΔSSIM=-0.00613  bytes_ratio@SSIM=1.185
   - Conclusion: @60% is a consistent improvement in bytes_ratio@SSIM without hurting SSIM overall; make this the default for `bpred-rdo`.

   - 2026-01-10 follow-up: retune `--bpred-rdo-ac-deadzone` on commons-hq at larger sizes (SIZES=512 1024, QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
      - @60%: bytes_ratio@SSIM=1.195
      - @70%: bytes_ratio@SSIM=1.149
      - Conclusion: @70% is a consistent size win on photo corpora; update the `bpred-rdo` default.

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

- 2026-01-10 zebra artifact forensics + mitigation (vertical brightening, macroblock-aligned):
   - Repro case (external corpus): `../imagenet/images/commons-00018.jpg` resized to 1024px, `q=30`.
   - Symptom in baseline `bpred`: large positive luma bias that increases towards the bottom of the frame (visible as light vertical “zebra” areas).
   - Added dependency-free tooling for investigating artifact dirs produced by `scripts/enc_vs_cwebp_quality.sh`:
      - `scripts/analyze_ppm_zebra.py`: per-column mean ΔY (and phase stats at period=16), plus coarse RGB/Cb/Cr deltas.
      - `scripts/ppm_to_png.py`: convert PPM→PNG and generate luma-diff heatmaps.
   - Mitigation experiment (kept in experimental `--mode bpred-rdo` only to keep baseline gates stable):
      - Add a small DC-only post-quant refinement during mode evaluation in [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c).
      - Tries DC deltas in [-1..+1] and accepts only if it improves a boundary-weighted SSE score.
   - Result on the repro case: `bpred-rdo` reduces global luma bias substantially vs baseline (and reduces bottom-third brightening), while `make test` remains green because baseline `bpred` is unchanged.

- 2026-01-10 Step 2 continuation: `bpred-rdo` can choose I16 vs B_PRED (RDO-lite at macroblock level):
   - Change: extend `--mode bpred-rdo` to decide per macroblock between I16 (modes 0..3) and B_PRED (mode 4), using the same quant-aware $J=D+\lambda R$ scoring.
   - Safety: `make test` stays green because this work was isolated behind `--mode bpred-rdo` (at the time, the default mode was still `--mode bpred`).
   - Vs libwebp on the original 3-image set (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
      - Baseline `MODE=bpred`: Overall: ΔPSNR=-5.645 dB  ΔSSIM=-0.02894  bytes_ratio@SSIM=1.694
      - `MODE=bpred-rdo` (proxy-era defaults at the time: `--bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1`): Overall: ΔPSNR=-3.560 dB  ΔSSIM=-0.01527  bytes_ratio@SSIM=1.422
   - Quick $
     \lambda$ sweep on the same set (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `MODE=bpred-rdo`):
      - `mul=4 div=1`: Overall: ΔPSNR=-3.807 dB  ΔSSIM=-0.01594  bytes_ratio@SSIM=1.523
      - `mul=6 div=1`: Overall: ΔPSNR=-3.560 dB  ΔSSIM=-0.01527  bytes_ratio@SSIM=1.422
      - `mul=8 div=1`: Overall: ΔPSNR=-3.625 dB  ΔSSIM=-0.01553  bytes_ratio@SSIM=1.539
   - Conclusion: `mul=6 div=1` remains the best of these three on this set (especially for bytes_ratio@SSIM).

- 2026-01-10 Step 2 continuation: UV DC refinement + broader validation:
   - Change: apply the same small DC-only post-quant refinement to chroma blocks during UV mode evaluation in `bpred-rdo`.
   - 3-image set impact (256px QS 40/60/80): essentially neutral, with a tiny bytes_ratio@SSIM improvement (1.422 → 1.421).
   - Commons HQ (12 photos) vs libwebp (256px QS 40/60/80, `OURS_FLAGS="--loopfilter --bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1"`):
      - `MODE=bpred`:     Overall: ΔPSNR=-5.794 dB  ΔSSIM=-0.03562  bytes_ratio@SSIM=1.701
      - `MODE=bpred-rdo`: Overall: ΔPSNR=-4.504 dB  ΔSSIM=-0.02074  bytes_ratio@SSIM=1.532
   - Negative result: tried switching 4x4 mode scoring SSE to a boundary-weighted SSE (to protect future predictors), but it regressed Overall on the 3-image set, so it was reverted.

- 2026-01-10 Step 2 continuation: weight I16 Y2 rate higher (neutral):
   - Change: in `bpred-rdo` I16 evaluation, weight the rate proxy of the Y2 (WHT/DC) block by 2x.
   - Result: no measurable change on the 3-image set and no change in the commons-hq Overall at the current defaults, so we keep it but it’s not a win by itself.

- 2026-01-10 Step 2 continuation: tried a more token-like residual rate proxy (regressed; reverted):
   - Change: modified `rdo_rate_proxy4x4()` in [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c) to be zigzag/EOB/zero-run aware (closer to the VP8 token stream structure).
   - Result: large regressions on the 3-image baseline set even after re-tuning $\lambda$.
      - Example (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `MODE=bpred-rdo`, `mul=6 div=1`): Overall: ΔPSNR=-6.804 dB  ΔSSIM=-0.03453  bytes_ratio@SSIM=1.756
      - This is much worse than the previous magnitude-only proxy at the same knobs: Overall: ΔPSNR=-3.571 dB  ΔSSIM=-0.01527  bytes_ratio@SSIM=1.421
   - Conclusion: the “token-like” proxy was too punitive/miscalibrated for our current $\lambda$(qindex) scale; we reverted to the prior magnitude-based proxy.

- 2026-01-10 Step 2 continuation: entropy-style token cost as the rate term (big win so far):
   - Change: added an optional `bpred-rdo` rate estimator that approximates VP8's coefficient token coding cost (tree walk with default probs) and uses that as $R$ in $J=D+\lambda R$.
   - New flag (only affects `--mode bpred-rdo`): `--bpred-rdo-rate <proxy|entropy>` (default is now `entropy`).
   - Results vs libwebp, 3-image set (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `MODE=bpred-rdo`):
      - `--bpred-rdo-rate entropy --bpred-rdo-lambda-mul 8 --bpred-rdo-lambda-div 1`: Overall: ΔPSNR=-0.778 dB  ΔSSIM=-0.00630  bytes_ratio@SSIM=1.320
      - Previous proxy default (`--bpred-rdo-rate proxy --bpred-rdo-lambda-mul 6 --bpred-rdo-lambda-div 1`): Overall: ΔPSNR=-3.571 dB  ΔSSIM=-0.01527  bytes_ratio@SSIM=1.421
   - Results vs libwebp, commons-hq (12 photos, 256px QS 40/60/80):
      - Proxy default:  Overall: ΔPSNR=-4.504 dB  ΔSSIM=-0.02074  bytes_ratio@SSIM=1.532
      - Entropy (mul=8): Overall: ΔPSNR=-0.622 dB  ΔSSIM=-0.00635  bytes_ratio@SSIM=1.281
   - Notes:
      - This keeps “mode sprawl” contained: still one experimental mode (`bpred-rdo`), just a runtime knob.
      - `make test` remains green.

- 2026-01-10 Step 2 continuation: tried adding entropy-style mode signaling costs (regressed; reverted):
   - Change attempt: in `--bpred-rdo-rate entropy`, also include entropy-estimated signaling costs for keyframe ymode/uv mode/bmode (tree/prob-based) in the rate term.
   - Result: massive regression vs libwebp on the 3-image set, so this was reverted. Example (256px QS 40/60/80, `OURS_FLAGS="--loopfilter"`, `MODE=bpred-rdo`, `--bpred-rdo-rate entropy --bpred-rdo-lambda-mul 8 --bpred-rdo-lambda-div 1`):
      - Regressed: Overall: ΔPSNR≈-8.6 dB  ΔSSIM≈-0.047  bytes_ratio@SSIM≈1.85
      - Restored after revert: Overall: ΔPSNR=-0.778 dB  ΔSSIM=-0.00630  bytes_ratio@SSIM=1.320
   - Conclusion: our coefficient-token entropy estimator is a strong win, but extending it to mode signaling needs better calibration/context handling.

- 2026-01-10 tooling: lambda sweep script supports `--rate`:
   - Change: [scripts/enc_bpred_rdo_lambda_sweep.py](scripts/enc_bpred_rdo_lambda_sweep.py) now supports `--rate proxy|entropy` to sweep lambda for either rate estimator.

- 2026-01-10 larger Commons/Commons-HQ focus (512/1024px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
   - `images/commons/*.{jpg,png}` (7 images):
      - `MODE=bpred`:     Overall: ΔPSNR=-9.913 dB  ΔSSIM=-0.04541  bytes_ratio@SSIM=1.984
      - `MODE=bpred-rdo`: Overall: ΔPSNR=-0.657 dB  ΔSSIM=-0.00566  bytes_ratio@SSIM=1.212
   - `images/commons-hq/*.jpg` (12 images):
      - `MODE=bpred`:     Overall: ΔPSNR=-10.166 dB  ΔSSIM=-0.05335  bytes_ratio@SSIM=2.008
      - `MODE=bpred-rdo`: Overall: ΔPSNR=-0.718 dB  ΔSSIM=-0.00501  bytes_ratio@SSIM=1.233

- 2026-01-10 loopfilter param mapping v2 (aligned to libwebp defaults):
   - Updated [src/enc-m08_filter/enc_loopfilter.c](src/enc-m08_filter/enc_loopfilter.c) to better match `cwebp`’s default keyframe loopfilter fields (notably `sharpness=0` and lower `level` for most qindex values).
   - Updated the pinned header-field gate in [scripts/enc_m10_loopfilter_expected.txt](scripts/enc_m10_loopfilter_expected.txt).
   - Rerun vs libwebp on larger Commons/Commons-HQ (512/1024px QS 40/60/80, `OURS_FLAGS="--loopfilter"`):
      - `images/commons/*.{jpg,png}`:
         - `MODE=bpred`:     Overall: ΔPSNR=-9.912 dB  ΔSSIM=-0.04558  bytes_ratio@SSIM=1.983
         - `MODE=bpred-rdo`: Overall: ΔPSNR=-0.650 dB  ΔSSIM=-0.00584  bytes_ratio@SSIM=1.212
      - `images/commons-hq/*.jpg`:
         - `MODE=bpred`:     Overall: ΔPSNR=-10.167 dB  ΔSSIM=-0.05350  bytes_ratio@SSIM=2.008
         - `MODE=bpred-rdo`: Overall: ΔPSNR=-0.718 dB  ΔSSIM=-0.00524  bytes_ratio@SSIM=1.233
   - Conclusion: essentially neutral on these corpora; keep for closer apples-to-apples with libwebp.

- 2026-01-10 Step 5 (token coding efficiency): adaptive coefficient probabilities (experimental flag)
   - Implemented `--token-probs <default|adaptive>` in [src/encoder_main.c](src/encoder_main.c).
   - Added keyframe coefficient probability adaptation (counts token branches, emits selective prob updates, then encodes tokens using the updated tables):
      - [src/enc-m07_tokens/enc_vp8_tokens.c](src/enc-m07_tokens/enc_vp8_tokens.c)
      - [src/enc-m07_tokens/enc_vp8_tokens.h](src/enc-m07_tokens/enc_vp8_tokens.h)
   - Safety: `make test` stays green; default output unchanged unless the flag is used.
   - Vs libwebp on larger Commons/Commons-HQ with `OURS_FLAGS="--loopfilter --token-probs adaptive"` (512/1024px QS 40/60/80):
      - `images/commons/*.{jpg,png}`:
         - `MODE=bpred`:     Overall: ΔPSNR=-9.794 dB  ΔSSIM=-0.04505  bytes_ratio@SSIM=1.839
         - `MODE=bpred-rdo`: Overall: ΔPSNR=-0.417 dB  ΔSSIM=-0.00342  bytes_ratio@SSIM=1.151
      - `images/commons-hq/*.jpg`:
         - `MODE=bpred`:     Overall: ΔPSNR=-9.945 dB  ΔSSIM=-0.05178  bytes_ratio@SSIM=1.891
         - `MODE=bpred-rdo`: Overall: ΔPSNR=-0.634 dB  ΔSSIM=-0.00456  bytes_ratio@SSIM=1.181
   - Conclusion: clear win in bytes_ratio@SSIM (and slightly better ΔPSNR/ΔSSIM) on large photo corpora.

- 2026-01-10 Step 5 follow-up: make adaptive coeff probs more stable
   - Change: added simple smoothing/prior toward default probs and ignored tiny prob changes to reduce overfitting/noisy updates in `--token-probs adaptive`.
   - File: [src/enc-m07_tokens/enc_vp8_tokens.c](src/enc-m07_tokens/enc_vp8_tokens.c)
   - Safety: `make test` stays green; still opt-in.
   - Sanity run vs libwebp on the default micro corpus (5 images, 512/1024px QS 40/60/80, `OURS_FLAGS="--loopfilter --token-probs adaptive"`):
      - `MODE=bpred`:     Overall: ΔPSNR=-9.049 dB  ΔSSIM=-0.04631  bytes_ratio@SSIM=1.722
      - `MODE=bpred-rdo`: Overall: ΔPSNR=-0.718 dB  ΔSSIM=-0.00460  bytes_ratio@SSIM=1.178

- 2026-01-10 Step 5 (token coding efficiency): mb_skip_coeff signaling + token omission (experimental flag)
   - Added `--mb-skip` (opt-in) in [src/encoder_main.c](src/encoder_main.c).
   - Encoder now can signal per-macroblock `mb_skip_coeff` in partition 0 and omit coefficient tokens for macroblocks that are fully zero.
   - Implementation: [src/enc-m07_tokens/enc_vp8_tokens.c](src/enc-m07_tokens/enc_vp8_tokens.c)
   - Safety: `make test` stays green; default output unchanged unless `--mb-skip` is used.
   - TODO: benchmark impact on `bytes_ratio@SSIM` alongside `--token-probs adaptive`.

(append new entries here as we improve)

- 2026-01-10 default settings update (commons-hq driven): enable adaptive token probs by default
   - Change: set encoder default to `--token-probs adaptive` because it consistently improves `bytes_ratio@SSIM` vs libwebp on large photo corpora.
   - Files:
      - [src/encoder_main.c](src/encoder_main.c)
      - [scripts/enc_ultra_parity_check.sh](scripts/enc_ultra_parity_check.sh) (now pins `--token-probs default` for parity)
