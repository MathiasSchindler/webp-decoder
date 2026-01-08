# WebP (Lossy) Decoder — Incremental Implementation Plan (C, syscalls-only)

Date: 2026-01-08

This plan is designed to avoid “spec drift” by validating every step against:

- The container/format RFC: `rfc9649.txt` (WebP container)
- The VP8 bitstream RFC: `rfc6386.txt` (VP8 intra decoding)
- Google’s reference tools (oracle): `cwebp` and `dwebp`
- The repo’s golden outputs in `images/png-out/`

The key idea: **each increment produces a measurable artifact** (parsed structure, deterministic dumps, decoded pixels, checksums) and a **test** that must pass before moving on.

---

## Milestone Status

| # | Milestone | Status | Notes / verification |
|---:|---|---|---|
| 1 | Container parsing (RFC 9649) | Done | `scripts/m1_compare_info_with_webpinfo.sh` |
| 2 | VP8 frame header parse (key-frame header) | Done | `scripts/m2_compare_vp8hdr_with_webpinfo.sh` |
| 3 | Bit readers + boolean entropy decoder | Done | `scripts/m3_compare_framehdr_basic_with_webpinfo.sh` (header-level fields depend on bool decoder) |
| 4 | VP8 frame header (incl. token partition size table support) | Done | `scripts/m4_compare_all_partitions_with_webpinfo.sh` (currently all samples have `Total partitions: 1`) |
| 5 | Macroblock syntax decode (tokens) | Done | `scripts/m5_coeff_hash_smoke.sh` + `scripts/m5_compare_decode_ok_with_dwebp.sh` + `scripts/m5_scan_outliers.sh` (deterministic coefficient hash + decode-ok vs oracle + outlier scan; partition=1 only for now) |
| 6 | Inverse transforms + prediction + reconstruction (YUV pixels) | Done | `scripts/m6_compare_yuv_with_dwebp.sh` (byte-identical I420 vs no-loopfilter oracle across corpus) |
| 7 | Loop filter | Done | `scripts/m7_compare_yuv_filtered_with_oracle.sh` (byte-identical I420 vs filtered oracle across corpus) |
| 8 | Y’CbCr → RGB + PPM output | Done | `scripts/m8_compare_ppm_with_dwebp.sh` (byte-identical PPM vs `dwebp -ppm` across corpus) |
| 9 | Robustness + hardening | Not started | Corruption tests + strict bounds |

### Verification status (current confidence)

As of 2026-01-08, Milestones **M1–M8** are validated by the repo scripts against libwebp’s tools:

- Header/container parity vs `webpinfo`: `scripts/m1_...` through `scripts/m4_...`
- Token decode sanity + decode-ok vs `dwebp`: `scripts/m5_...`
- Pixel parity (I420):
   - Unfiltered output (`-yuv`) matches `dwebp -yuv -nofilter`: `scripts/m6_compare_yuv_with_dwebp.sh`
   - Filtered output (`-yuvf`) matches default `dwebp -yuv`: `scripts/m7_compare_yuv_filtered_with_oracle.sh`
- Pixel parity (PPM):
   - RGB output (`-ppm`) matches `dwebp -ppm`: `scripts/m8_compare_ppm_with_dwebp.sh`

Scope note: this confidence statement applies to the current supported scope (simple lossy WebP with a single `VP8 ` chunk, key frames only, and token partitions=1 for coefficient data).

## 0) Ground Rules / Scope

Initial target (keep it narrow to make progress):

- Decode **simple lossy WebP** only: RIFF `WEBP` + a single `VP8 ` chunk.
  - RFC 9649: Section 2.4 (header) and 2.5 (simple lossy)
- Decode **VP8 key frames only** (which is what lossy still WebP uses).
  - RFC 6386: focus on the intra-frame path.
- No alpha (`ALPH`), no extended container (`VP8X`), no animation.
- Output format for *your* decoder: recommend **PPM (P6)** or **raw planar YUV**.
  - PPM is easy to write with syscalls, no external libraries.

Code organization (to keep attempts separated):

- All implementation lives under `src/`.
- Each milestone gets its own subdirectory (see `src/README.md`).
- Shared low-level code (syscall I/O, endian helpers, bitreaders) goes in `src/common/`.

