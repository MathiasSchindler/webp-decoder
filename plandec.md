# Decoder plan / status (WebP VP8)

Date: 2026-01-08

This file is a *living* status + roadmap for this repository’s standalone WebP (lossy VP8) decoder.

It focuses on two things:

- A brief summary of what’s already implemented and verified.
- A shortlist of sensible future directions (features + robustness) based on the RFCs and practical decoder work.

## What’s done (brief)

Core VP8/WebP decoding (lossy still images):

- RFC 9649 RIFF/WebP parsing for simple lossy files (`RIFF`/`WEBP` + one `VP8 ` chunk)
- RFC 6386 VP8 key-frame decode pipeline:
  - Frame header parsing (key frame)
  - Boolean entropy decoder
  - Macroblock token decode (currently assumes token partitions = 1)
  - Inverse transforms + intra prediction + reconstruction to I420
  - In-loop deblocking filter
- Output formats:
  - Raw I420 (`-yuv` unfiltered, `-yuvf` filtered)
  - RGB conversion + PPM output (`-ppm`) matching libwebp
  - Built-in PNG output (`-png`) using a minimal PNG writer (RGB8, filter=0, zlib stored blocks)

Verification (oracle-based):

- Header/container parity vs `webpinfo`
- YUV parity vs `dwebp -yuv` / `dwebp -yuv -nofilter` across the corpus
- PPM parity vs `dwebp -ppm` across the corpus
- PNG correctness validated by decoding PNG and comparing bytes to already-oracle-validated RGB (PPM path)

Engineering / build system milestones:

- Repo hygiene for publication (README, license, ignore rules)
- Multiple build targets:
  - `make` builds the normal libc tool (`decoder`)
  - `make nolibc` builds a static Linux x86_64 syscall-only variant (`decoder_nolibc`)
  - `make nolibc_tiny` builds a smaller YUV-only syscall build (`decoder_nolibc_tiny`)
  - `make ultra` builds a very small PNG-by-default syscall build (`decoder_nolibc_ultra`)

## Current scope and known limitations

- Container scope: “simple lossy” WebP only (no `VP8X`, `ALPH`, `ANIM`/`ANMF`, `VP8L`).
- VP8 scope: key frames only (no inter frames).
- Token partitions: coefficient token decoding currently supports `Total partitions: 1` only.

## Future work / roadmap ideas

This is intentionally a grab bag of good next steps; you can pick items based on goals (feature completeness vs hardening vs size/perf).

### 1) Token partitions > 1 (VP8)

Goal: support bitstreams with `Total partitions` in {2, 4, 8}.

- Implement multi-partition token stream dispatch per RFC 6386 (partition size table already exists).
- Add/curate test vectors that actually exercise >1 partitions, then add an oracle-backed script.

Why it matters: real encoders can emit multiple token partitions; without them the decoder will reject valid VP8.

### 2) Extended WebP container (`VP8X`) and metadata

Goal: parse (and optionally expose) additional chunks.

- Add `VP8X` parsing (canvas size, feature flags).
- Add safe skipping/passthrough parsing for:
  - `ICCP`, `EXIF`, `XMP` (metadata)
  - future-proof chunk iteration and offset validation

Notes:
- Even if you don’t “use” metadata, supporting `VP8X` makes many real-world WebPs parseable.

### 3) Alpha (`ALPH`) for lossy WebP

Goal: support lossy+alpha images.

- Parse and decode `ALPH` chunk and composite/write RGBA.
- Decide output behavior:
  - PNG RGBA output (most natural)
  - or write RGB only + ignore alpha behind a flag

### 4) Lossless WebP (`VP8L`)

Goal: support the VP8L bitstream (lossless).

- Add `VP8L` parsing and decoding (different entropy coding and transforms).
- Add PNG output path for lossless too.

### 5) Animation (`ANIM`/`ANMF`)

Goal: support animated WebP.

- Parse animation chunks.
- Frame composition rules (blend/dispose).
- Output strategy:
  - write frame sequence to PNG files
  - or emit a simple raw frame stream

### 6) Inter frames (full VP8)

Goal: support VP8’s inter-frame prediction.

- Motion vectors, reference frames, loopfilter interactions.
- This is a larger effort than “keyframes only” and will likely need new internal dataflow + more test coverage.

### 7) Robustness and security hardening

Goal: safely handle untrusted input.

- Strict bounds checks and integer overflow checks everywhere parsing touches sizes/offsets.
- Add a curated “corrupt corpus” (truncation, size mismatches, invalid tags).
- Add fuzzing harnesses (even a simple file mutator + crash checker is useful).
- Add resource limits (max image size, max allocations, max recursion/loops).

### 8) Determinism and debugging

Goal: make parity work and regressions easy.

- Add stable trace modes for parsing/entropy decode (versioned output).
- Add a small self-test mode.
- Keep scripts that compare against libwebp outputs as the ground truth.

### 9) Performance + size (optional, depending on goals)

Goal: choose one axis and optimize intentionally.

- Performance:
  - profile hot paths (IDCT/prediction/loopfilter)
  - consider carefully chosen inlining/loop unrolling, while keeping correctness
- Size:
  - further prune syscall-only glue, error messages, and optional features
  - maintain a “known-good PNG hash” regression check

### 10) Portability

Goal: run in more environments.

- Extend the syscall-only build beyond Linux x86_64 (or add a portable freestanding layer).
- Keep the normal libc build as the reference (easiest to debug and profile).

---

## Oracle tools

This repo’s development style is “oracle-driven”. Typical comparisons use libwebp’s tools:

- `webpinfo` for container/bitstream fields
- `dwebp` for pixel-exact output comparisons

(See the scripts under `scripts/` for the current comparison workflow.)
