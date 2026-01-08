#include "fmt.h"

#include "os.h"

static size_t cstr_len(const char* s) {
	size_t n = 0;
	while (s && s[n]) n++;
	return n;
}

void fmt_write_str(int fd, const char* s) {
	if (!s) return;
	os_write_all(fd, s, cstr_len(s));
}

static void write_uint_dec(int fd, uint64_t v) {
	char buf[32];
	size_t i = 0;
	if (v == 0) {
		buf[i++] = '0';
		os_write_all(fd, buf, i);
		return;
	}
	while (v > 0 && i < sizeof(buf)) {
		buf[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	for (size_t j = 0; j < i / 2; j++) {
		char tmp = buf[j];
		buf[j] = buf[i - 1 - j];
		buf[i - 1 - j] = tmp;
	}
	os_write_all(fd, buf, i);
}

void fmt_write_u32(int fd, uint32_t v) { write_uint_dec(fd, v); }
void fmt_write_u64(int fd, uint64_t v) { write_uint_dec(fd, v); }
void fmt_write_size(int fd, size_t v) { write_uint_dec(fd, (uint64_t)v); }

void fmt_write_i32(int fd, int32_t v) {
	if (v < 0) {
		os_write_all(fd, "-", 1);
		// Cast via int64_t to avoid UB on INT32_MIN.
		uint64_t mag = (uint64_t)(-(int64_t)v);
		write_uint_dec(fd, mag);
		return;
	}
	write_uint_dec(fd, (uint64_t)v);
}

void fmt_write_fourcc(int fd, uint32_t fourcc_le) {
	char s[4];
	s[0] = (char)(fourcc_le & 0xFFu);
	s[1] = (char)((fourcc_le >> 8) & 0xFFu);
	s[2] = (char)((fourcc_le >> 16) & 0xFFu);
	s[3] = (char)((fourcc_le >> 24) & 0xFFu);
	os_write_all(fd, s, sizeof(s));
}

void fmt_write_nl(int fd) { os_write_all(fd, "\n", 1); }
