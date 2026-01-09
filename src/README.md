# src/

This folder contains the decoder and encoder implementations, split into milestone-focused subdirectories so it’s easy to keep progress isolated and reproducible.

- `common/`: shared low-level utilities (syscall I/O, bounded reads, endian helpers, bitreaders)

## Decoder milestones

- `m01_container/`: RIFF/WebP container parsing (RFC 9649)
- `m02_vp8_header/`: VP8 frame tag + key-frame header parsing (RFC 6386)
- `m03_bool_decoder/`: boolean entropy decoder + bitreader
- `m04_frame_header_full/`: full VP8 frame header parsing
- `m05_tokens/`: coefficient/token decoding
- `m06_recon/`: prediction + inverse transforms + reconstruct to YUV
- `m07_loopfilter/`: in-loop deblocking filter
- `m08_yuv2rgb_ppm/`: YUV->RGB + PPM writer
- `m09_png/`: PNG writer for decoded output

## Encoder milestones

- `enc-m00_png/`: PNG reader (input)
- `enc-m01_riff/`: WebP RIFF container writer (output wrapper)
- `enc-m02_vp8_bitwriter/`: VP8 bit writing (boolean encoder + helpers)
- `enc-m03_vp8_headers/`: VP8 keyframe/header emission helpers
- `enc-m04_yuv/`: RGB→YUV420 conversion + padding helpers
- `enc-m05_intra/`: intra prediction + transforms
- `enc-m06_quant/`: quantization + quality→qindex mapping
- `enc-m07_tokens/`: tokenization + entropy coding of coeffs/modes
- `enc-m08_filter/`: loopfilter parameter derivation / header plumbing
- `enc-m08_recon/`: in-loop reconstruction + mode decision drivers

## Entrypoints

- `main.c`: normal decoder CLI
- `main_ultra.c`: syscall-only decoder CLI (ultra/nolibc)
- `encoder_main.c`: normal encoder CLI
- `encoder_main_ultra.c`: syscall-only encoder CLI (ultra/nolibc)
- `nolibc/`: minimal syscall-only runtime glue used by the ultra builds

The intention is that each milestone can be built/run independently, while sharing low-level primitives from `common/`.
