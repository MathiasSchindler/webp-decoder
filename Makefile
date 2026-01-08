CC ?= cc

BIN := decoder
BUILD_DIR := build
NOLIBC_BUILD_DIR := build/nolibc
NOLIBC_BIN := decoder_nolibc

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

.PHONY: all clean nolibc

all: $(BIN)

nolibc: $(NOLIBC_BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS_COMMON) -o $@ $(OBJ) $(LDFLAGS_COMMON)

NOLIBC_SRC := $(SRC) \
	src/nolibc/syscall_glue.c

NOLIBC_OBJ := $(patsubst src/%.c,$(NOLIBC_BUILD_DIR)/%.o,$(filter %.c,$(NOLIBC_SRC))) \
	$(NOLIBC_BUILD_DIR)/nolibc/start.o

NOLIBC_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-O3 -march=native \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-omit-frame-pointer -fno-common \
	-ffunction-sections -fdata-sections \
	-DNO_LIBC

NOLIBC_LDFLAGS := -nostdlib -static \
	-Wl,-e,_start -Wl,--gc-sections -Wl,--build-id=none -s

$(NOLIBC_BIN): $(NOLIBC_OBJ)
	$(CC) -o $@ $(NOLIBC_OBJ) $(NOLIBC_LDFLAGS) -lgcc

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
	rm -rf $(BUILD_DIR) $(BIN) $(NOLIBC_BIN)
