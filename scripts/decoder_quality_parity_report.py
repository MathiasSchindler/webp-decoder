#!/usr/bin/env python3
"""Print a WebP decoder quality/format parity report for the local corpus."""

from __future__ import annotations

import argparse
import collections
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Counter, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CORPUS = ["images/**/*.webp"]
INFO_FIELDS = (
    "Key frame",
    "Profile",
    "Display",
    "X scale",
    "Y scale",
    "Color space",
    "Clamp type",
    "Use segment",
    "Simple filter",
    "Use lf delta",
    "Total partitions",
)


def read_chunks(path: Path) -> tuple[str, ...]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WEBP":
        return ("NOT_RIFF_WEBP",)
    chunks: list[str] = []
    off = 12
    while off + 8 <= len(data):
        tag = data[off : off + 4].decode("latin1")
        size = int.from_bytes(data[off + 4 : off + 8], "little")
        off += 8
        if off + size > len(data):
            chunks.append(f"{tag}:TRUNCATED")
            return tuple(chunks)
        chunks.append(tag)
        off += size + (size & 1)
    if off != len(data):
        chunks.append("TRAILING_BYTES")
    return tuple(chunks)


def corpus_files(patterns: Iterable[str]) -> list[Path]:
    files: set[Path] = set()
    for pat in patterns:
        files.update(ROOT.glob(pat))
    return sorted(p for p in files if p.is_file())


def parse_decoder_info(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"\s*([^:]+):\s*(.*?)\s*$", line)
        if not m:
            continue
        key = m.group(1)
        if key in INFO_FIELDS:
            value = m.group(2).split()[0] if m.group(2) else ""
            out[key] = value
    return out


