CC ?= cc

BIN := decoder
BUILD_DIR := build
NOLIBC_BUILD_DIR := build/nolibc
NOLIBC_BIN := decoder_nolibc
NOLIBC_TINY_BUILD_DIR := build/nolibc_tiny
NOLIBC_TINY_BIN := decoder_nolibc_tiny
NOLIBC_ULTRA_BUILD_DIR := build/nolibc_ultra
NOLIBC_ULTRA_BIN := decoder_nolibc_ultra

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

.PHONY: all clean nolibc nolibc_tiny nolibc_ultra

all: $(BIN)

nolibc: $(NOLIBC_BIN)

nolibc_tiny: $(NOLIBC_TINY_BIN)

nolibc_ultra: $(NOLIBC_ULTRA_BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS_COMMON) -o $@ $(OBJ) $(LDFLAGS_COMMON)

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

$(NOLIBC_TINY_BIN): $(NOLIBC_TINY_OBJ)
	$(CC) $(NOLIBC_LTO) -o $@ $(NOLIBC_TINY_OBJ) $(NOLIBC_LDFLAGS) -lgcc

NOLIBC_ULTRA_SRC := \
	src/main_ultra.c \
	src/common/os.c \
	src/m01_container/webp_container.c \
	src/m02_vp8_header/vp8_header.c \
	src/m03_bool_decoder/bool_decoder.c \
	src/m04_frame_header_full/vp8_frame_header_basic.c \
	src/m05_tokens/vp8_tree.c \
	src/m05_tokens/vp8_tokens.c \
	src/m06_recon/vp8_recon.c \
	src/m07_loopfilter/vp8_loopfilter.c \
	src/nolibc/syscall_glue.c

NOLIBC_ULTRA_OBJ := $(patsubst src/%.c,$(NOLIBC_ULTRA_BUILD_DIR)/%.o,$(filter %.c,$(NOLIBC_ULTRA_SRC))) \
	$(NOLIBC_ULTRA_BUILD_DIR)/nolibc/start.o

$(NOLIBC_ULTRA_BIN): $(NOLIBC_ULTRA_OBJ)
	$(CC) $(NOLIBC_LTO) -o $@ $(NOLIBC_ULTRA_OBJ) $(NOLIBC_LDFLAGS) -lgcc

$(NOLIBC_ULTRA_BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(NOLIBC_CFLAGS) -c $< -o $@

$(NOLIBC_ULTRA_BUILD_DIR)/nolibc/start.o: src/nolibc/start.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

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
	rm -rf $(BUILD_DIR) $(BIN) $(NOLIBC_BIN) $(NOLIBC_TINY_BIN)