Out of scope for the *first milestone* (can be added later once the core works):
- `VP8X` extended container, `ALPH`, `ICCP`, `EXIF`, `XMP`, animation (`ANIM`/`ANMF`)
- Lossless `VP8L`

---

## Oracle tools (must be referenced by absolute + relative path)

These exist on your machine in at least two usable locations:

- Relative to this workspace (from `/home/mathias/webp-stuff/decoder`):
  - `../../libwebp/examples/cwebp`
  - `../../libwebp/examples/dwebp`
   - `../../libwebp/examples/webpinfo`
- In your home directory:
  - `/home/mathias/libwebp/examples/cwebp`
  - `/home/mathias/libwebp/examples/dwebp`
   - `/home/mathias/libwebp/examples/webpinfo`

Use `../../libwebp/examples/dwebp` in commands below unless you prefer the absolute home path.

Oracle availability note:

- In environments where `dwebp` isn’t available on PATH (or not built), we can still validate Milestone 6 behavior using an ffmpeg oracle for *no-loopfilter* output.
- The repo script `scripts/m6_compare_yuv_with_dwebp.sh` prefers `dwebp -yuv -nofilter` when available, and otherwise falls back to `ffmpeg -skip_loop_filter all` producing `yuv420p` raw I420.

Filtered oracle note (Milestone 7):

- The repo script `scripts/m7_compare_yuv_filtered_with_oracle.sh` prefers `dwebp -yuv` (default filtering) when available.
- If `dwebp` isn’t available, it falls back to `ffmpeg` without `-skip_loop_filter`, producing `yuv420p` raw I420.

`webpinfo` is especially useful early on because it prints the parsed RIFF/chunk structure and (optionally) bitstream header fields without decoding pixels.

Quick oracle sanity check (already confirmed working):

```sh
../../libwebp/examples/dwebp images/webp/solid_rgb_16x16_017_034_051_q050.webp -o /tmp/out.png
sha256sum /tmp/out.png images/png-out/solid_rgb_16x16_017_034_051_q050.png
```

---

## Test Data Layout (repo contract)

- `images/png-in/` : source PNGs used to create WebPs (inputs to `cwebp`).
- `images/webp/`   : known-good WebPs (mostly `*_q010.webp`, `*_q050.webp`, `*_q090.webp`).
- `images/png-out/`: oracle decode outputs (what Google `dwebp` produces).

For deterministic comparison without writing a PNG encoder yourself, prefer:
- Use `dwebp -ppm` to generate PPM from WebP.
- Have your decoder also output PPM.
- Then compare `sha256sum` of the PPM files.

Example oracle command (PPM):

```sh
../../libwebp/examples/dwebp images/webp/<file>.webp -ppm -o /tmp/oracle.ppm
sha256sum /tmp/oracle.ppm
```

---

## Milestone 1: Container parsing (RFC 9649) — no VP8 decode yet

### Step 1.1 — Read file + parse RIFF/WebP header
Implement:
- Read file bytes (syscalls only: `open`, `read`, `lseek`, `close`, `mmap` optional).
- Parse:
  - `RIFF` FourCC
  - file size (little-endian uint32)
  - `WEBP` FourCC

RFC mapping:
- `rfc9649.txt` Section 2.4

Tests:
1) For each `images/webp/*.webp`, assert:
   - first FourCC is `RIFF`
   - format is `WEBP`
   - RIFF “file size” matches actual file size (allow trailing bytes per RFC: MAY ignore, but in tests require exact match for repo files).
2) Print a one-line summary:
   - filename, riff_size, actual_size

Oracle cross-check (structure):

```sh
../../libwebp/examples/webpinfo images/webp/<file>.webp
```

### Step 1.2 — Parse chunk headers and locate exactly one `VP8 ` chunk
Implement:
- Loop over chunks: read FourCC + chunk size + payload (skip or index).
- Enforce “simple lossy layout” for now:
  - exactly one top-level chunk: `VP8 `
  - reject `VP8X`, `ALPH`, `ANIM`, `ANMF`, `VP8L` in milestone 1
- Handle padding to even sizes.

RFC mapping:
- `rfc9649.txt` Sections 2.3 (chunk basics) and 2.5 (`VP8 ` chunk)

Tests:
1) For each `images/webp/*.webp`, assert:
   - chunk list contains `VP8 `
   - no other chunks exist (given your repo appears to be “simple lossy”; if any file violates this, record it and decide whether to support it earlier)
