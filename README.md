# Standalone WebP (VP8) decoder (C11)

This project is a from-scratch **lossy WebP** decoder (VP8 intra) written in **portable C11**, with a strong focus on being easy to audit and validate.

It aims to be a small, self-contained reference implementation you can step through while reading the specs:

- [rfc9649.txt](rfc9649.txt) — WebP container
- [rfc6386.txt](rfc6386.txt) — VP8 bitstream

The implementation is developed in milestones and repeatedly compared against **libwebp**’s tools (`dwebp`, `webpinfo`) for bit-exact output.

## What it does

Given a simple lossy `.webp` file, the decoder can:

- Parse the RIFF/WebP container and VP8 headers (`-info`)
- Decode VP8 key frames into YUV (I420)
  - Unfiltered output (`-yuv`) intended to match `dwebp -yuv -nofilter`
  - Filtered output (`-yuvf`) intended to match default `dwebp -yuv`
- Convert to RGB using libwebp-compatible fixed-point math + fancy upsampling
  - PPM output (`-ppm`) intended to match `dwebp -ppm`
  - PNG output (`-png`) via a minimal built-in PNG writer (RGB8, filter=0, zlib stored blocks)

## What it does *not* try to do (yet)

Scope is intentionally narrow.

- Container features: no `VP8X`, `ALPH`, `ANIM`/`ANMF`, `VP8L`
- VP8 features: key frames only (no inter frames)
- Token partitions: currently expects `Total partitions: 1` for coefficient data

See [plandec.md](plandec.md) for current status and verification notes.

## Build

Requirements: a reasonably recent C toolchain on Linux x86_64.

```sh
make
```

This produces two binaries:

- `build/decoder`
- `build/encoder`

The default build is syscall-only/nolibc. No root-level `decoder` or `encoder`
binary is produced.

The default build uses all decoder speed optimizations enabled in this
Makefile: `build/decoder` is built with `-O3 -march=native`. The encoder
remains size-oriented and portable across x86_64 machines: `-Os -march=x86-64`.

To force a portable decoder speed build, disable CPU-specific code generation:

```sh
make clean && make NATIVE=0
```

To force the older size-oriented portable decoder build:

```sh
make clean && make SPEED=0 NATIVE=0
```

`SPEED=0` switches only `build/decoder` back to `-Os -march=x86-64`.
The encoder remains built with `-Os -march=x86-64` in all cases.

## Benchmarking decoder speed

For per-file WebP-to-PNG timing against `dwebp`, build the decoder variant you
want to measure and run:

```sh
RUNS=5 scripts/benchmark_decode_png_csv.sh build/test-artifacts/bench-png.csv
```

The benchmark script scans `images/**/*.webp`, records the best of `RUNS` for
each file, and writes CSV output. It expects libwebp's `dwebp` at
`$HOME/libwebp/examples/dwebp` unless `DWEBP=/path/to/dwebp` is set.

For whole-corpus timings across all decoder modes:

```sh
RUNS=5 scripts/benchmark_decoder_modes.py build/test-artifacts/benchmark-modes
```

This writes `decoder_modes_summary.csv` and `decoder_modes_times.csv` into the
chosen output directory.

Final local whole-corpus timing for the 2026-06 decoder optimization pass:

- Host: `mathias-b650`, Linux 7.0.0-22-generic x86_64
- Compiler: `cc (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- Corpus: all 433 lossy WebP files under `images/**/*.webp`, 35.987 MP total
- Build: default nolibc decoder, `-O3 -march=native`
- Method: median of five end-to-end whole-corpus runs per mode; output files
  overwritten under each benchmark artifact directory
- Baseline artifact: `build/profile/baseline-81c640b-20260612T1039/`
- Final artifact: `build/profile/final-speed-validation-docs-2-20260612T105703/`
- Optimized areas: bool/token/tree decode, loopfilter, reconstruction/IDCT,
  YUV-to-RGB/PPM/PNG output, and nolibc memory syscalls

| Mode | Baseline | Final | Throughput gain |
| --- | ---: | ---: | ---: |
| `-info` | 0.511921 s / 70.30 MP/s | 0.472928 s / 76.09 MP/s | +8.2% |
| `-yuv` | 0.674262 s / 53.37 MP/s | 0.512736 s / 70.19 MP/s | +31.5% |
| `-yuvf` | 0.841425 s / 42.77 MP/s | 0.658468 s / 54.65 MP/s | +27.8% |
| `-ppm` | 1.028450 s / 34.99 MP/s | 0.846114 s / 42.53 MP/s | +21.6% |
| `-png` | 1.204730 s / 29.87 MP/s | 1.006997 s / 35.74 MP/s | +19.7% |

Commands used for the final run:

```sh
make clean && make
mkdir -p build/test-artifacts/_tmp
TMPDIR=$PWD/build/test-artifacts/_tmp make test

# Extra decoder byte-exact gates:
TMPDIR=$PWD/build/test-artifacts/_tmp ./scripts/m6_compare_yuv_with_dwebp.sh
TMPDIR=$PWD/build/test-artifacts/_tmp ./scripts/m7_compare_yuv_filtered_with_oracle.sh
TMPDIR=$PWD/build/test-artifacts/_tmp ./scripts/m8_compare_ppm_with_dwebp.sh
TMPDIR=$PWD/build/test-artifacts/_tmp ./scripts/m8_compare_png_with_ppm.sh

