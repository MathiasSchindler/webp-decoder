# src/

This folder contains the decoder and encoder implementations, now organized by runtime component rather than by development milestone.

- `common/`: shared low-level utilities (syscall I/O, bounded reads, endian helpers, bitreaders)
- `vp8/`: shared VP8 spec primitives used by both directions
	- quant lookup/factor derivation
	- inverse WHT/DCT transforms
	- B_PRED 4x4 intra prediction
	- YUV420-to-RGB conversion and upsampling

## Decoder

- `decoder/`: consolidated decoder implementation
	- RIFF/WebP container parsing
	- VP8 frame headers, bool decoder, trees, coefficient tokens
	- reconstruction and loopfilter
	- YUV->RGB PPM output and PNG writer

## Encoder

- `encoder/`: consolidated encoder implementation
	- PNG input, RIFF writer, VP8 bool/bit writing
	- RGB→YUV conversion and padding helpers
	- forward transform and encoder-specific quantization
	- tokenization/entropy coding, loopfilter params
	- in-loop reconstruction + mode decision drivers

## Entrypoints

- `main.c`: decoder CLI used by both normal and nolibc builds
- `encoder_main.c`: normal encoder CLI
- `nolibc/`: minimal syscall-only runtime glue used by the nolibc build