2) Dump a machine-readable manifest:
   - `out/manifests/<file>.chunks.txt` containing FourCC, size, offset

Oracle cross-check (chunk offsets + canvas size):

```sh
../../libwebp/examples/webpinfo images/webp/<file>.webp
```

---

## Milestone 2: VP8 frame header parse (RFC 6386) — still no full decode

### Step 2.1 — Parse VP8 uncompressed data chunk (frame tag)
Implement:
- Inside the `VP8 ` payload: parse the VP8 frame tag
  - key-frame bit
  - version
  - show_frame
  - first partition length

RFC mapping:
- `rfc6386.txt` Section 9.1 (uncompressed data chunk)
- Also read Section 5 (overview) to guide order

Tests:
1) Assert all repo files are key frames (lossy still images should be intra): key-frame flag set.
2) Assert first partition length is within payload bounds.
3) Log parsed fields; compare against `dwebp -v` output if useful.

### Step 2.2 — Parse key-frame start code + width/height
Implement:
- Parse key-frame start code bytes.
- Parse width/height fields + scaling bits.
- Validate against expectations:
  - 16x16 for your current dataset.

RFC mapping:
- `rfc6386.txt` Section 9.1 + key-frame header details
- Note from `rfc9649.txt` Section 2.5: canvas size assumed from VP8 frame header

Tests:
1) For each WebP, assert decoded width/height equals 16x16.
2) Cross-check with oracle:
   - `../../libwebp/examples/dwebp <file> -v` prints dimensions; ensure your parser matches.

Additional oracle cross-check (bitstream header parse only):

```sh
../../libwebp/examples/webpinfo -bitstream_info images/webp/<file>.webp
```

---

## Milestone 3: Bit readers and boolean entropy decoder (VP8 core primitive)

### Step 3.1 — Safe bitreader
Implement:
- A bounded bitreader abstraction for reading bytes/bits from a buffer.
- Hard fail on out-of-bounds reads.

Tests:
- Unit tests that feed known byte patterns and assert bit extraction sequences.

### Step 3.2 — VP8 boolean entropy decoder
Implement:
- The boolean arithmetic decoder exactly per RFC.

RFC mapping:
- `rfc6386.txt` Section 7 (Boolean Entropy Decoder)

Tests:
1) Unit tests from hand-constructed streams (tiny synthetic cases).
2) Cross-check by implementing a “trace mode” that prints the first N decoded bits and compare with a tiny reference trace you generate once (from your own implementation + locked expected output).

---

## Milestone 4: Parse VP8 frame header fully (no pixel reconstruction yet)

### Step 4.1 — Frame header fields
Implement parsing for:
- segmentation (Section 9.3)
- loop filter params (Section 9.4)
- token partitions / partition sizes (Section 9.5)
- quant indices (Section 9.6)
- probability updates (Section 9.9)

Tests:
1) Structural validation only:
   - partition boundaries do not exceed payload
   - number of partitions consistent
2) Produce a deterministic header dump:
   - `out/headers/<file>.json` (or line-based text) with all parsed fields
3) Cross-check behavior changes using oracle flags:
   - decode with `dwebp -nofilter` and verify (later) your `-nofilter` matches pixel output.

Note on token partitions (RFC 6386 Section 9.5):

- The current `images/webp/*.webp` and `images/testimages/webp/*.webp` corpora report `Total partitions: 1` for all files (per `webpinfo -bitstream_info`).
- Decision (for now): treat multi-partition coefficient streams as a future feature. “Normal” images appear to use a single token partition, so we won’t spend time generating special test vectors just to hit `Total partitions > 1`.
- We still keep the parsing support for the partition size table so we can enable it later, but a *meaningful* oracle comparison needs at least one sample with `Total partitions > 1`.

Future milestone idea:

- Add at least one WebP with `Total partitions > 1` (generated intentionally), and extend our oracle scripts to validate `Part. 1..N-1 length:` lines and bounds checks.

---

## Milestone 5: Macroblock syntax decode (tokens) — produce coefficient blocks

### Current constraints / status notes (important)

