CC ?= cc

# Default to parallel builds using all available CPU cores.
# Users can override with `make -jN` or by setting `JOBS=`.
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
ifeq (,$(filter -j%,$(MAKEFLAGS)))
MAKEFLAGS += -j$(JOBS)
endif

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# The syscall-only (nolibc) builds are Linux x86_64 specific.
NOLIBC_SUPPORTED := 0
ifeq ($(UNAME_S),Linux)
ifeq ($(UNAME_M),x86_64)
NOLIBC_SUPPORTED := 1
endif
endif

BIN := decoder
ENCODER := encoder
BUILD_DIR := build
ENC_PNGDUMP_BIN := build/enc_pngdump
ENC_PNG2PPM_BIN := build/enc_png2ppm
ENC_QUALITY_METRICS_BIN := build/enc_quality_metrics
ENC_WEBPWRAP_BIN := build/enc_webpwrap
ENC_BOOLSELFTEST_BIN := build/enc_boolselftest
ENC_M03_MINIFRAME_BIN := build/enc_m03_miniframe
ENC_M04_MINIFRAME_BIN := build/enc_m04_miniframe
ENC_M05_YUVDUMP_BIN := build/enc_m05_yuvdump
ENC_M06_INTRADUMP_BIN := build/enc_m06_intradump
ENC_M07_QUANTDUMP_BIN := build/enc_m07_quantdump
ENC_M08_TOKENTEST_BIN := build/enc_m08_tokentest
ENC_M09_DCENC_BIN := build/enc_m09_dcenc
ENC_M09_MODEENC_BIN := build/enc_m09_modeenc
ENC_M09_BPREDENC_BIN := build/enc_m09_bpredenc
NOLIBC_BUILD_DIR := build/nolibc
NOLIBC_BIN := decoder_nolibc
NOLIBC_TINY_BUILD_DIR := build/nolibc_tiny
NOLIBC_TINY_BIN := decoder_nolibc_tiny
NOLIBC_ULTRA_BUILD_DIR := build/nolibc_ultra
NOLIBC_ULTRA_BIN := decoder_nolibc_ultra
ENC_NOLIBC_ULTRA_BUILD_DIR := build/nolibc_ultra_enc
ENC_NOLIBC_ULTRA_BIN := encoder_nolibc_ultra

SRC := \
	src/main.c \
	src/common/os.c \
	src/common/fmt.c \
	src/m01_container/webp_container.c \
	src/m02_vp8_header/vp8_header.c \
	src/m03_bool_decoder/bool_decoder.c \
	src/m04_frame_header_full/vp8_frame_header_basic.c \
	src/m05_tokens/vp8_tree.c \
	src/m05_tokens/vp8_tokens.c \
	src/m06_recon/vp8_recon.c \
	src/m07_loopfilter/vp8_loopfilter.c \
	src/m08_yuv2rgb_ppm/yuv2rgb_ppm.c \
	src/m09_png/yuv2rgb_png.c

OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))

CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-O3 -march=native -flto \
	-fno-omit-frame-pointer -fno-common

LDFLAGS_COMMON := -flto

.PHONY: all clean nolibc nolibc_tiny nolibc_ultra ultra test
.PHONY: enc_pngdump
.PHONY: enc_png2ppm
.PHONY: enc_quality_metrics
.PHONY: enc_webpwrap
.PHONY: enc_boolselftest
.PHONY: enc_m03_miniframe
.PHONY: enc_m04_miniframe
.PHONY: enc_m05_yuvdump
.PHONY: enc_m06_intradump
.PHONY: enc_m07_quantdump
.PHONY: enc_m08_tokentest
.PHONY: enc_m09_dcenc
.PHONY: enc_m09_modeenc
.PHONY: enc_m09_bpredenc

all: $(BIN) $(ENCODER)

test: all ultra \
	enc_pngdump enc_png2ppm enc_quality_metrics enc_webpwrap enc_boolselftest \
	enc_m03_miniframe enc_m04_miniframe enc_m05_yuvdump enc_m06_intradump \
	enc_m07_quantdump enc_m08_tokentest enc_m09_dcenc enc_m09_modeenc enc_m09_bpredenc
	# Run gates without inheriting MAKEFLAGS/MAKELEVEL to avoid jobserver warnings
	# from scripts that invoke `make` internally.
	env -u MAKEFLAGS -u MAKELEVEL TEST_JOBS=$(JOBS) ./scripts/run_all.sh

# Encoder Milestone 0 helper: tiny PNG reader driver
enc_pngdump: $(ENC_PNGDUMP_BIN)