# Whole-corpus benchmark runner:
RUNS=5 scripts/benchmark_decoder_modes.py build/profile/final-speed-validation-docs-2-20260612T105703
```

`perf` was present, but `perf_event_paranoid=4` prevented useful hardware or
software event profiling; the artifact keeps the attempted `perf stat` output.

Current Commons stage/core profile after the fused decode/reconstruct,
SSE2 loopfilter, and wider YUV/RGB SIMD integration
(`build/profile/final-215mp-integration-20260612T1217/`, 2026-06-12):

- Corpus: 28 generated Commons WebPs, 378.031 MP per run; median of 3 runs.
- Our cumulative modes: `-info` 144.00 MP/s, `-yuv` 173.37 MP/s, `-yuvf`
  113.80 MP/s, `-ppm` 93.22 MP/s, `-png` 62.69 MP/s.
- System libwebp core helpers: YUV no-filter 326.12 MP/s, YUV filtered
  279.65 MP/s, RGB buffer 216.25 MP/s, RGB+PPM 214.95 MP/s.
- Comparative PPM: ffmpeg 76.78 MP/s, ImageMagick 101.59 MP/s.
- Derived deltas: our loopfilter 1.141 s, our RGB/PPM output 0.734 s,
  our PNG output 2.709 s; internal `-profile_stages` reports token decode at
  1.555 s, reconstruction at 1.029 s, derived loopfilter at 1.042 s,
  YUV-to-RGB formatting at 0.730 s, and PPM pixel writes at 0.025 s. Remaining
  distance to libwebp is mostly core VP8 decode/filter work plus RGB formatting;
  PPM writes are negligible on this run.

Caveats: these are local machine timings over the generated Commons artifact
set and include process startup, file reads, allocation, and decode work. The
libwebp comparisons use a generated helper linked against system libwebp rather
than local `../../libwebp/dwebp`; `-info` includes coefficient stats and is not
a pure header-only decode.

## Usage

```sh
./build/decoder -info input.webp

# Raw I420 (Y plane then U then V)
./build/decoder -yuv  input.webp out.i420   # unfiltered
./build/decoder -yuvf input.webp out.i420   # filtered (loop filter enabled)

# RGB outputs
./build/decoder -ppm input.webp out.ppm
./build/decoder -png input.webp out.png

# Internal profiling CSV for PPM-output decode stages
./build/decoder -profile_stages input.webp out.ppm
```

## Encoder (PNG -> WebP)

The repository also contains a from-scratch **lossy WebP (VP8 keyframe) encoder**.
It is intentionally not expected to match libwebp (`cwebp`) in quality/speed yet; the
primary goals are **spec-correctness**, **determinism**, and **incremental test-gated
progress**.

Basic usage:

```sh
# Encode PNG -> WebP (default: --mode bpred-rdo, --q 75)
./build/encoder input.png out.webp

# Explicit baseline mode (simple reference)
./build/encoder --mode bpred input.png out.webp

# Choose quality and intra mode
./build/encoder --q 90 --mode i16 input.png out.webp

# Opt-in: write loopfilter header params
./build/encoder --loopfilter --q 75 input.png out.webp

# Inspect using our decoder
./build/decoder -info out.webp
```

Notes:

- The YUV outputs are raw I420 with no container/header.
- The PNG path is meant as a convenient “no external libraries” output format; it is not tuned for compression ratio.

## Validation (how to know it’s correct)

Most milestones have an oracle-comparison script under [scripts/](scripts/).

Examples:

- Header/container checks vs `webpinfo`
- YUV byte-for-byte checks vs `dwebp -yuv` (filtered/unfiltered)
- PPM byte-for-byte checks vs `dwebp -ppm`
- PNG validation by comparing decoded PNG bytes to the already-validated PPM path:
  - [scripts/m8_compare_png_with_ppm.sh](scripts/m8_compare_png_with_ppm.sh)

These scripts assume you have libwebp’s tools available (commonly at `~/libwebp/examples/` as described in [plandec.md](plandec.md)).

If your libwebp tools live elsewhere, point the scripts at them via:

```sh
LIBWEBP_BIN_DIR=/path/to/libwebp/examples make test
```

For final decoder correctness gates, keep temporary files inside the repository:

```sh
make clean && make
mkdir -p build/test-artifacts/_tmp
TMPDIR=$PWD/build/test-artifacts/_tmp make test
```

These gates are byte-exact against libwebp for the supported decoder scope:
simple lossy VP8 key frames in the local corpus. They do not claim support for
the intentionally unsupported features listed above (`VP8X`, alpha, animation,
lossless `VP8L`, inter frames, or multi-token-partition VP8 streams).

To audit the current libwebp parity gap and local corpus coverage, run:

```sh
scripts/decoder_quality_parity_report.py
```

The decoder gates include all simple lossy WebPs under the local corpora,
including generated Commons files when `images/commons/generated-webp/` is
present. The parity report separates current format/quality gaps from later
speed work.

Encoder regression gates live alongside the decoder ones under [scripts/](scripts/) and
are named `enc_mXX_*.sh`. See [planenc.md](planenc.md) for the encoder milestone plan.

## Repo layout

- [src/](src/) — the decoder implementation
  - See [src/README.md](src/README.md) for the milestone/module breakdown
- [scripts/](scripts/) — verification helpers
  - See [scripts/README.md](scripts/README.md)
- [images/](images/) — local corpora and oracle outputs (often large)

By default, `build/` and `images/` are ignored via [.gitignore](.gitignore) (they tend to be machine-local and/or large).

## License

**CC0-1.0 (Public Domain dedication).** No rights reserved.

## Authorship note

This codebase is **mostly LLM-generated**, with human-directed iteration and extensive oracle-based testing.

Primary model used: **GPT-5.2**.
