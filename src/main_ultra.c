#include "common/os.h"
#include "m01_container/webp_container.h"
#include "m02_vp8_header/vp8_header.h"
#include "m06_recon/vp8_recon.h"

#include <fcntl.h>
#include <unistd.h>

static int cmd_yuv_common(const char* in_path, const char* out_path, int filtered) {
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
	if (!filtered) {
		rc = vp8_reconstruct_keyframe_yuv(&kf, &decoded, &img);
	} else {
		rc = vp8_reconstruct_keyframe_yuv_filtered(&kf, &decoded, &img);
	}
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

	size_t ysz = (size_t)img.stride_y * (size_t)img.height;
	size_t uvh = (size_t)((img.height + 1u) / 2u);
	size_t uvsz = (size_t)img.stride_uv * uvh;
	int wrc = 0;
	wrc |= os_write_all(fd, img.y, ysz);
	wrc |= os_write_all(fd, img.u, uvsz);
	wrc |= os_write_all(fd, img.v, uvsz);
	(void)close(fd);

	yuv420_free(&img);
	vp8_decoded_frame_free(&decoded);
	os_unmap_file(file);
	return (wrc != 0);
}

int main(int argc, char** argv) {
	if (argc != 4) return 2;

	if (argv[1][0] == '-' && argv[1][1] == 'y' && argv[1][2] == 'u' && argv[1][3] == 'v' && argv[1][4] == '\0') {
		return cmd_yuv_common(argv[2], argv[3], 0);
	}
	if (argv[1][0] == '-' && argv[1][1] == 'y' && argv[1][2] == 'u' && argv[1][3] == 'v' && argv[1][4] == 'f' &&
	    argv[1][5] == '\0') {
		return cmd_yuv_common(argv[2], argv[3], 1);
	}
	return 2;
}
