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

## Encoder

- `encoder/`: consolidated encoder implementation
	- PNG input, RIFF writer, VP8 bool/bit writing
	- RGB→YUV conversion and padding helpers
	- intra prediction, transform, quantization
	- tokenization/entropy coding, loopfilter params
	- in-loop reconstruction + mode decision drivers

## Entrypoints

- `main.c`: normal decoder CLI
- `main_ultra.c`: syscall-only decoder CLI (ultra/nolibc)
- `encoder_main.c`: normal encoder CLI
- `encoder_main_ultra.c`: syscall-only encoder CLI (ultra/nolibc)
- `nolibc/`: minimal syscall-only runtime glue used by the ultra builds

The intention is that each milestone can be built/run independently, while sharing low-level primitives from `common/`.