enc_png2ppm: $(ENC_PNG2PPM_BIN)

enc_quality_metrics: $(ENC_QUALITY_METRICS_BIN)

enc_webpwrap: $(ENC_WEBPWRAP_BIN)

enc_boolselftest: $(ENC_BOOLSELFTEST_BIN)

enc_m03_miniframe: $(ENC_M03_MINIFRAME_BIN)

enc_m04_miniframe: $(ENC_M04_MINIFRAME_BIN)

enc_m05_yuvdump: $(ENC_M05_YUVDUMP_BIN)

enc_m06_intradump: $(ENC_M06_INTRADUMP_BIN)

enc_m07_quantdump: $(ENC_M07_QUANTDUMP_BIN)

enc_m08_tokentest: $(ENC_M08_TOKENTEST_BIN)

enc_m09_dcenc: $(ENC_M09_DCENC_BIN)

enc_m09_modeenc: $(ENC_M09_MODEENC_BIN)

enc_m09_bpredenc: $(ENC_M09_BPREDENC_BIN)

ifeq ($(NOLIBC_SUPPORTED),1)
nolibc: $(NOLIBC_BIN)

nolibc_tiny: $(NOLIBC_TINY_BIN)

nolibc_ultra: $(NOLIBC_ULTRA_BIN)
else
nolibc nolibc_tiny nolibc_ultra:
	@echo "error: nolibc targets are supported only on Linux x86_64" >&2
	@echo "hint: use 'make' (normal libc build) or 'make ultra' (portable ultra build)" >&2
	@exit 2
endif

# Friendly alias: `make ultra` builds the tiny PNG-by-default syscall-only binary,
# and also builds both the normal (libc) encoder and a nolibc ultra encoder.
ifeq ($(NOLIBC_SUPPORTED),1)
ultra: nolibc_ultra $(ENC_NOLIBC_ULTRA_BIN) $(ENCODER)
else
.PHONY: ultra_portable
ultra: ultra_portable $(ENCODER)
endif

$(BIN): $(OBJ)
	$(CC) $(CFLAGS_COMMON) -o $@ $(OBJ) $(LDFLAGS_COMMON)

ENCODER_SRC := \
	src/encoder_main.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m04_yuv/enc_pad.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m06_quant/enc_quant.c \
	src/enc-m06_quant/enc_quality_table.c \
	src/enc-m07_tokens/enc_vp8_tokens.c \
	src/enc-m08_filter/enc_loopfilter.c \
	src/enc-m08_recon/enc_recon.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/enc-m01_riff/enc_riff.c

$(ENCODER): $(ENCODER_SRC) \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h \
	src/enc-m04_yuv/enc_pad.h \
	src/enc-m07_tokens/enc_vp8_tokens.h \
	src/enc-m08_filter/enc_loopfilter.h \
	src/enc-m08_recon/enc_recon.h \
	src/enc-m01_riff/enc_riff.h
	$(CC) $(CFLAGS_COMMON) -o $@ $(ENCODER_SRC) $(LDFLAGS_COMMON)

ENC_PNGDUMP_SRC := \
	tools/enc_pngdump.c \
	src/enc-m00_png/enc_png.c

$(ENC_PNGDUMP_BIN): $(ENC_PNGDUMP_SRC) src/enc-m00_png/enc_png.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_PNGDUMP_SRC)

ENC_PNG2PPM_SRC := \
	tools/enc_png2ppm.c \
	src/enc-m00_png/enc_png.c

$(ENC_PNG2PPM_BIN): $(ENC_PNG2PPM_SRC) src/enc-m00_png/enc_png.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_PNG2PPM_SRC)

ENC_QUALITY_METRICS_SRC := \
	tools/enc_quality_metrics.c \
	src/quality/quality_ppm.c \
	src/quality/quality_psnr.c \
	src/quality/quality_ssim.c

$(ENC_QUALITY_METRICS_BIN): $(ENC_QUALITY_METRICS_SRC) \
	src/quality/quality_ppm.h \
	src/quality/quality_psnr.h \
	src/quality/quality_ssim.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_QUALITY_METRICS_SRC) -lm

ENC_WEBPWRAP_SRC := \
	tools/enc_webpwrap.c \
	src/enc-m01_riff/enc_riff.c

$(ENC_WEBPWRAP_BIN): $(ENC_WEBPWRAP_SRC) src/enc-m01_riff/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_WEBPWRAP_SRC)

ENC_BOOLSELFTEST_SRC := \
	tools/enc_boolselftest.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/m03_bool_decoder/bool_decoder.c \
	src/common/os.c

