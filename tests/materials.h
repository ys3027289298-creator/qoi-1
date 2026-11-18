/* Deterministic, in-memory test materials for the configurable cache tests.

No files, no randomness beyond a fixed-seed xorshift, no external libraries.
Each generator is a pure function of (width, height, channels); RGBA pixels are
written in channel order, with alpha set explicitly for channels == 4. */

#ifndef QOI_TEST_MATERIALS_H
#define QOI_TEST_MATERIALS_H

#include <stdlib.h>
#include <string.h>

static unsigned int qoi_test_rand(unsigned int *s) {
	unsigned int x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static unsigned char *qoi_test_alloc(int w, int h, int channels) {
	return (unsigned char *)malloc((size_t)w * (size_t)h * (size_t)channels);
}

/* Pixel art: a small fixed palette, 8x8 logical blocks, flat areas and repeated
colors. Hash slots are reused heavily, so index hits are common. */
static unsigned char *qoi_test_pixelart(int w, int h, int channels) {
	static const unsigned char palette[12][4] = {
		{  0,   0,   0, 255}, {255, 255, 255, 255}, { 34,  32,  52, 255},
		{ 69,  40,  60, 255}, {102,  57,  49, 255}, {143,  86,  59, 255},
		{223, 113,  38, 255}, {217, 160, 102, 255}, {238, 195, 154, 255},
		{251, 242,  54, 255}, { 99, 155,  95, 255}, { 95, 205, 228, 255}
	};
	unsigned char *p = qoi_test_alloc(w, h, channels);
	int x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int bx = x / 8, by = y / 8;
			int idx;
			/* A hand-drawn-ish pattern: bands, a checker and a flat field. */
			if (by < 3) {
				idx = (bx + by) % 3;
			}
			else if (by < 6) {
				idx = ((bx / 2) + (by / 2)) % 2 ? 9 : 10;
			}
			else {
				idx = (bx / 3 + by) % 4 + 2;
			}
			if ((x % 8) == 0 && (y % 8) == 0) {
				idx = 1; /* block corner highlight */
			}
			p[(y * w + x) * channels + 0] = palette[idx][0];
			p[(y * w + x) * channels + 1] = palette[idx][1];
			p[(y * w + x) * channels + 2] = palette[idx][2];
			if (channels == 4) {
				p[(y * w + x) * channels + 3] = palette[idx][3];
			}
		}
	}
	return p;
}

/* Smooth two-axis gradient; every pixel differs from its neighbors by a small
delta, so DIFF/LUMA dominate and colors rarely repeat. */
static unsigned char *qoi_test_gradient(int w, int h, int channels) {
	unsigned char *p = qoi_test_alloc(w, h, channels);
	int x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int r = w > 1 ? x * 255 / (w - 1) : 0;
			int g = h > 1 ? y * 255 / (h - 1) : 0;
			int b = (x + y) * 255 / (w + h > 2 ? w + h - 2 : 1);
			p[(y * w + x) * channels + 0] = (unsigned char)r;
			p[(y * w + x) * channels + 1] = (unsigned char)g;
			p[(y * w + x) * channels + 2] = (unsigned char)b;
			if (channels == 4) {
				p[(y * w + x) * channels + 3] = 255;
			}
		}
	}
	return p;
}

/* Photo-like: slow low-frequency structure plus deterministic per-channel
grain. Lots of distinct colors, occasional repeats in flat areas. */
static unsigned char *qoi_test_photo(int w, int h, int channels) {
	unsigned char *p = qoi_test_alloc(w, h, channels);
	unsigned int seed = 0x9e3779b9u;
	int x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int base_r = 90 + 70 * x / (w > 1 ? w - 1 : 1);
			int base_g = 80 + 60 * y / (h > 1 ? h - 1 : 1);
			int base_b = 110 + 50 * (x + y) / (w + h > 2 ? w + h - 2 : 1);
			int grain = (int)(qoi_test_rand(&seed) >> 24) % 11 - 5;
			int r = base_r + grain;
			int g = base_g + (int)(qoi_test_rand(&seed) >> 24) % 11 - 5;
			int b = base_b + (int)(qoi_test_rand(&seed) >> 24) % 11 - 5;
			p[(y * w + x) * channels + 0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
			p[(y * w + x) * channels + 1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
			p[(y * w + x) * channels + 2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
			if (channels == 4) {
				p[(y * w + x) * channels + 3] = 255;
			}
		}
	}
	return p;
}

/* Uniform random colors: no locality and (almost) no repeats, so the cache hit
rate is expected to be ~0 for every configuration. */
static unsigned char *qoi_test_random(int w, int h, int channels) {
	unsigned char *p = qoi_test_alloc(w, h, channels);
	unsigned int seed = 0x12345678u;
	int i, n = w * h;
	for (i = 0; i < n; i++) {
		unsigned int v = qoi_test_rand(&seed);
		p[i * channels + 0] = (unsigned char)(v);
		p[i * channels + 1] = (unsigned char)(v >> 8);
		p[i * channels + 2] = (unsigned char)(v >> 16);
		if (channels == 4) {
			p[i * channels + 3] = (unsigned char)(v >> 24);
		}
	}
	return p;
}

typedef unsigned char *(*qoi_test_gen_fn)(int, int, int);

static qoi_test_gen_fn qoi_test_gen(int material) {
	switch (material) {
		case 0: return qoi_test_pixelart;
		case 1: return qoi_test_gradient;
		case 2: return qoi_test_photo;
		default: return qoi_test_random;
	}
}

static const char *qoi_test_name(int material) {
	switch (material) {
		case 0: return "pixelart";
		case 1: return "gradient";
		case 2: return "photo";
		default: return "random";
	}
}

#endif
