// Ultra-minimal syscall-only glue for Linux x86_64.
// Only provides the libc-like symbols needed by the nolibc_ultra build.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

int* __errno_location(void) {
	static int e;
	return &e;
}

static inline long sys_call6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile("syscall"
	                 : "=a"(ret)
	                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
	                 : "rcx", "r11", "memory");
	return ret;
}

static inline long sys_call3(long n, long a1, long a2, long a3) { return sys_call6(n, a1, a2, a3, 0, 0, 0); }
static inline long sys_call1(long n, long a1) { return sys_call6(n, a1, 0, 0, 0, 0, 0); }

enum {
	__NR_read = 0,
	__NR_write = 1,
	__NR_close = 3,
	__NR_fstat = 5,
	__NR_mmap = 9,
	__NR_munmap = 11,
	__NR_exit = 60,
	__NR_openat = 257,
};

static inline long sys_read(long fd, void* buf, unsigned long len) { return sys_call3(__NR_read, fd, (long)buf, (long)len); }
static inline long sys_write(long fd, const void* buf, unsigned long len) {
	return sys_call3(__NR_write, fd, (long)buf, (long)len);
}
static inline long sys_close(long fd) { return sys_call1(__NR_close, fd); }
static inline long sys_fstat(long fd, struct stat* st) { return sys_call3(__NR_fstat, fd, (long)st, 0); }
static inline long sys_openat(long dirfd, const char* path, long flags, long mode) {
	return sys_call6(__NR_openat, dirfd, (long)path, flags, mode, 0, 0);
}
static inline long sys_mmap(void* addr, size_t len, long prot, long flags, long fd, long off) {
	return sys_call6(__NR_mmap, (long)addr, (long)len, prot, flags, fd, off);
}
static inline long sys_munmap(void* addr, size_t len) { return sys_call3(__NR_munmap, (long)addr, (long)len, 0); }

__attribute__((noreturn)) void _exit(int code) {
	(void)sys_call1(__NR_exit, code);
	__builtin_unreachable();
}

ssize_t read(int fd, void* buf, size_t count) {
	long r = sys_read(fd, buf, (unsigned long)count);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return -1;
	}
	return (ssize_t)r;
}

ssize_t write(int fd, const void* buf, size_t count) {
	long r = sys_write(fd, buf, (unsigned long)count);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return -1;
	}
	return (ssize_t)r;
}

int close(int fd) {
	long r = sys_close(fd);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return -1;
	}
	return 0;
}

int fstat(int fd, struct stat* st) {
	long r = sys_fstat(fd, st);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return -1;
	}
	return 0;
}

int open(const char* pathname, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		__builtin_va_list ap;
		__builtin_va_start(ap, flags);
		mode = (mode_t)__builtin_va_arg(ap, int);
		__builtin_va_end(ap);
	}
	long r = sys_openat(-100 /*AT_FDCWD*/, pathname, flags, mode);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return -1;
	}
	return (int)r;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
	long r = sys_mmap(addr, length, prot, flags, fd, (long)offset);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return MAP_FAILED;
	}
	return (void*)r;
}

int munmap(void* addr, size_t length) {
	long r = sys_munmap(addr, length);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return -1;
	}
	return 0;
}

void* memcpy(void* dst, const void* src, size_t n) {
	uint8_t* d = (uint8_t*)dst;
	const uint8_t* s = (const uint8_t*)src;
	for (size_t i = 0; i < n; i++) d[i] = s[i];
	return dst;
}

void* __memcpy_chk(void* dst, const void* src, size_t n, size_t dstlen) {
	(void)dstlen;
	return memcpy(dst, src, n);
}

void* memset(void* dst, int c, size_t n) {
	uint8_t* d = (uint8_t*)dst;
	uint8_t v = (uint8_t)c;
	for (size_t i = 0; i < n; i++) d[i] = v;
	return dst;
}

void* __memset_chk(void* dst, int c, size_t n, size_t dstlen) {
	(void)dstlen;
	return memset(dst, c, n);
}

static size_t align16(size_t n) { return (n + 15u) & ~(size_t)15u; }

void* malloc(size_t size) {
	if (size == 0) size = 1;
	size_t total = align16(size + sizeof(size_t));
	void* p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) return NULL;
	*(size_t*)p = total;
	return (uint8_t*)p + sizeof(size_t);
}

void free(void* ptr) {
	if (!ptr) return;
	uint8_t* base = (uint8_t*)ptr - sizeof(size_t);
	size_t total = *(size_t*)base;
	(void)munmap(base, total);
}

void* calloc(size_t nmemb, size_t size) {
	if (nmemb && size > (SIZE_MAX / nmemb)) return NULL;
	size_t total = nmemb * size;
	void* p = malloc(total);
	if (!p) return NULL;
	return memset(p, 0, total);
}
