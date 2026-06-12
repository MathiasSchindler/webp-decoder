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

BUILD_DIR := build
BIN := $(BUILD_DIR)/decoder
ENCODER := $(BUILD_DIR)/encoder
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

SPEED ?= 1
NATIVE ?= 1

VP8_DECODER_SHARED_SRC := \
	src/vp8/vp8_quant.c \
	src/vp8/vp8_transform.c \
	src/vp8/vp8_pred.c \
	src/vp8/vp8_yuv_rgb.c

VP8_ENCODER_SHARED_SRC := \
	src/vp8/vp8_quant.c \
	src/vp8/vp8_transform.c \
	src/vp8/vp8_pred.c

SRC := \
	src/main.c \
	src/common/os.c \
	src/common/fmt.c \
	$(VP8_DECODER_SHARED_SRC) \
	src/decoder/webp_container.c \
	src/decoder/vp8_header.c \
	src/decoder/bool_decoder.c \
	src/decoder/vp8_frame_header_basic.c \
	src/decoder/vp8_tree.c \
	src/decoder/vp8_tokens.c \
	src/decoder/vp8_recon.c \
	src/decoder/vp8_loopfilter.c \
	src/decoder/yuv2rgb_ppm.c \
	src/decoder/yuv2rgb_png.c

CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-O3 -march=native -flto \
	-fno-omit-frame-pointer -fno-common

LDFLAGS_COMMON := -flto

.PHONY: all clean nolibc test
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

ifeq ($(NOLIBC_SUPPORTED),1)
all: $(BIN) $(ENCODER)
nolibc: all
else
all nolibc:
	@echo "error: default builds are nolibc and are supported only on Linux x86_64" >&2
	@exit 2
endif

test: all \
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

ENCODER_SRC := \
	src/encoder_main.c \
	src/common/os.c \
	src/common/fmt.c \
	$(VP8_ENCODER_SHARED_SRC) \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c \
	src/encoder/enc_pad.c \
	src/encoder/enc_transform.c \
	src/encoder/enc_quant.c \
	src/encoder/enc_quality_table.c \
	src/encoder/enc_vp8_tokens.c \
	src/encoder/enc_loopfilter.c \
	src/encoder/enc_recon.c \
	src/encoder/enc_bool.c \
	src/encoder/enc_riff.c

ENC_PNGDUMP_SRC := \
	tools/enc_pngdump.c \
	src/encoder/enc_png.c

$(ENC_PNGDUMP_BIN): $(ENC_PNGDUMP_SRC) src/encoder/enc_png.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_PNGDUMP_SRC)

ENC_PNG2PPM_SRC := \
	tools/enc_png2ppm.c \
	src/encoder/enc_png.c

$(ENC_PNG2PPM_BIN): $(ENC_PNG2PPM_SRC) src/encoder/enc_png.h
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
	src/encoder/enc_riff.c

$(ENC_WEBPWRAP_BIN): $(ENC_WEBPWRAP_SRC) src/encoder/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_WEBPWRAP_SRC)

ENC_BOOLSELFTEST_SRC := \
	tools/enc_boolselftest.c \
	src/encoder/enc_bool.c \
	src/decoder/bool_decoder.c \
	src/common/os.c

$(ENC_BOOLSELFTEST_BIN): $(ENC_BOOLSELFTEST_SRC) \
	src/encoder/enc_bool.h \
	src/decoder/bool_decoder.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_BOOLSELFTEST_SRC)

ENC_M03_MINIFRAME_SRC := \
	tools/enc_m03_miniframe.c \
	src/encoder/enc_riff.c \
	src/encoder/enc_bool.c \
	src/encoder/enc_vp8_miniframe.c

$(ENC_M03_MINIFRAME_BIN): $(ENC_M03_MINIFRAME_SRC) \
	src/encoder/enc_vp8_miniframe.h \
	src/encoder/enc_bool.h \
	src/encoder/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M03_MINIFRAME_SRC)

