# src/

This folder contains the decoder and encoder implementations, now organized by runtime component rather than by development milestone.

- `common/`: shared low-level utilities (syscall I/O, bounded reads, endian helpers, bitreaders)

## Decoder

- `decoder/`: consolidated decoder implementation
	- RIFF/WebP container parsing
	- VP8 frame headers, bool decoder, trees, coefficient tokens
	- prediction, inverse transforms, reconstruction, and loopfilter
	- YUV->RGB PPM output and PNG writer

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