- **Token partitions = 1 only (for now).** The decoder currently supports `Total partitions: 1` for coefficient token data. If a bitstream uses 2/4/8 token partitions, Milestone 5 returns `ENOTSUP`.
   - Rationale: the existing corpora here all report `Total partitions: 1`, and implementing multi-partition dispatch correctly is easy to get subtly wrong without a proper oracle-backed corpus.
   - Implication: we keep the size-table parsing (Milestone 4), but Milestone 5’s coefficient decode path is intentionally single-partition until we have test vectors.

- **Bool-decoder “overread” exists in real files (token partition).** Some valid WebPs require the boolean decoder to conceptually consume bits past the declared end of the token partition.
   - We treat this as **implicit zero-extension** (memory-safe, bounded) and record it in `decoder -info` as `Token overread: Yes/No` and `Token overread b: <N>`.
   - The outlier scan (`scripts/m5_scan_outliers.sh`) shows these cases; in practice they correlate with `token_slack=0` (exact consumption) and then refills needing more bits.
   - There is a probe tool to reduce “is this misparse?” doubt: `./decoder -probe <file.webp>` compares baseline vs `0x00`-padded and `0xFF`-padded payloads. Overread cases typically show baseline == `0x00` padding, and diverge with `0xFF` padding.

- **Milestone 5 now stores decoded outputs for Milestone 6.** The decoder can decode and store per-macroblock segment id, modes, and full coefficient blocks (see the `-dump_mb` debugging command).
- **Milestone 5 stores decoded outputs for Milestone 6.** The decoder stores per-macroblock segment id, modes, and full coefficient blocks (see `-dump_mb`).
- **Debugging helpers exist for parity work.** `-diff_mb <file.webp> <oracle.i420>` aggregates per-segment SAD (Y/U/V) to correlate mismatches with segmentation.

Correctness notes discovered while reaching M6 parity:

- The luma coefficient “above/left nonzero” context must propagate consistently across macroblocks regardless of intra mode (including B_PRED vs non-B_PRED); otherwise token decoding drifts badly on segmentation-heavy images.
- Some real files require boolean decoder “overread” beyond the token partition end; treating the virtual extension bytes as zeros is compatible with oracle output.

### Step 5.1 — Tree coding helpers
Implement:
- The token tree decoding logic

RFC mapping:
- `rfc6386.txt` Section 8 (tree coding)

Tests:
- Unit tests for tree walk decoding.

### Step 5.2 — DCT coefficient decoding into block arrays
Implement:
- Coefficient token decode per macroblock and per block type

RFC mapping:
- `rfc6386.txt` Section 13

Tests:
1) For a small subset of images, dump coefficients for the first few macroblocks.
2) Add invariants:
   - EOB handling correct
   - coefficient ranges reasonable

(At this stage you still won’t have pixels, but you will know the bitstream is being consumed correctly.)

---

## Milestone 6: Inverse transforms + prediction + reconstruction (pixels in Y’CbCr)

### Step 6.1 — Inverse WHT + inverse DCT
Implement:
- Exact integer transforms (no drift)

RFC mapping:
- `rfc6386.txt` Section 14

Tests:
- Unit tests for inverse transforms on known vectors.

### Step 6.2 — Intra prediction modes + add residue
Implement:
- Luma and chroma prediction modes
- Reconstruction: predictor + residual

RFC mapping:
- `rfc6386.txt` Sections 11–12 and 14.5

Tests:
1) Produce a raw YUV output (`-yuv`) and compare to a *no-loopfilter* oracle output:
   - Preferred: `dwebp -yuv -nofilter`
   - Fallback: `ffmpeg -skip_loop_filter all` to `yuv420p` raw I420
   - This avoids both RGB conversion differences and loopfilter differences while you’re still stabilizing.
2) Start with a single image (e.g. a solid color) and then expand to gradients/checkers.

Implementation note (important for edge correctness):

- Reconstruct into a macroblock-aligned padded buffer (`mb_cols*16` x `mb_rows*16`) and crop to the visible width/height when writing the final I420.

---

## Milestone 7: Loop filter (must match oracle when enabled/disabled)

### Step 7.1 — Implement in-loop deblocking filter
Implement:
- Simple/normal filter path + parameter calculation

RFC mapping:
- `rfc6386.txt` Section 15

Tests:
1) Ensure your unfiltered YUV output matches oracle `dwebp -yuv -nofilter`:
   - `scripts/m6_compare_yuv_with_dwebp.sh`
