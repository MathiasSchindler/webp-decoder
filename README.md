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

## Usage

```sh
./build/decoder -info input.webp

# Raw I420 (Y plane then U then V)
./build/decoder -yuv  input.webp out.i420   # unfiltered
./build/decoder -yuvf input.webp out.i420   # filtered (loop filter enabled)

# RGB outputs
./build/decoder -ppm input.webp out.ppm
./build/decoder -png input.webp out.png
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