$(ENC_BOOLSELFTEST_BIN): $(ENC_BOOLSELFTEST_SRC) \
	src/enc-m02_vp8_bitwriter/enc_bool.h \
	src/m03_bool_decoder/bool_decoder.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_BOOLSELFTEST_SRC)

ENC_M03_MINIFRAME_SRC := \
	tools/enc_m03_miniframe.c \
	src/enc-m01_riff/enc_riff.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/enc-m03_vp8_headers/enc_vp8_miniframe.c

$(ENC_M03_MINIFRAME_BIN): $(ENC_M03_MINIFRAME_SRC) \
	src/enc-m03_vp8_headers/enc_vp8_miniframe.h \
	src/enc-m02_vp8_bitwriter/enc_bool.h \
	src/enc-m01_riff/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M03_MINIFRAME_SRC)

ENC_M04_MINIFRAME_SRC := \
	tools/enc_m04_miniframe.c \
	src/enc-m01_riff/enc_riff.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/enc-m04_yuv/enc_pad.c \
	src/enc-m04_yuv/enc_vp8_eob.c

$(ENC_M04_MINIFRAME_BIN): $(ENC_M04_MINIFRAME_SRC) \
	src/enc-m04_yuv/enc_vp8_eob.h \
	src/enc-m04_yuv/enc_pad.h \
	src/enc-m02_vp8_bitwriter/enc_bool.h \
	src/enc-m01_riff/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M04_MINIFRAME_SRC)

ENC_M05_YUVDUMP_SRC := \
	tools/enc_m05_yuvdump.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c

$(ENC_M05_YUVDUMP_BIN): $(ENC_M05_YUVDUMP_SRC) \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M05_YUVDUMP_SRC)

ENC_M06_INTRADUMP_SRC := \
	tools/enc_m06_intradump.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m05_intra/enc_intra_dc.c

$(ENC_M06_INTRADUMP_BIN): $(ENC_M06_INTRADUMP_SRC) \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h \
	src/enc-m05_intra/enc_transform.h \
	src/enc-m05_intra/enc_intra_dc.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M06_INTRADUMP_SRC)

ENC_M07_QUANTDUMP_SRC := \
	tools/enc_m07_quantdump.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m05_intra/enc_intra_dc.c \
	src/enc-m06_quant/enc_quant.c \
	src/enc-m06_quant/enc_quality_table.c

$(ENC_M07_QUANTDUMP_BIN): $(ENC_M07_QUANTDUMP_SRC) \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h \
	src/enc-m05_intra/enc_transform.h \
	src/enc-m05_intra/enc_intra_dc.h \
	src/enc-m06_quant/enc_quant.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M07_QUANTDUMP_SRC)

ENC_M08_TOKENTEST_SRC := \
	tools/enc_m08_tokentest.c \
	src/common/os.c \
	src/m02_vp8_header/vp8_header.c \
	src/m03_bool_decoder/bool_decoder.c \
	src/m05_tokens/vp8_tree.c \
	src/m05_tokens/vp8_tokens.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m04_yuv/enc_pad.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m05_intra/enc_intra_dc.c \
	src/enc-m06_quant/enc_quant.c \
	src/enc-m06_quant/enc_quality_table.c \
	src/enc-m07_tokens/enc_vp8_tokens.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c

$(ENC_M08_TOKENTEST_BIN): $(ENC_M08_TOKENTEST_SRC) \
	src/m05_tokens/vp8_tokens.h \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h \
	src/enc-m05_intra/enc_intra_dc.h \
	src/enc-m06_quant/enc_quant.h \
	src/enc-m07_tokens/enc_vp8_tokens.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M08_TOKENTEST_SRC)

ENC_M09_DCENC_SRC := \
	tools/enc_m09_dcenc.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m04_yuv/enc_pad.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m06_quant/enc_quant.c \
	src/enc-m06_quant/enc_quality_table.c \
	src/enc-m07_tokens/enc_vp8_tokens.c \
	src/enc-m08_filter/enc_loopfilter.c \
	src/enc-m08_recon/enc_recon.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/enc-m01_riff/enc_riff.c

$(ENC_M09_DCENC_BIN): $(ENC_M09_DCENC_SRC) \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h \
	src/enc-m04_yuv/enc_pad.h \
	src/enc-m07_tokens/enc_vp8_tokens.h \
	src/enc-m08_recon/enc_recon.h \
	src/enc-m01_riff/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M09_DCENC_SRC)

