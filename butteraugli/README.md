# Butteraugli experiment (local)

This folder is a self-contained experiment runner to compare WebP encodes against a **reference PNG** using `butteraugli_nolibc_png`.

It will:

1. Convert `images/05924.jpg` → a reference `out/reference.png` (optionally downscaled).
2. Encode reference PNG to WebP using:
   - this repo’s `./encoder`
   - libwebp’s `cwebp` (optional)
3. Decode WebP back to PNG using this repo’s `./decoder`.
4. Score each decoded PNG vs reference using `./butteraugli_nolibc_png`.

## Run

From repo root:

```sh
./butteraugli/run_experiment.sh
```

Defaults:

- Sweeps `-q` from `0..100` for both `encoder` and `cwebp`
- Downscales input to `--max-dim 512`

Common knobs:

```sh
# Downscale to max 512px, sweep qualities
./butteraugli/run_experiment.sh --max-dim 512 --qualities 30,50,70,80,90,95

# Only our encoder, multiple modes
./butteraugli/run_experiment.sh --skip-cwebp --our-modes bpred-rdo,i16,dc

# Point at libwebp tools
LIBWEBP_BIN_DIR=~/libwebp/examples ./butteraugli/run_experiment.sh
```

Outputs:

- `out/reference.png`
- `out/webp/*.webp`
- `out/png/*.png`
- `out/results/results.csv` (raw)
- `out/results/results.sorted.csv` (best first)
- `out/results/summary.txt`
- `out/results/plot.svg` (x: WebP bytes, y: Butteraugli)
