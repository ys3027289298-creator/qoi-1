/* Deterministic test materials for the streaming encoder tests.

No files, no external libraries, no randomness beyond a fixed-seed xorshift.
Every generator is a pure function of (width, height, channels, kind), which
matters for the resume tests: the second run must regenerate exactly the same
pixels without storing them anywhere. */

#ifndef QOI_TEST_MATERIALS_H
#define QOI_TEST_MATERIALS_H

#include <stdlib.h>
#include <string.h>

/* Kinds chosen so every QOI chunk type gets exercised:
0 flat (runs), 1 gradient (diff/luma), 2 noise (rgb/rgba), 3 pixel art
(index hits and runs), 4 alpha ramp (rgba chunks). */
#define QOI_TEST_KINDS 5

static unsigned int qoi_test_rand(unsigned int *s) {
	unsigned int x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

/* Row at a time, so the resume test can regenerate any slice of the image. */
static void qoi_test_row(
	unsigned char *row, int w, int y, int channels, int kind
) {
	static const unsigned char palette[8][4] = {
		{  0,   0,   0, 255}, {255, 255, 255, 255}, { 34,  32,  52, 255},
		{ 69,  40,  60, 255}, {102,  57,  49, 255}, {143,  86,  59, 255},
		{223, 113,  38, 255}, {217, 160, 102, 255}
	};
	unsigned int s = (unsigned int)(0x9e3779b9u * (unsigned int)(y + 1)) ^ 0x85ebca6bu;
	int x;

	for (x = 0; x < w; x++) {
		unsigned char *q = row + (size_t)x * channels;
		if (kind == 0) {
			q[0] = 17; q[1] = 34; q[2] = 51;
		}
		else if (kind == 1) {
			q[0] = (unsigned char)((x * 255) / (w > 1 ? w - 1 : 1));
			q[1] = (unsigned char)(((y & 255) * 255) / 255);
			q[2] = (unsigned char)((x + y) & 255);
		}
		else if (kind == 2) {
			unsigned int r = qoi_test_rand(&s);
			q[0] = (unsigned char)r;
			q[1] = (unsigned char)(r >> 8);
			q[2] = (unsigned char)(r >> 16);
			if (channels == 4) {
				q[3] = (unsigned char)(qoi_test_rand(&s) >> 24);
			}
			continue;
		}
		else if (kind == 3) {
			int idx = ((x / 8) + (y / 8)) % 3;
			if ((x % 8) == 0 && (y % 8) == 0) {
				idx = 7;
			}
			q[0] = palette[idx][0];
			q[1] = palette[idx][1];
			q[2] = palette[idx][2];
			if (channels == 4) {
				q[3] = palette[idx][3];
			}
			continue;
		}
		else {
			q[0] = (unsigned char)(x & 255);
			q[1] = (unsigned char)(y & 255);
			q[2] = (unsigned char)((x * y) & 255);
			if (channels == 4) {
				q[3] = (unsigned char)((x >> 3) & 255);
			}
			continue;
		}
		if (channels == 4) {
			q[3] = 255;
		}
	}
}

/* Fill a standalone buffer. */
static unsigned char *qoi_test_image(int w, int h, int channels, int kind) {
	unsigned char *p = (unsigned char *)malloc((size_t)w * (size_t)h * (size_t)channels);
	int y;
	if (!p) {
		return NULL;
	}
	for (y = 0; y < h; y++) {
		qoi_test_row(p + (size_t)y * w * channels, w, y, channels, kind);
	}
	return p;
}

#endif