ENC_M09_MODEENC_SRC := \
	tools/enc_m09_modeenc.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m04_yuv/enc_pad.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m06_quant/enc_quant.c \
	src/enc-m06_quant/enc_quality_table.c \
	src/enc-m07_tokens/enc_vp8_tokens.c \
	src/enc-m08_filter/enc_loopfilter.c \
	src/enc-m08_recon/enc_recon.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/enc-m01_riff/enc_riff.c

$(ENC_M09_MODEENC_BIN): $(ENC_M09_MODEENC_SRC) \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h \
	src/enc-m04_yuv/enc_pad.h \
	src/enc-m07_tokens/enc_vp8_tokens.h \
	src/enc-m08_recon/enc_recon.h \
	src/enc-m01_riff/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M09_MODEENC_SRC)

ENC_M09_BPREDENC_SRC := \
	tools/enc_m09_bpredenc.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m04_yuv/enc_pad.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m06_quant/enc_quant.c \
	src/enc-m06_quant/enc_quality_table.c \
	src/enc-m07_tokens/enc_vp8_tokens.c \
	src/enc-m08_filter/enc_loopfilter.c \
	src/enc-m08_recon/enc_recon.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/enc-m01_riff/enc_riff.c

$(ENC_M09_BPREDENC_BIN): $(ENC_M09_BPREDENC_SRC) \
	src/enc-m00_png/enc_png.h \
	src/enc-m04_yuv/enc_rgb_to_yuv.h \
	src/enc-m04_yuv/enc_pad.h \
	src/enc-m07_tokens/enc_vp8_tokens.h \
	src/enc-m08_recon/enc_recon.h \
	src/enc-m01_riff/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M09_BPREDENC_SRC)

NOLIBC_SRC := $(SRC) \
	src/nolibc/syscall_glue.c

NOLIBC_OBJ := $(patsubst src/%.c,$(NOLIBC_BUILD_DIR)/%.o,$(filter %.c,$(NOLIBC_SRC))) \
	$(NOLIBC_BUILD_DIR)/nolibc/start.o

NOLIBC_LTO := -flto

NOLIBC_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-Os -march=x86-64 \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-common \
	-ffunction-sections -fdata-sections \
	-DNO_LIBC \
	$(NOLIBC_LTO)

NOLIBC_LDFLAGS := -nostdlib -static \
	-Wl,-e,_start -Wl,--gc-sections -Wl,--build-id=none -s

$(NOLIBC_BIN): $(NOLIBC_OBJ)
	$(CC) $(NOLIBC_LTO) -o $@ $(NOLIBC_OBJ) $(NOLIBC_LDFLAGS) -lgcc

NOLIBC_TINY_SRC := $(filter-out \
	src/m08_yuv2rgb_ppm/yuv2rgb_ppm.c \
	src/m09_png/yuv2rgb_png.c,\
	$(SRC)) \
	src/nolibc/syscall_glue.c

NOLIBC_TINY_OBJ := $(patsubst src/%.c,$(NOLIBC_TINY_BUILD_DIR)/%.o,$(filter %.c,$(NOLIBC_TINY_SRC))) \
	$(NOLIBC_TINY_BUILD_DIR)/nolibc/start.o

NOLIBC_TINY_CFLAGS := $(NOLIBC_CFLAGS) -DDECODER_TINY

# Ultra is a very small, PNG-by-default, nolibc build. Allow extra size-tuning flags here.
# You can pass additional flags at build time, e.g.
#   make nolibc_ultra ULTRA_EXTRA_CFLAGS='-fno-ipa-cp'
NOLIBC_ULTRA_CFLAGS := $(NOLIBC_CFLAGS) -DDECODER_ULTRA -fno-inline $(ULTRA_EXTRA_CFLAGS)

$(NOLIBC_TINY_BIN): $(NOLIBC_TINY_OBJ)
	$(CC) $(NOLIBC_LTO) -o $@ $(NOLIBC_TINY_OBJ) $(NOLIBC_LDFLAGS) -lgcc

NOLIBC_ULTRA_SRC := \
	src/main_ultra.c \
	src/common/os_readall.c \
	src/m01_container/webp_container.c \
	src/m02_vp8_header/vp8_header.c \
	src/m03_bool_decoder/bool_decoder.c \
	src/m04_frame_header_full/vp8_frame_header_basic.c \
	src/m05_tokens/vp8_tree.c \
	src/m05_tokens/vp8_tokens.c \
	src/m06_recon/vp8_recon.c \
	src/m07_loopfilter/vp8_loopfilter.c \
	src/m09_png/yuv2rgb_png.c \
	src/nolibc/syscall_glue.c

