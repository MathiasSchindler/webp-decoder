# Encoder quality metrics plan — PSNR (step 1), SSIM (step 2)

Date: 2026-01-09

This document proposes a **quality regression** gate for the lossy VP8 encoder.
It complements (does not replace) the existing correctness/oracle gates.

## Goals

- Detect accidental **quality regressions** when changing encoder heuristics (mode decision, quant, tokenization, probabilities, etc.).
- Keep the repo’s core properties:
  - deterministic encoder output (including libc vs nolibc-ultra parity)
  - stable test execution via `make test` (`scripts/run_all.sh`)
- Prefer an in-repo implementation: **C tool** for computing PSNR and SSIM.

## Non-goals

- Matching libwebp/cwebp quality.
- “Perfect perceptual quality” scoring.
- Making the metrics gate a hard correctness oracle (it’s a guardrail, not a spec test).

## Core design choices (must be consistent)

Quality metrics depend heavily on *what* you compare. These choices must remain stable:

1) **Comparison space**
- Recommend: compute metrics on **8-bit sRGB RGB** (P6 PPM), per channel.
- Alternative (simpler SSIM): compute SSIM on **luma** derived from sRGB RGB.

2) **Reference vs distorted**
- Reference: decoded pixels from the input PNG.
- Distorted: pixels obtained by encoding PNG → WebP with our encoder, then decoding WebP back to pixels.

3) **Decode path**
- Short term (oracle-friendly): use `dwebp -ppm` for decoding the encoded WebP.
- Longer term (optional): use our decoder once we want to validate it as a metric pipeline.

4) **Alpha policy**
- If input images can include alpha, we must choose a deterministic policy before metrics are meaningful:
  - either pre-filter corpus to RGB-only PNGs
  - or define compositing rules (background color) and apply them consistently to both reference and distorted.

5) **Pass/fail strategy**
- Do not require exact floating results.
- Gate on:
  - per-image thresholds (min PSNR/SSIM)
  - and/or “no regression beyond tolerance” vs a pinned baseline manifest.

## Proposed artifacts

Status: implemented.

Implemented artifacts:

- Tooling:
  - `build/enc_quality_metrics` (from `tools/enc_quality_metrics.c` + `src/quality/*`)
    - Output: `psnr_rgb=<val> psnr_r=<val> psnr_g=<val> psnr_b=<val> ssim_y=<val>`
  - `build/enc_png2ppm` (from `tools/enc_png2ppm.c`)
    - RGBA policy: alpha is ignored (RGB channels only).

- Gate + baseline:
  - `scripts/enc_quality_manifest.sh` (current metrics)
  - `scripts/enc_quality_check.sh` (guardrail vs baseline)
  - `scripts/enc_quality_expected.txt` (pinned baseline; update via `--update`)
  - Wired into `make test` via `scripts/run_all.sh`.

Frozen SSIM parameters (must remain stable):
- luma: `Y = (77*R + 150*G + 29*B + 128) >> 8` (full-range)
- windowing: non-overlapping 8x8 blocks from (0,0)
- edges: include partial blocks on right/bottom edges
- aggregation: unweighted mean across blocks

All temp files should follow the existing convention: `build/test-artifacts/<script>/`.

---

## Step 1 — PSNR gate

### Definition

For 8-bit pixels with max value $MAX=255$:

- For each channel $c \in \{R,G,B\}$:
  - $\mathrm{MSE}_c = \frac{1}{N}\sum_i (x_{i,c} - y_{i,c})^2$
  - $\mathrm{PSNR}_c = 10\log_{10}(MAX^2 / \mathrm{MSE}_c)$
- Also compute a combined MSE across channels (or average PSNR) for a single summary value.

Special case:
- If MSE is 0 → PSNR is +∞ (print a large sentinel like `inf` or `999.0`).

### Implementation approach (C)

- Parse P6 PPM (binary) for both images.
- Require exact size match (width/height).
- Compute SSE and MSE using 64-bit integers:
  - per-channel SSE fits within 64-bit for reasonable image sizes (guard with overflow checks).
- Convert to PSNR using double `log10()` (linking `-lm` is acceptable for a tool and does not affect encoder build parity).
- Output a single line:

