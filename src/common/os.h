#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
	const uint8_t* data;
	size_t size;
} ByteSpan;

// Maps a file read-only. Returns 0 on success.
int os_map_file_readonly(const char* path, ByteSpan* out_span);
void os_unmap_file(ByteSpan span);

// Writes all bytes to fd. Returns 0 on success.
int os_write_all(int fd, const void* buf, size_t len);