NOLIBC_ULTRA_OBJ := $(patsubst src/%.c,$(NOLIBC_ULTRA_BUILD_DIR)/%.o,$(filter %.c,$(NOLIBC_ULTRA_SRC))) \
	$(NOLIBC_ULTRA_BUILD_DIR)/nolibc/start.o

ifeq ($(NOLIBC_SUPPORTED),1)
$(NOLIBC_ULTRA_BIN): $(NOLIBC_ULTRA_OBJ)
	$(CC) $(NOLIBC_LTO) -o $@ $(NOLIBC_ULTRA_OBJ) $(NOLIBC_LDFLAGS) -lgcc

$(NOLIBC_ULTRA_BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(NOLIBC_ULTRA_CFLAGS) -c $< -o $@

$(NOLIBC_ULTRA_BUILD_DIR)/nolibc/start.o: src/nolibc/start.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@
else
ULTRA_PORTABLE_DECODER_SRC := $(filter-out src/nolibc/syscall_glue.c,$(NOLIBC_ULTRA_SRC))

ultra_portable: $(NOLIBC_ULTRA_BIN) $(ENC_NOLIBC_ULTRA_BIN)

$(NOLIBC_ULTRA_BIN): $(ULTRA_PORTABLE_DECODER_SRC)
	$(CC) $(CFLAGS_COMMON) -Os -o $@ $(ULTRA_PORTABLE_DECODER_SRC) $(LDFLAGS_COMMON)
endif

$(NOLIBC_TINY_BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(NOLIBC_TINY_CFLAGS) -c $< -o $@

$(NOLIBC_TINY_BUILD_DIR)/nolibc/start.o: src/nolibc/start.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(NOLIBC_BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(NOLIBC_CFLAGS) -c $< -o $@

$(NOLIBC_BUILD_DIR)/nolibc/start.o: src/nolibc/start.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN) $(ENCODER) $(NOLIBC_BIN) $(NOLIBC_TINY_BIN) $(NOLIBC_ULTRA_BIN) $(ENC_NOLIBC_ULTRA_BIN)

# --- nolibc ultra encoder ---

ENC_NOLIBC_ULTRA_SRC := \
	src/encoder_main_ultra.c \
	src/enc-m00_png/enc_png.c \
	src/enc-m04_yuv/enc_rgb_to_yuv.c \
	src/enc-m04_yuv/enc_gamma_tables.c \
	src/enc-m04_yuv/enc_pad.c \
	src/enc-m05_intra/enc_transform.c \
	src/enc-m06_quant/enc_quant.c \
	src/enc-m06_quant/enc_quality_table.c \
	src/enc-m07_tokens/enc_vp8_tokens.c \
	src/enc-m08_filter/enc_loopfilter.c \
	src/enc-m08_recon/enc_recon.c \
	src/enc-m02_vp8_bitwriter/enc_bool.c \
	src/enc-m01_riff/enc_riff.c \
	src/nolibc/syscall_glue.c

ENC_NOLIBC_ULTRA_OBJ := $(patsubst src/%.c,$(ENC_NOLIBC_ULTRA_BUILD_DIR)/%.o,$(filter %.c,$(ENC_NOLIBC_ULTRA_SRC))) \
	$(ENC_NOLIBC_ULTRA_BUILD_DIR)/nolibc/start.o

ENC_NOLIBC_ULTRA_CFLAGS := $(NOLIBC_CFLAGS) -DENCODER_ULTRA -fno-inline $(ULTRA_EXTRA_CFLAGS)

ifeq ($(NOLIBC_SUPPORTED),1)
$(ENC_NOLIBC_ULTRA_BIN): $(ENC_NOLIBC_ULTRA_OBJ)
	$(CC) $(NOLIBC_LTO) -o $@ $(ENC_NOLIBC_ULTRA_OBJ) $(NOLIBC_LDFLAGS) -lgcc

$(ENC_NOLIBC_ULTRA_BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ENC_NOLIBC_ULTRA_CFLAGS) -c $< -o $@

$(ENC_NOLIBC_ULTRA_BUILD_DIR)/nolibc/start.o: src/nolibc/start.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@
else
ULTRA_PORTABLE_ENCODER_SRC := $(filter-out src/nolibc/syscall_glue.c,$(ENC_NOLIBC_ULTRA_SRC))

$(ENC_NOLIBC_ULTRA_BIN): $(ULTRA_PORTABLE_ENCODER_SRC)
	$(CC) $(CFLAGS_COMMON) -Os -o $@ $(ULTRA_PORTABLE_ENCODER_SRC) $(LDFLAGS_COMMON)
endif
