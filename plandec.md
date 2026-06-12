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
- Pixel-output commands now use a fused decode+reconstruct path that streams
  macroblock coefficients and avoids frame-sized coefficient arrays.
- x86_64 builds include guarded SIMD helpers for loopfilter edges,
  reconstruction block add/clamp, and YUV-to-RGB formatting; `NATIVE=0` builds
  still compile with the baseline x86-64 ISA.

Verification (oracle-based):

- Header/container parity vs `webpinfo`
- YUV parity vs `dwebp -yuv` / `dwebp -yuv -nofilter` across the corpus
- PPM parity vs `dwebp -ppm` across the corpus
- PNG correctness validated by decoding PNG and comparing bytes to already-oracle-validated RGB (PPM path)

Engineering / build system status:

- Repo hygiene for publication (README, license, ignore rules).
- `make` builds static Linux x86_64 syscall-only `build/decoder` and
  `build/encoder` binaries. The default decoder is speed-oriented
  (`-O3 -march=native`); `make NATIVE=0 all` keeps the decoder on baseline
  `-march=x86-64` while still compiling the guarded SSE2 files.
- `make test` runs the decoder/encoder oracle gates; keep `TMPDIR` inside
  `build/test-artifacts/_tmp` for reproducible local validation.

## Current scope and known limitations

- Container scope: “simple lossy” WebP only (no `VP8X`, `ALPH`, `ANIM`/`ANMF`, `VP8L`).
- VP8 scope: key frames only (no inter frames).
- Token partitions: coefficient token decoding currently supports `Total partitions: 1` only.

## Libwebp parity matrix (decoder)

Snapshot command:

```sh
scripts/decoder_quality_parity_report.py
```

Local snapshot after broadening the decoder gates:

- Corpus: all 466 local WebPs under `images/**/*.webp`, including
  `images/commons/generated-webp/`.
- Container signatures: 466/466 are simple lossy `VP8 ` only.
- Decoder `-info`: 466/466 pass.
- Observed VP8 fields: key frame=yes for all files; profiles 0 (461 files)
  and 1 (5 files); color space=0, clamp=0, display=yes, x/y scale=0 for all
  files; segmentation on in 248 files; simple filter on in 5 files; total
  token partitions=1 for all files.

| Area | Current status | Validation state / next gate |
| --- | --- | --- |
| Simple RIFF/WebP + `VP8 ` still image | Supported when the file is exactly `RIFF`/`WEBP` with one `VP8 ` chunk and no extra chunks. | Header and pixel gates compare against `webpinfo`/`dwebp` across the simple-lossy corpus. |
| VP8 key frames | Supported. The decode path includes boolean entropy decode, intra prediction, inverse transforms, deblocking, fixed-point YUV→RGB, PPM, and RGB8 PNG output. | `m5` syntax smoke plus `m6`/`m7`/`m8` byte-exact gates. |
| Token partitions | Only `Total partitions: 1` is decoded. Valid streams with 2/4/8 token partitions remain unsupported. | Need generated/curated >1 partition vectors, then pixel parity gates. |
| VP8 profiles / segmentation / filters | Profiles 0 and 1 are covered by the local corpus; segmentation and both normal/simple filters are exercised. Profiles 2/3 are not yet explicitly covered. | Keep byte-exact gates; add explicit profile 2/3 vectors before claiming full VP8 profile parity. |
| `VP8X` extended container | Unsupported. The parser intentionally rejects extra chunks today. | Add a safe chunk iterator, VP8X canvas/flag validation, and negative fixtures. |
| Alpha (`ALPH`) | Unsupported. There is no alpha decode, compositing policy, or RGBA PNG output. | After VP8X, add lossy+alpha vectors and byte-exact RGBA output checks vs `dwebp -png`. |
| Lossless (`VP8L`) | Unsupported; this is a separate bitstream from VP8 lossy. | Separate VP8L milestone and PNG/RGBA parity gates. |
| Animation (`ANIM`/`ANMF`) and inter frames | Unsupported; no frame composition, disposal/blending, references, or VP8 inter-frame prediction. | Defer until still-image extended-container support is stable. |
| Metadata / color profile (`ICCP`, `EXIF`, `XMP`) | Unsupported as container chunks. Current RGB conversion matches libwebp for decoded YUV, but no color-management semantics are applied. | Parse/skip metadata safely; decide whether ICC profiles are exposed only or applied. |

