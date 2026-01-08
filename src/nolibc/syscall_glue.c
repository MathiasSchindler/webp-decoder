// Minimal syscall-only glue for Linux x86_64.
// This file intentionally provides a handful of libc-like symbols (open/read/write/...
// + malloc/free/etc) so the existing code can link with -nostdlib.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

// --- errno support (glibc headers typically implement `errno` via __errno_location) ---
int* __errno_location(void) {
	static int e;
	return &e;
}

// --- raw syscall helpers ---
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
	__NR_open = 2,
	__NR_close = 3,
	__NR_lseek = 8,
	__NR_mmap = 9,
	__NR_munmap = 11,
	__NR_fstat = 5,
	__NR_exit = 60,
	__NR_openat = 257,
};

static inline long sys_read(long fd, void* buf, unsigned long len) { return sys_call3(__NR_read, fd, (long)buf, (long)len); }
static inline long sys_write(long fd, const void* buf, unsigned long len) {
	return sys_call3(__NR_write, fd, (long)buf, (long)len);
}
static inline long sys_close(long fd) { return sys_call1(__NR_close, fd); }
static inline long sys_fstat(long fd, struct stat* st) { return sys_call3(__NR_fstat, fd, (long)st, 0); }
static inline long sys_lseek(long fd, long off, long whence) { return sys_call3(__NR_lseek, fd, off, whence); }
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

// --- libc-like syscall wrappers used by the existing code ---

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

off_t lseek(int fd, off_t offset, int whence) {
	long r = sys_lseek(fd, (long)offset, whence);
	if (r < 0) {
		*__errno_location() = (int)-r;
		return (off_t)-1;
	}
	return (off_t)r;
}

int open(const char* pathname, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		// manual varargs: mode is the next stack slot in the SysV ABI
		// Use compiler built-in va_list is okay (header-only), but keep it simple.
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

// --- tiny libc shims (no external libc) ---

void* memcpy(void* dst, const void* src, size_t n) {
	uint8_t* d = (uint8_t*)dst;
	const uint8_t* s = (const uint8_t*)src;
	for (size_t i = 0; i < n; i++) d[i] = s[i];
	return dst;
}

// Some system headers may rewrite memcpy/memset to fortified variants at -O.
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

int memcmp(const void* a, const void* b, size_t n) {
	const uint8_t* x = (const uint8_t*)a;
	const uint8_t* y = (const uint8_t*)b;
	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i]) return (int)x[i] - (int)y[i];
	}
	return 0;
}

size_t strlen(const char* s) {
	size_t n = 0;
	while (s && s[n]) n++;
	return n;
}

// Very small strtoul (base 10 only; sufficient for current usage).
unsigned long strtoul(const char* nptr, char** endptr, int base) {
	(void)base;
	const char* p = nptr;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	unsigned long v = 0;
	int any = 0;
	while (*p >= '0' && *p <= '9') {
		any = 1;
		unsigned long next = v * 10ul + (unsigned long)(*p - '0');
		if (next < v) {
			*__errno_location() = ERANGE;
			break;
		}
		v = next;
		p++;
	}
	if (!any) *__errno_location() = EINVAL;
	if (endptr) *endptr = (char*)p;
	return v;
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
	size_t total;
	if (__builtin_mul_overflow(nmemb, size, &total)) {
		*__errno_location() = ENOMEM;
		return NULL;
	}
	void* p = malloc(total);
	if (!p) return NULL;
	memset(p, 0, total);
	return p;
}