2) Ensure your filtered YUV output matches oracle `dwebp -yuv`:
   - `scripts/m7_compare_yuv_filtered_with_oracle.sh`

---

## Milestone 8: Y’CbCr -> RGB and final PPM output

### Step 8.1 — Color conversion
Implement:
- Y’CbCr (BT.601) to RGB conversion + 4:2:0 chroma upsampling.
- For byte-identical parity with libwebp, match libwebp’s fixed-point VP8 YUV->RGB conversion and “fancy” upsampling behavior.

RFC mapping:
- `rfc9649.txt` Section 2.5 recommends Rec.601

Tests:
1) Compare your PPM output to oracle PPM output (single file):

```sh
../../libwebp/examples/dwebp images/webp/<file>.webp -ppm -o /tmp/oracle.ppm
./your_decoder images/webp/<file>.webp -ppm -o /tmp/mine.ppm
sha256sum /tmp/oracle.ppm /tmp/mine.ppm
```

2) Run the entire corpus and require all hashes match.

Repo script:

```sh
scripts/m8_compare_ppm_with_dwebp.sh
```

---

## Milestone 9: Robustness, hardening, and “syscalls-only” compliance

### Step 9.1 — Defensive parsing + limits
Add:
- size limits (max width/height, max partitions)
- integer overflow checks
- graceful errors

Tests:
- Corrupt a few files (truncate, flip chunk sizes) and verify decoder errors without OOB reads.

### Step 9.2 — Determinism + reproducibility
Add:
- a `--trace` mode that prints stable, versioned traces for debugging
- a `--selftest` mode that runs a small embedded unit-test suite

---

## Continuous verification scripts (recommended)

Repository conventions (please keep following in all future milestones):

- Put verification scripts in `scripts/`.
- Name scripts with an `m<N>_...` prefix matching the milestone (e.g. `m1_...`, `m2_...`).
- Document every script in `scripts/README.md` with what it checks and which milestone it belongs to.

Create simple shell scripts (no dependencies beyond POSIX tools) to keep yourself honest:

1) `scripts/oracle_decode_all_ppm.sh`
   - loops over `images/webp/*.webp`
   - writes oracle PPMs via `dwebp -ppm`
   - stores `sha256sum` manifest

2) `scripts/compare_my_ppm_to_oracle.sh`
   - runs your decoder
   - compares hashes

3) `scripts/oracle_roundtrip_sanity.sh`
   - (optional) re-encode `images/png-in/*.png` with `cwebp -q {10,50,90}`
   - decode with `dwebp` and ensure it matches your own expectations

---

## Notes / Recommendations (to avoid common WebP/VP8 pitfalls)

- Start by matching **YUV output** against `dwebp -yuv` before you even think about RGB.
- Keep both unfiltered (`-yuv`) and filtered (`-yuvf`) outputs available; it makes parity debugging much easier.
- Keep all arithmetic **integer**, exactly as VP8 requires (avoid float).
- Add strict bounds checks everywhere; a decoder is a parser for untrusted input.
- Resist adding container features early; your dataset suggests simple lossy only.

## Known limitations / concerns

- Token partitions: still limited to `Total partitions: 1` (multi-token-partition frames return `ENOTSUP`).
- Frame types: key frames only (inter frames not supported yet).
- Loopfilter deltas: ref/mode delta arrays are parsed and stored; full generality for inter frames will require applying the correct ref/mode indices from the per-MB prediction state.

---

## Suggested first command-line contract for your decoder

Keep it minimal but test-friendly:

- `./decoder -info in.webp` (prints RIFF/chunk + VP8 summary)
- `./decoder -yuv in.webp out.i420` (unfiltered I420; matches Milestone 6 / `dwebp -yuv -nofilter`)
- `./decoder -yuvf in.webp out.i420` (filtered I420; matches Milestone 7 / `dwebp -yuv`)
- `./decoder -ppm in.webp out.ppm` (PPM; matches Milestone 8 / `dwebp -ppm`)
- `./decoder -png in.webp out.png` (PNG; internal minimal writer, RGB)
- `./decoder -diff_mb in.webp oracle.i420` (macroblock-level diff helper)
- `./decoder -probe in.webp` (overread-sensitivity probe)
- `./decoder -dump_mb in.webp [mb_index]` (MB syntax dump)