def scan_decoder(files: list[Path], decoder: Path | None) -> tuple[dict[str, Counter[str]], list[tuple[Path, str]]]:
    counts: dict[str, Counter[str]] = {field: collections.Counter() for field in INFO_FIELDS}
    failures: list[tuple[Path, str]] = []
    if decoder is None or not decoder.exists():
        return counts, [(Path("<all>"), "decoder binary not found; run make first or set DECODER")] if files else []
    for path in files:
        rel = path.relative_to(ROOT)
        proc = subprocess.run(
            [str(decoder), "-info", str(rel)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if proc.returncode != 0:
            failures.append((rel, proc.stderr.strip() or f"exit {proc.returncode}"))
            continue
        info = parse_decoder_info(proc.stdout)
        for field, value in info.items():
            counts[field][value] += 1
    return counts, failures


def fmt_counter(counter: Counter[str]) -> str:
    if not counter:
        return "n/a"
    return ", ".join(f"{k}={v}" for k, v in sorted(counter.items()))


def print_table(rows: list[tuple[str, str, str, str]]) -> None:
    print("| Area | Current decoder status | Local corpus evidence | Validation / next gate |")
    print("| --- | --- | --- | --- |")
    for row in rows:
        print("| " + " | ".join(cell.replace("\n", "<br>") for cell in row) + " |")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patterns", nargs="*", default=DEFAULT_CORPUS, help="glob(s) relative to repo root")
    parser.add_argument("--decoder", default=os.environ.get("DECODER", "build/decoder"), help="decoder binary")
    args = parser.parse_args()

    files = corpus_files(args.patterns)
    chunk_counts = collections.Counter(read_chunks(path) for path in files)
    by_dir = collections.Counter(str(path.relative_to(ROOT).parent) for path in files)
    chunk_presence = collections.Counter(tag for path in files for tag in set(read_chunks(path)))
    decoder_path = (ROOT / args.decoder) if not Path(args.decoder).is_absolute() else Path(args.decoder)
    info_counts, failures = scan_decoder(files, decoder_path)

    supported_vp8 = chunk_counts.get(("VP8 ",), 0)
    extended = sum(n for sig, n in chunk_counts.items() if sig != ("VP8 ",))
    total = len(files)

    print("# Decoder quality/feature parity report")
    print()
    print(f"Corpus patterns: `{', '.join(args.patterns)}`")
    print(f"Files scanned: {total}")
    print(f"Simple lossy `VP8 ` only: {supported_vp8}")
    print(f"Extended/lossless/other signatures: {extended}")
    print(f"Decoder `-info` failures: {len(failures)}")
    print()
    print("## Corpus inventory")
    print()
    print("| Directory | WebP files |")
    print("| --- | ---: |")
    for directory, count in sorted(by_dir.items()):
        print(f"| `{directory}` | {count} |")
    print()
    print("## Observed bitstream fields")
    print()
    print("| Field | Counts |")
    print("| --- | --- |")
    for field in INFO_FIELDS:
        print(f"| {field} | {fmt_counter(info_counts[field])} |")
    print()
    print("## Quality/format parity matrix")
    print()
    rows = [
        (
            "Simple RIFF/WebP + `VP8 ` still image",
            "Supported when the file is exactly `RIFF`/`WEBP` with one `VP8 ` chunk and no extra chunks.",
            f"{supported_vp8}/{total} files are simple `VP8 ` only.",
            "Header and pixel gates compare against `webpinfo`/`dwebp`.",
        ),
        (
            "VP8 key frames",
            "Supported; inter frames are rejected before decode.",
            fmt_counter(info_counts["Key frame"]),
            "`m5` syntax smoke plus `m6`/`m7`/`m8` byte-exact pixel gates.",
        ),
        (
            "Token partitions",
            "Only `Total partitions: 1` is decoded; 2/4/8 partition streams remain unsupported.",
            fmt_counter(info_counts["Total partitions"]),
            "Need generated or curated >1 partition vectors, then extend `m4` and pixel gates.",
        ),
        (
            "VP8 profiles / filters / segmentation",
            "Observed profiles, normal/simple filter, quant deltas, probability updates, skip flags, and segmentation are decoded for key frames.",
            "Profile " + fmt_counter(info_counts["Profile"]) + "; segment " + fmt_counter(info_counts["Use segment"]) + "; simple_filter " + fmt_counter(info_counts["Simple filter"]),
            "Keep byte-exact YUV/PPM gates; add explicit profile 2/3 vectors before claiming full VP8 profile parity.",
        ),
        (
            "VP8X extended container",
            "Unsupported; parser currently requires the first and only chunk to be `VP8 `.",
            f"VP8X chunks present: {chunk_presence.get('VP8X', 0)}",
            "Add chunk iterator, canvas validation, feature flag checks, and negative fixtures.",
        ),
        (
            "Alpha (`ALPH`)",
            "Unsupported; no alpha decode, compositing, or RGBA PNG output.",
            f"ALPH chunks present: {chunk_presence.get('ALPH', 0)}",
            "After VP8X, add ALPH decode and byte-exact RGBA output tests vs `dwebp -png`.",
        ),
        (
            "Lossless (`VP8L`)",
            "Unsupported; VP8L is a different bitstream.",
            f"VP8L chunks present: {chunk_presence.get('VP8L', 0)}",
            "Separate lossless decoder milestone and PNG/RGBA parity gates.",
        ),
        (
            "Animation (`ANIM`/`ANMF`) and inter frames",
            "Unsupported; no canvas frame composition or inter-frame VP8 references.",
            f"ANIM={chunk_presence.get('ANIM', 0)}, ANMF={chunk_presence.get('ANMF', 0)}",
            "Add only after still-image VP8X/alpha/lossless priorities are clear.",
        ),
        (
            "Metadata and color/profile implications (`ICCP`, `EXIF`, `XMP`)",
            "Unsupported as container chunks; RGB conversion is libwebp-compatible for current YUV but no color management is applied.",
            f"ICCP={chunk_presence.get('ICCP', 0)}, EXIF={chunk_presence.get('EXIF', 0)}, XMP={chunk_presence.get('XMP ', 0)}",
            "Parse/skip metadata safely; decide whether ICC profiles are reported only or applied.",
        ),
    ]
    print_table(rows)
    print()
    print("## Prioritized roadmap")
    print()
    print("1. **Strengthen current-scope gates**: keep all simple lossy VP8 corpora, including `images/commons/generated-webp`, in byte-exact YUV/PPM/PNG validation.")
    print("2. **Format parity before speed**: implement multi-token partitions, then VP8X chunk iteration/metadata skipping, then alpha output policy.")
    print("3. **Broader libwebp parity**: add VP8L lossless and animation only after still-image extended-container support is stable.")
    print("4. **Speed work**: benchmark only within a frozen feature gate so optimizations cannot hide format or pixel regressions.")
    if failures:
        print()
        print("## Decoder failures")
        print()
        for path, msg in failures[:20]:
            print(f"- `{path}`: {msg}")
        if len(failures) > 20:
            print(f"- ... {len(failures) - 20} more")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
