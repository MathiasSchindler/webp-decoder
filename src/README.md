# src/

This folder contains the decoder implementation, split into milestone-focused subdirectories so it’s easy to keep progress isolated and reproducible.

- `common/`: shared low-level utilities (syscall I/O, bounded reads, endian helpers, bitreaders)
- `m01_container/`: RIFF/WebP container parsing (RFC 9649)
- `m02_vp8_header/`: VP8 frame tag + key-frame header parsing (RFC 6386)
- `m03_bool_decoder/`: boolean entropy decoder + bitreader
- `m04_frame_header_full/`: full VP8 frame header parsing
- `m05_tokens/`: coefficient/token decoding
- `m06_recon_yuv/`: prediction + inverse transforms + reconstruct to YUV
- `m07_loop_filter/`: in-loop deblocking filter
- `m08_yuv2rgb_ppm/`: YUV->RGB + PPM writer
- `m09_hardening/`: limits, fuzz/robustness harnesses, trace modes

The intention is that each milestone can be built/run independently (or via a shared build system later), while sharing low-level primitives from `common/`.
