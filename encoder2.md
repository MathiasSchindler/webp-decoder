# Encoder improvement proposal (next iteration)

Date: 2026-01-10

This document proposes the next set of experiments to implement, test, and (if they win) keep as defaults.

Constraints (non-negotiable):
- Keep `make test` green.
- Keep deterministic output.
- Keep `encoder_nolibc_ultra` parity (where already enforced by scripts).

## Baseline definition (today)

We define “baseline” as:
- Our encoder: **current defaults** (no extra flags besides `--loopfilter` in the harness)
  - `MODE=bpred-rdo`
  - default `--token-probs adaptive`
  - default bpred-rdo tuning: `--bpred-rdo-rate entropy`, `--bpred-rdo-quant ac-deadzone`, `--bpred-rdo-ac-deadzone 70`, `--bpred-rdo-lambda-mul 10`, `--bpred-rdo-lambda-div 1`
- Reference encoder: **libwebp** (`cwebp -q Q`)
- Comparison harness: [scripts/enc_vs_cwebp_quality.sh](scripts/enc_vs_cwebp_quality.sh)
- Standard run shape for photo corpora:
  - `SIZES="512 1024"`
  - `QS="40 60 80"`
  - `OURS_FLAGS="--loopfilter"`
  - `JOBS=8` (or any reasonable parallelism)

### Baseline corpora (fixed test files)

We will keep these exact file lists stable across the next iterations so results remain comparable.

**Photo corpus A (commons-hq, 12 photos)**
- [images/commons-hq/commons-00047.jpg](images/commons-hq/commons-00047.jpg)
- [images/commons-hq/commons-00068.jpg](images/commons-hq/commons-00068.jpg)
- [images/commons-hq/commons-00080.jpg](images/commons-hq/commons-00080.jpg)
- [images/commons-hq/commons-00087.jpg](images/commons-hq/commons-00087.jpg)
- [images/commons-hq/commons-00161.jpg](images/commons-hq/commons-00161.jpg)
- [images/commons-hq/commons-00165.jpg](images/commons-hq/commons-00165.jpg)
- [images/commons-hq/commons-00179.jpg](images/commons-hq/commons-00179.jpg)
- [images/commons-hq/commons-00188.jpg](images/commons-hq/commons-00188.jpg)
- [images/commons-hq/commons-00204.jpg](images/commons-hq/commons-00204.jpg)
- [images/commons-hq/commons-00209.jpg](images/commons-hq/commons-00209.jpg)
- [images/commons-hq/commons-00232.jpg](images/commons-hq/commons-00232.jpg)
- [images/commons-hq/commons-00256.jpg](images/commons-hq/commons-00256.jpg)

**Micro corpus B (3 classic images)**
- [images/commons/antpilla.jpg](images/commons/antpilla.jpg)
- [images/commons/crane.jpg](images/commons/crane.jpg)
- [images/commons/penguin.jpg](images/commons/penguin.jpg)

### Baseline scores (vs libwebp)

All results below are from:
- `SIZES="512 1024" QS="40 60 80" OURS_FLAGS="--loopfilter"`

**Current defaults (MODE=bpred-rdo)**
- commons-hq (12): Overall: ΔPSNR=-0.642 dB  ΔSSIM=-0.00769  bytes_ratio@SSIM=1.149
- micro corpus (3): Overall: ΔPSNR=-0.443 dB  ΔSSIM=-0.00492  bytes_ratio@SSIM=1.100

**Reference baseline path (MODE=bpred)**
- commons-hq (12): Overall: ΔPSNR=-9.945 dB  ΔSSIM=-0.05178  bytes_ratio@SSIM=1.892
- micro corpus (3): Overall: ΔPSNR=-9.326 dB  ΔSSIM=-0.03631  bytes_ratio@SSIM=1.863

## How we measure (commands)

Run baseline (current defaults):

```sh
JOBS=8 SIZES="512 1024" QS="40 60 80" MODE=bpred-rdo OURS_FLAGS="--loopfilter" \
  ./scripts/enc_vs_cwebp_quality.sh images/commons-hq/*.jpg | tail -n 40

JOBS=8 SIZES="512 1024" QS="40 60 80" MODE=bpred-rdo OURS_FLAGS="--loopfilter" \
  ./scripts/enc_vs_cwebp_quality.sh \
  images/commons/antpilla.jpg images/commons/crane.jpg images/commons/penguin.jpg | tail -n 40
```

Run reference baseline mode (bpred):

```sh
JOBS=8 SIZES="512 1024" QS="40 60 80" MODE=bpred OURS_FLAGS="--loopfilter" \
  ./scripts/enc_vs_cwebp_quality.sh images/commons-hq/*.jpg | tail -n 40

JOBS=8 SIZES="512 1024" QS="40 60 80" MODE=bpred OURS_FLAGS="--loopfilter" \
  ./scripts/enc_vs_cwebp_quality.sh \
  images/commons/antpilla.jpg images/commons/crane.jpg images/commons/penguin.jpg | tail -n 40
```

After any code change:
- Run `make test`.
- Re-run the baseline comparisons above and paste the new `Overall:` lines into a “Progress log” section here.

## Proposal: next experiments (step-by-step)

We’ll do these in order, one at a time, only keeping wins.

