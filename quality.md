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

Status:
- Step 1 (PSNR): **implemented** and wired into `make test`.
- Step 2 (SSIM): **implemented** and wired into `make test`.

Implemented artifacts (Step 1):

- New C tool (PSNR):
  - `build/enc_quality_metrics` (built from `tools/enc_quality_metrics.c` + `src/quality/*`)
  - Inputs: reference PPM and distorted PPM
  - Output: machine-readable one-liner:
    - `psnr_rgb=<val> psnr_r=<val> psnr_g=<val> psnr_b=<val>`

- SSIM (Step 2):
  - Output includes: `ssim_y=<val>`
  - Frozen parameters (must remain stable):
    - luma: `Y = (77*R + 150*G + 29*B + 128) >> 8` (full-range)
    - windowing: non-overlapping 8x8 blocks from (0,0)
    - edges: include partial blocks on right/bottom edges
    - aggregation: unweighted mean across blocks

- New helper tool (PNG → PPM):
  - `build/enc_png2ppm` (built from `tools/enc_png2ppm.c`)
  - Deterministic policy: if input PNG is RGBA, alpha is ignored (RGB channels only).

- New scripts:
  - `scripts/enc_quality_manifest.sh` (generate current metrics manifest)
  - `scripts/enc_quality_check.sh` (gate run; compare against baseline with tolerances)

- New expected file:
  - `scripts/enc_quality_expected.txt` (pinned baseline; update via `--update`)

- Integration:
  - Add `enc_quality_check.sh` to `scripts/run_all.sh` under Encoder gates.

All temp files should follow the existing convention: `build/test-artifacts/<script>/`.

---

## Step 1 — PSNR gate (C tool)

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

### Acceptance criteria for Step 1

- `make test` includes the new PSNR gate. (Done: `scripts/run_all.sh` runs `scripts/enc_quality_check.sh`.)
- The gate is stable and reproducible. (Done: metrics are integer SSE → deterministic PSNR; guardrail is `0.05 dB`.)
- A baseline `scripts/enc_quality_expected.txt` exists. (Done; update with `./scripts/enc_quality_check.sh --update`.)

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

### Acceptance criteria for Step 2

- `make test` enforces SSIM gate.
- SSIM implementation parameters are documented and stable.

---

## Rollout / sequencing

1) Add the C tool with PSNR only.
2) Add the scripts and wire into `scripts/run_all.sh`.
3) Generate baseline expected metrics and pin them.
4) Extend the tool to compute SSIM and update baseline.

---

## Questions (to lock requirements)

I can implement this without further documentation, but I need a few decisions to avoid churn:

1) Should metrics be computed on **RGB** (all channels) or **luma-only**?
   - Suggestion: PSNR on RGB + SSIM on luma.

2) What input corpus should the quality gate use?
   - Reuse the existing encoder corpus list (if it’s stable), or define `scripts/enc_quality_corpus.txt`.

3) Is it acceptable for the metrics tool to link `-lm` (for `log10`), since it’s a standalone tool and doesn’t affect encoder parity?

4) Any preferred default encode settings for the gate (e.g. `--q 75 --mode bpred` to match parity), or do you want to test multiple Qs/modes?