```
psnr_rgb=<val> psnr_r=<val> psnr_g=<val> psnr_b=<val>
```

### Script gate design

`enc_quality_check.sh`:
- Corpus: start with a small deterministic subset (e.g. reuse the existing encoder corpus list file if it’s PNG-based), or define a dedicated list.
- For each input PNG:
  1) Encode to WebP using `./encoder --q <Q> --mode <MODE>`.
  2) Decode WebP to PPM via `dwebp -ppm`.
  3) Decode the original PNG to a reference PPM.
     - Options:
       - use our existing PNG reader helper to write PPM (preferred, in-repo)
       - or use `dwebp` is not applicable; for PNG a simple in-repo converter tool is best.
  4) Run `build/enc_quality_metrics ref.ppm out.ppm`.

- Compare against `scripts/enc_quality_expected.txt` with tolerances.

### How to run

- Build tools: `make enc_png2ppm enc_quality_metrics`
- Run gate: `./scripts/enc_quality_check.sh`
- Update baseline (intentional quality change): `./scripts/enc_quality_check.sh --update`

### Ad-hoc comparison vs libwebp (cwebp)

For exploratory “ours vs libwebp” comparisons (not part of `make test`), use:

- `scripts/enc_vs_cwebp_quality.sh images/commons/penguin.jpg images/commons/crane.jpg images/commons/antpilla.jpg`

Defaults (override via env):

- `SIZES="256 512"` constrains max dimension while preserving aspect ratio.
- `QS="30 40 50 60 70 80"` sweeps quality values.
- `MODE=bpred` sets our encoder mode.
- `OURS_FLAGS="..."` passes extra flags to `./encoder` (e.g. `--loopfilter`).

This script:
- resizes/normalizes inputs via ImageMagick (`magick`)
- encodes with `./encoder --q ...` and `$CWEBP -q ...`
- decodes via `$DWEBP -ppm`
- computes PSNR/SSIM vs the original PNG pixels using `build/enc_quality_metrics`
- prints a per-image summary for:
  - “similar filesize”: quality delta (PSNR/SSIM)
  - “similar quality”: filesize ratio

---

## Step 2 — SSIM gate (C tool)

Status: implemented.

SSIM is more subjective and has more implementation degrees of freedom; we must pin parameters.

### Definition (single-scale SSIM)

Compute SSIM on luma $Y$ (recommended for first iteration) using the standard form:

$$
\mathrm{SSIM}(x,y) = \frac{(2\mu_x\mu_y + C_1)(2\sigma_{xy} + C_2)}{(\mu_x^2 + \mu_y^2 + C_1)(\sigma_x^2 + \sigma_y^2 + C_2)}
$$

with constants:
- $C_1 = (K_1 L)^2$, $C_2 = (K_2 L)^2$
- $L=255$, typically $K_1=0.01$, $K_2=0.03$

### Windowing

Pin one option:

Option A (simple, fast):
- 8x8 block SSIM with uniform weights, average across blocks.

Option B (more standard):
- 11x11 Gaussian window (σ≈1.5), slide across image.

Recommendation:
- Start with **Option A** (8x8 uniform) to keep code small and deterministic.
- If needed later, add the Gaussian variant behind a flag.

### Implementation approach (C)

- Convert RGB to luma using a deterministic integer approximation.
  - Example (BT.601-ish): `Y = (  66*R + 129*G +  25*B + 128) >> 8` + 16
  - Or full-range: `Y = (77*R + 150*G + 29*B + 128) >> 8`
  - Pick one and freeze it.

- Compute SSIM per window using integer sums and double for the final ratio.
- Output:

```
ssim_y=<val>
```

### Gate behavior

- Extend `enc_quality_metrics` output to include SSIM.
- Update the baseline file to include SSIM.
- Tighten or relax tolerances based on observed stability.


---

## Optional next steps

- Add a Gaussian-window SSIM variant behind a flag.
- Switch the “distorted” decode path from `dwebp -ppm` to our decoder once we want metrics to validate that pipeline end-to-end.
- Define an explicit alpha compositing policy (today: alpha is ignored in the PNG→PPM helper).
