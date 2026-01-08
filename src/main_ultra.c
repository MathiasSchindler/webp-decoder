
#include "common/os.h"
#include "m01_container/webp_container.h"
#include "m02_vp8_header/vp8_header.h"
#include "m06_recon/vp8_recon.h"
#include "m09_png/yuv2rgb_png.h"

#include <fcntl.h>
#include <unistd.h>

static int cmd_png(const char* in_path, const char* out_path) {
	ByteSpan file;
	if (os_map_file_readonly(in_path, &file) != 0) {
		return 1;
	}

	WebPContainer c;
	int rc = webp_parse_simple_lossy(file, &c);
	if (rc != 0) {
		os_unmap_file(file);
		return 1;
	}

	ByteSpan vp8_payload = {
		.data = file.data + c.vp8_chunk_offset,
		.size = c.vp8_chunk_size,
	};

	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0 || !kf.is_key_frame) {
		os_unmap_file(file);
		return 1;
	}

	Vp8DecodedFrame decoded;
	if (vp8_decode_decoded_frame(vp8_payload, &decoded) != 0) {
		os_unmap_file(file);
		return 1;
	}

	Yuv420Image img;
	// Match dwebp default output: filtered reconstruction.
	rc = vp8_reconstruct_keyframe_yuv_filtered(&kf, &decoded, &img);
	if (rc != 0) {
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		yuv420_free(&img);
		vp8_decoded_frame_free(&decoded);
		os_unmap_file(file);
		return 1;
	}

	int wrc = yuv420_write_png_fd(fd, &img);
	(void)close(fd);

	yuv420_free(&img);
	vp8_decoded_frame_free(&decoded);
	os_unmap_file(file);
	return (wrc != 0);
}

int main(int argc, char** argv) {
	if (argc != 3) return 2;
	return cmd_png(argv[1], argv[2]);
}
