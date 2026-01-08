#pragma once

#include <stddef.h>
#include <stdint.h>

// Minimal formatting helpers that avoid stdio.

void fmt_write_str(int fd, const char* s);
void fmt_write_u32(int fd, uint32_t v);
void fmt_write_u64(int fd, uint64_t v);
void fmt_write_size(int fd, size_t v);
void fmt_write_i32(int fd, int32_t v);
void fmt_write_fourcc(int fd, uint32_t fourcc_le);
void fmt_write_nl(int fd);
