# Cleanup plan — duplicates, ultra drift, naming consistency

Date: 2026-01-09

This document is an implementation plan for three cleanup items:

1) duplicate/legacy milestone directories
2) behavior drift between normal encoder and `make ultra` encoder
3) binary naming consistency + docs/scripts alignment

For each item, the plan includes steps and regression/testing safeguards.

Status note (repo reality)

- The consistency work has landed: `make test` runs `scripts/run_all.sh`, oracle tools are resolved via `LIBWEBP_BIN_DIR`, and scripts standardize temp outputs under `build/test-artifacts/`.
- A binary smoke check exists as `scripts/smoke_binaries.sh`.

---

## C1 — Duplicate / legacy milestone directories in `src/`

Observation

- `src/` currently contains both canonical milestone dirs and older/alternate ones, e.g.
  - `m06_recon/` and `m06_recon_yuv/`
  - `m07_loopfilter/` and `m07_loop_filter/`
  - `m09_png/` and `m09_hardening/`

Goal

- Reduce confusion without breaking builds or scripts.

Status

- Phase A implemented: canonical dirs are described in `src/README.md`.
- Phase B implemented: unreferenced legacy milestone directories were deleted:
  - `src/m06_recon_yuv/`
  - `src/m07_loop_filter/`
  - `src/m09_hardening/`

Plan (safe two-phase)

Phase A — Make usage explicit (no deletes)

1. Inventory usage:
   - grep which paths are referenced by `Makefile`, `#include` directives, and scripts.
2. Decide canonical directories (likely the ones used by `Makefile` today).
3. Mark non-canonical dirs as legacy:
   - add a tiny `README.md` in each legacy dir (“Legacy; not built; kept for history”).
4. Ensure top-level docs point to the canonical path (already started in `src/README.md`).

Phase B — Remove or merge (optional, later)

5. If a legacy dir is truly unused:
  - delete it.
6. If it contains useful code:
  - merge it deliberately into the canonical module and update includes.

Testing / regressions

- Run `make clean && make`.
- Run all decoder scripts and encoder scripts.
- If any file moves:
  - ensure `webpinfo`/`dwebp` oracle scripts still locate the right binaries.

Acceptance criteria

- `Makefile` only references one canonical dir per milestone.
- Docs describe canonical dirs; legacy dirs clearly labeled or removed.

---

## C2 — Make `encoder_nolibc_ultra` behave identically to normal `encoder`

Observation

- Historically, the ultra encoder avoided libm and used approximations:
  - gamma tables were skipped (plain averaging)
  - fixed-point quality→qindex mapping differed from the libc/libm path

Goal

- Ultra encoder output should match normal encoder output for the same inputs and flags.
- Avoid platform-dependent runtime floating point differences.

Status

- Implemented: normal and ultra encoder now match byte-for-byte for a deterministic corpus, enforced by a regression gate.

Key design decision

- To make outputs identical **and** keep nolibc:
  - remove runtime `pow()` usage from the normal encoder too
  - replace it with **precomputed constant tables** committed to the repo

Plan

1) Replace runtime gamma tables with static tables

- Implemented as:
  - `src/enc-m04_yuv/enc_gamma_tables.c`
  - `src/enc-m04_yuv/enc_gamma_tables.h`
- Tables are committed and used by both normal and ultra builds (no runtime `pow()`).
- Generator tool lives in-tree as `tools/gen_enc_tables.c` (not used at runtime).

2) Replace quality→qindex mapping with a static lookup table

- Implemented as:
  - `src/enc-m06_quant/enc_quality_table.c`
  - `src/enc-m06_quant/enc_quality_table.h`
- Exported symbol: `enc_qindex_from_quality[101]`.
- Both normal and ultra builds use the table.

3) Remove `-lm` from the normal encoder link line (optional but recommended)

- Implemented: encoder-related link lines no longer use `-lm`.

4) Add a hard regression gate: normal vs ultra bit-identical

- Implemented:
  - `scripts/enc_ultra_parity_check.sh` compares output `.webp` files byte-for-byte (and prints SHA-256 on mismatch).
  - Wired into `scripts/run_all.sh`, so `make test` enforces parity.

Testing / regressions

- Run `make clean && make`.
- Run `make ultra`.
- Run `make test` (includes the parity check).

Acceptance criteria

- `encoder` and `encoder_nolibc_ultra` produce identical `.webp` output for the parity corpus.
- No libm dependency at runtime.

Notes

- A subtle ultra-vs-normal mismatch was fixed by making nolibc `memcpy()` overlap-safe (treating it as `memmove()`), ensuring deterministic behavior across builds.

---

## C3 — Naming consistency (binaries, docs, scripts)

Observation

- The repo produces multiple binaries:
  - `decoder`, `encoder`
  - `decoder_nolibc`, `decoder_nolibc_tiny`, `decoder_nolibc_ultra`
  - `encoder_nolibc_ultra`

Goal

- Make names predictable and ensure scripts/docs consistently refer to them.

Status

- `.gitignore` covers build outputs for the normal and nolibc binaries.
- `make test` runs `scripts/run_all.sh`, which checks required binaries and oracle tools.
- Documented: top-level `README.md` describes `make ultra` outputs and `LIBWEBP_BIN_DIR` usage.
- Implemented: `scripts/build_all.sh` builds all expected binaries.

Plan

1. Standardize naming rules (documented in top-level `README.md`):
   - normal builds: `decoder`, `encoder`
   - nolibc variants: suffix `_nolibc[_tiny|_ultra]`
2. Ensure `.gitignore` lists all built binaries.
3. Ensure `make ultra` behavior is documented:
   - builds `decoder_nolibc_ultra`, `encoder_nolibc_ultra`, and `encoder`
4. Add a tiny helper script (optional):
   - `scripts/build_all.sh` that builds everything expected.

Testing / regressions

- Run the existing smoke script after builds:
  - `scripts/smoke_binaries.sh` checks `-x` for required binaries.

Acceptance criteria

- All references in docs/scripts match actual outputs.
- New contributors can guess the binary name from the build flavor.
