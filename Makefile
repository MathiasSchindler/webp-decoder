CC ?= cc

BIN := decoder
BUILD_DIR := build

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

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS_COMMON) -o $@ $(OBJ) $(LDFLAGS_COMMON)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN)