### Experiment 1 — RDO-lite “dry-run bitcount” for intra mode decisions (highest ROI)

Goal:
- Improve rate modeling in `bpred-rdo` by measuring actual bit cost for candidate modes using a scratch bitwriter, rather than relying on approximations.

High-level approach:
1) Add a helper that can encode *just enough* of the candidate decision into a scratch VP8 writer to estimate delta bits.
   - Keep it local to `bpred-rdo` decision loops.
   - Do not affect the final bitstream unless the chosen mode changes.
2) Use the scratch bitcount as the $R$ term in $J = D + \lambda R$.
3) Keep deterministic behavior by:
   - Fixed candidate ordering.
   - No floating point in decisions (or quantize to integers).

Implementation touchpoints (expected):
- [src/enc-m08_recon/enc_recon.c](src/enc-m08_recon/enc_recon.c) (bpred-rdo evaluation loops)
- [src/enc-m02_vp8_bitwriter/enc_bool.c](src/enc-m02_vp8_bitwriter/enc_bool.c) and/or token helpers for scratch bit cost accounting
- Potentially token-cost helpers in [src/enc-m07_tokens/enc_vp8_tokens.c](src/enc-m07_tokens/enc_vp8_tokens.c)

Safety plan:
1) Gate behind existing `MODE=bpred-rdo` only.
2) `make test`.
3) Run baseline comparisons (commons-hq + micro corpus).

Acceptance criteria:
- commons-hq (12): `bytes_ratio@SSIM` improves by at least ~0.01 absolute (e.g. 1.149 → ≤ 1.139) with no meaningful SSIM regression.
- micro corpus (3): no worse `bytes_ratio@SSIM` (≤ 1.100 + 0.005).

### Experiment 2 — Quantization weighting (DC/AC and luma/chroma)

Goal:
- Reduce bits for the same SSIM by adjusting how quantization treats DC vs AC and luma vs chroma.

High-level approach:
1) Add a small, deterministic scaling layer applied to quant steps:
   - Separate scaling for Y vs UV.
   - Separate scaling for DC vs AC.
2) Start with conservative defaults and a single new tuning knob (or keep it internal until proven).

Implementation touchpoints (expected):
- [src/enc-m06_quant/enc_quant.c](src/enc-m06_quant/enc_quant.c)
- [src/enc-m06_quant/enc_quality_table.c](src/enc-m06_quant/enc_quality_table.c)
- Possibly the qindex mapping in [src/enc-m06_quant/enc_quality_table.c](src/enc-m06_quant/enc_quality_table.c)

Safety plan:
1) Start by affecting only `MODE=bpred-rdo` (if practical) to avoid forcing a baseline update.
2) `make test`.
3) Run baseline comparisons.

Acceptance criteria:
- commons-hq (12): `bytes_ratio@SSIM` improves by at least ~0.01 absolute.
- No visible “color wash” regressions on commons-hq at QS 40/60/80 (quick spot-check of a few outputs is enough).

### Experiment 3 — Adaptive token-prob update strategy improvements (savings/overhead)

Goal:
- Improve entropy efficiency (bits) without changing reconstruction.

High-level approach:
1) Improve the update decision rule:
   - Better estimate of signaling overhead.
   - Better estimate of expected savings (avoid overfitting to tiny sample sizes).
2) Consider per-band/context priors (still deterministic), instead of one global prior strength.

Implementation touchpoints (expected):
- [src/enc-m07_tokens/enc_vp8_tokens.c](src/enc-m07_tokens/enc_vp8_tokens.c)

Safety plan:
1) Since token probs are already default adaptive, this must remain stable/deterministic.
2) `make test`.
3) Run baseline comparisons.

Acceptance criteria:
- commons-hq (12): `bytes_ratio@SSIM` improves measurably without harming ΔSSIM.

## Progress log (append)

- 2026-01-10 baseline (current defaults, MODE=bpred-rdo, `OURS_FLAGS="--loopfilter"`, SIZES=512/1024 QS 40/60/80):
  - commons-hq (12): Overall: ΔPSNR=-0.642 dB  ΔSSIM=-0.00769  bytes_ratio@SSIM=1.149
  - micro corpus (3): Overall: ΔPSNR=-0.443 dB  ΔSSIM=-0.00492  bytes_ratio@SSIM=1.100

- 2026-01-10 Experiment 1 (bpred-rdo `--bpred-rdo-rate dry-run`, `OURS_FLAGS="--loopfilter --bpred-rdo-rate dry-run"`, SIZES=512/1024 QS 40/60/80):
  - commons-hq (12): Overall: ΔPSNR=-0.644 dB  ΔSSIM=-0.00769  bytes_ratio@SSIM=1.144
  - micro corpus (3): Overall: ΔPSNR=-0.445 dB  ΔSSIM=-0.00492  bytes_ratio@SSIM=1.096
  - Notes: this adds a new experimental rate estimator that dry-runs coefficient token coding via the real bool encoder; defaults remain unchanged for now.

- 2026-01-10 Experiment 1 follow-up (attempted per-4x4 dry-run bitcount inside bmode selection):
  - Result: large regression (bytes_ratio@SSIM worsened significantly); reverted.
  - Takeaway: resetting the bool coder state per-block is not a good local rate proxy for choosing b_modes.
