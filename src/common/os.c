#include "os.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int os_map_file_readonly(const char* path, ByteSpan* out_span) {
	if (!out_span) return -1;
	out_span->data = NULL;
	out_span->size = 0;

	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;

	struct stat st;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	if (st.st_size < 0) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	if (st.st_size == 0) {
		close(fd);
		errno = EINVAL;
		return -1;
	}

	size_t size = (size_t)st.st_size;
	void* p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	if (p == MAP_FAILED) return -1;

	out_span->data = (const uint8_t*)p;
	out_span->size = size;
	return 0;
}

void os_unmap_file(ByteSpan span) {
	if (!span.data || span.size == 0) return;
	munmap((void*)span.data, span.size);
}

int os_write_all(int fd, const void* buf, size_t len) {
	const uint8_t* p = (const uint8_t*)buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return -1;
		off += (size_t)n;
	}
	return 0;
}