### Prioritized parity roadmap

1. **Lock current-scope correctness first.** Keep all simple lossy VP8 corpora,
   including generated Commons WebPs, in the byte-exact YUV/PPM/PNG gates.
2. **Format parity before speed:** implement multi-token partitions, then VP8X
   chunk iteration/metadata skipping, then alpha output policy.
3. **Broader libwebp parity:** add VP8L lossless and animation only after
   still-image extended-container support is stable.
4. **Speed work:** continue benchmarking only with frozen format/pixel gates, so
   performance changes cannot hide parity regressions.

## Current speed snapshot

Latest local Commons stage/core profile:

- Command: `python3 scripts/profile_decode_stages.py --webp-glob 'images/commons/generated-webp/*.webp' --runs 5 --warmups 1 --out-dir build/profile/opt2-integrated-comparison-20260612T1420`
- Corpus: 28 generated Commons WebPs, 378.031 MP per run.
- Our cumulative throughput: `-yuv` 225.09 MP/s, `-yuvf` 186.26 MP/s,
  `-ppm` 167.50 MP/s, `-png` 146.15 MP/s.
- System libwebp core throughput: YUV no-filter 325.13 MP/s, YUV filtered
  282.71 MP/s, RGB buffer 214.57 MP/s, RGB+PPM 215.64 MP/s.
- Comparative PPM throughput: ffmpeg 76.55 MP/s, ImageMagick 101.16 MP/s.
- Remaining largest local costs: token decode (~1.273 s), reconstruction
  (~0.754 s), loopfilter (~0.311 s), RGB formatting (~0.235 s), and PNG
  output delta from filtered YUV (~0.557 s) over the 378 MP corpus. PPM pixel writes
  remain negligible at ~0.025 s.

Treat these as local-machine guideposts, not portable absolute performance
claims.

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

#### Reusable decoder context / allocation plan

The current CLI decodes one file per process call and intentionally keeps state
local to each command. That is simple, but in the nolibc build every `malloc`
maps a new anonymous region, so batch callers pay syscall overhead for each
scratch/output allocation.

Current allocation hotspots:

- input mapping: one read-only file mapping per input (`os_map_file_readonly`)
- VP8 syntax storage: per-frame macroblock metadata and coefficient arrays in
  `Vp8DecodedFrame`
- token decode scratch: macroblock prediction records plus above/left contexts
- reconstruction output: padded/cropped I420 images
- RGB/PNG output scratch: two RGB scanlines and the PNG IDAT buffer

Low-risk cleanup already done: `Yuv420Image` now stores Y/U/V planes in one
contiguous allocation while keeping the existing `y`, `u`, and `v` pointers.
This preserves CLI simplicity and cuts two nolibc allocation mappings for each
I420 image (3→1); reconstruction uses padded+cropped images, so the decode
paths that reconstruct pixels now use 2 image allocations instead of 6.

A libwebp-style reusable API should introduce an explicit context that owns
grow-only scratch buffers:

```c
typedef struct WebPDecoderContext WebPDecoderContext;

int webp_decoder_context_init(WebPDecoderContext* ctx);
void webp_decoder_context_reset(WebPDecoderContext* ctx);
void webp_decoder_context_free(WebPDecoderContext* ctx);

int webp_decode_to_yuv(WebPDecoderContext* ctx,
                       ByteSpan webp,
                       unsigned flags,
                       Yuv420Image* out_view);
int webp_decode_write_png(WebPDecoderContext* ctx, ByteSpan webp, int fd);
```

`reset` should clear per-frame metadata without freeing capacity; `free` should
release all backing storage. The first context-owned buffers to add are:

1. `Vp8DecodedFrame` backing arrays sized by macroblock capacity.
2. token scratch (`MbInfo`, above coefficient contexts, above b-modes).
3. padded/cropped `Yuv420Image` storage sized by image capacity.
4. RGB row and PNG IDAT scratch buffers.

Keep the CLI as a thin wrapper: single-file commands can create/free one context
per invocation, while future batch/lib callers can reuse a context across many
inputs. Add resource limits before exposing batch decode so malformed files
cannot force unbounded context growth.

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