ENC_M04_MINIFRAME_SRC := \
	tools/enc_m04_miniframe.c \
	src/encoder/enc_riff.c \
	src/encoder/enc_bool.c \
	src/encoder/enc_pad.c \
	src/encoder/enc_vp8_eob.c

$(ENC_M04_MINIFRAME_BIN): $(ENC_M04_MINIFRAME_SRC) \
	src/encoder/enc_vp8_eob.h \
	src/encoder/enc_pad.h \
	src/encoder/enc_bool.h \
	src/encoder/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M04_MINIFRAME_SRC)

ENC_M05_YUVDUMP_SRC := \
	tools/enc_m05_yuvdump.c \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c

$(ENC_M05_YUVDUMP_BIN): $(ENC_M05_YUVDUMP_SRC) \
	src/encoder/enc_png.h \
	src/encoder/enc_rgb_to_yuv.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M05_YUVDUMP_SRC)

ENC_M06_INTRADUMP_SRC := \
	tools/enc_m06_intradump.c \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c \
	src/encoder/enc_transform.c \
	src/encoder/enc_intra_dc.c

$(ENC_M06_INTRADUMP_BIN): $(ENC_M06_INTRADUMP_SRC) \
	src/encoder/enc_png.h \
	src/encoder/enc_rgb_to_yuv.h \
	src/encoder/enc_transform.h \
	src/encoder/enc_intra_dc.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M06_INTRADUMP_SRC)

ENC_M07_QUANTDUMP_SRC := \
	tools/enc_m07_quantdump.c \
	$(VP8_ENCODER_SHARED_SRC) \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c \
	src/encoder/enc_transform.c \
	src/encoder/enc_intra_dc.c \
	src/encoder/enc_quant.c \
	src/encoder/enc_quality_table.c

$(ENC_M07_QUANTDUMP_BIN): $(ENC_M07_QUANTDUMP_SRC) \
	src/encoder/enc_png.h \
	src/encoder/enc_rgb_to_yuv.h \
	src/encoder/enc_transform.h \
	src/encoder/enc_intra_dc.h \
	src/encoder/enc_quant.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M07_QUANTDUMP_SRC)

ENC_M08_TOKENTEST_SRC := \
	tools/enc_m08_tokentest.c \
	src/common/os.c \
	$(VP8_ENCODER_SHARED_SRC) \
	src/decoder/vp8_header.c \
	src/decoder/bool_decoder.c \
	src/decoder/vp8_tree.c \
	src/decoder/vp8_tokens.c \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c \
	src/encoder/enc_pad.c \
	src/encoder/enc_transform.c \
	src/encoder/enc_intra_dc.c \
	src/encoder/enc_quant.c \
	src/encoder/enc_quality_table.c \
	src/encoder/enc_vp8_tokens.c \
	src/encoder/enc_bool.c

$(ENC_M08_TOKENTEST_BIN): $(ENC_M08_TOKENTEST_SRC) \
	src/decoder/vp8_tokens.h \
	src/encoder/enc_png.h \
	src/encoder/enc_rgb_to_yuv.h \
	src/encoder/enc_intra_dc.h \
	src/encoder/enc_quant.h \
	src/encoder/enc_vp8_tokens.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M08_TOKENTEST_SRC)

ENC_M09_DCENC_SRC := \
	tools/enc_m09_dcenc.c \
	$(VP8_ENCODER_SHARED_SRC) \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c \
	src/encoder/enc_pad.c \
	src/encoder/enc_transform.c \
	src/encoder/enc_quant.c \
	src/encoder/enc_quality_table.c \
	src/encoder/enc_vp8_tokens.c \
	src/encoder/enc_loopfilter.c \
	src/encoder/enc_recon.c \
	src/encoder/enc_bool.c \
	src/encoder/enc_riff.c

$(ENC_M09_DCENC_BIN): $(ENC_M09_DCENC_SRC) \
	src/encoder/enc_png.h \
	src/encoder/enc_rgb_to_yuv.h \
	src/encoder/enc_pad.h \
	src/encoder/enc_vp8_tokens.h \
	src/encoder/enc_recon.h \
	src/encoder/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M09_DCENC_SRC)

