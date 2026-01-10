#pragma once

#include <stddef.h>
#include <stdint.h>

#include <stdio.h>

typedef struct QualityPpmImage {
	uint32_t width;
	uint32_t height;
	uint8_t* rgb; /* RGB24, width*height*3 bytes */
} QualityPpmImage;

/*
Reads a binary P6 PPM file.

Constraints:
- maxval must be 255
- output is RGB24

Returns 0 on success, -1 on failure.
*/
int quality_ppm_read_file(const char* path, QualityPpmImage* out_img);

/*
Reads a binary P6 PPM from an already-open stream.

The stream must be positioned at the start of the file.
This supports non-seekable inputs (pipes, /dev/fd/N).
*/
int quality_ppm_read_stream(FILE* f, QualityPpmImage* out_img);

void quality_ppm_free(QualityPpmImage* img);