ENC_M09_MODEENC_SRC := \
	tools/enc_m09_modeenc.c \
	$(VP8_ENCODER_SHARED_SRC) \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c \
	src/encoder/enc_pad.c \
	src/encoder/enc_transform.c \
	src/encoder/enc_quant.c \
	src/encoder/enc_quality_table.c \
	src/encoder/enc_vp8_tokens.c \
	src/encoder/enc_loopfilter.c \
	src/encoder/enc_recon.c \
	src/encoder/enc_bool.c \
	src/encoder/enc_riff.c

$(ENC_M09_MODEENC_BIN): $(ENC_M09_MODEENC_SRC) \
	src/encoder/enc_png.h \
	src/encoder/enc_rgb_to_yuv.h \
	src/encoder/enc_pad.h \
	src/encoder/enc_vp8_tokens.h \
	src/encoder/enc_recon.h \
	src/encoder/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M09_MODEENC_SRC)

ENC_M09_BPREDENC_SRC := \
	tools/enc_m09_bpredenc.c \
	$(VP8_ENCODER_SHARED_SRC) \
	src/encoder/enc_png.c \
	src/encoder/enc_rgb_to_yuv.c \
	src/encoder/enc_gamma_tables.c \
	src/encoder/enc_pad.c \
	src/encoder/enc_transform.c \
	src/encoder/enc_quant.c \
	src/encoder/enc_quality_table.c \
	src/encoder/enc_vp8_tokens.c \
	src/encoder/enc_loopfilter.c \
	src/encoder/enc_recon.c \
	src/encoder/enc_bool.c \
	src/encoder/enc_riff.c

$(ENC_M09_BPREDENC_BIN): $(ENC_M09_BPREDENC_SRC) \
	src/encoder/enc_png.h \
	src/encoder/enc_rgb_to_yuv.h \
	src/encoder/enc_pad.h \
	src/encoder/enc_vp8_tokens.h \
	src/encoder/enc_recon.h \
	src/encoder/enc_riff.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -o $@ $(ENC_M09_BPREDENC_SRC)

NOLIBC_DECODER_SRC := $(SRC) \
	src/nolibc/syscall_glue.c

NOLIBC_ENCODER_SRC := $(ENCODER_SRC) \
	src/nolibc/syscall_glue.c

NOLIBC_LTO := -flto

NOLIBC_DECODER_OPT_FLAGS := -Os -march=x86-64
NOLIBC_ENCODER_OPT_FLAGS := -Os -march=x86-64
ifeq ($(SPEED),1)
NOLIBC_DECODER_OPT_FLAGS := -O3 -march=x86-64
ifeq ($(NATIVE),1)
NOLIBC_DECODER_OPT_FLAGS := -O3 -march=native
endif
endif

NOLIBC_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-common \
	-ffunction-sections -fdata-sections \
	-DNO_LIBC \
	$(NOLIBC_LTO)

NOLIBC_LDFLAGS := -nostdlib -static \
	-Wl,-e,_start -Wl,--gc-sections -Wl,--build-id=none -s

$(BIN): $(NOLIBC_DECODER_SRC) src/nolibc/start.S
	@mkdir -p $(dir $@)
	$(CC) $(NOLIBC_CFLAGS) $(NOLIBC_DECODER_OPT_FLAGS) -o $@ $(NOLIBC_DECODER_SRC) src/nolibc/start.S $(NOLIBC_LDFLAGS) -lgcc

$(ENCODER): $(NOLIBC_ENCODER_SRC) src/nolibc/start.S
	@mkdir -p $(dir $@)
	$(CC) $(NOLIBC_CFLAGS) $(NOLIBC_ENCODER_OPT_FLAGS) -o $@ $(NOLIBC_ENCODER_SRC) src/nolibc/start.S $(NOLIBC_LDFLAGS) -lgcc

clean:
	rm -rf $(BUILD_DIR) decoder encoder decoder_nolibc encoder_nolibc
