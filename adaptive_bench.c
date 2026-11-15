/*

Sample / benchmark for the adaptive block prediction layer in qoi.h.

Generates two small synthetic test images (a photo-like gradient and a
pixel-art image), encodes them with a fixed predictor and with adaptive
16x16 block prediction, and reports the size ratio, the block header
overhead, encode/decode timings and the roundtrip consistency. Also checks
that invalid block sizes are rejected.

Compile and run with:
	gcc -std=c99 -O2 adaptive_bench.c -o adaptive_bench -lm && ./adaptive_bench

*/

#define _POSIX_C_SOURCE 199309L

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLOCK_SIZE 16
#define REPS 100

static unsigned int rng_state = 0x9e3779b9u;
static unsigned int rng(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int clamp8(int v) {
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* Photo-like material: smooth gradients, soft waves and a little noise. */
static void gen_photo(unsigned char *img, int w, int h, int ch) {
	int x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int i = (y * w + x) * ch;
			int noise = (int)(rng() % 17) - 8;
			img[i + 0] = clamp8(128 +
				(int)(70.0 * sin(x * 0.045) * cos(y * 0.031)) + noise);
			img[i + 1] = clamp8(128 +
				(int)(60.0 * sin((x + y) * 0.021)) + noise / 2);
			img[i + 2] = clamp8(128 +
				(int)(90.0 * cos(x * 0.012 + y * 0.037)) + noise);
			if (ch == 4) {
				img[i + 3] = 255;
			}
		}
	}
}

/* Pixel-art material: small palette, hard edges, flat regions. */
static void gen_pixelart(unsigned char *img, int w, int h, int ch) {
	static const unsigned char pal[12][3] = {
		{ 24,  24,  40}, {240, 240, 216}, {220,  48,  48},
		{ 48, 168,  72}, { 64,  96, 224}, {248, 168,  32},
		{144,  72, 184}, { 56, 200, 200}, {128,  88,  48},
		{252, 120, 168}, { 88,  88, 104}, {180, 240,  80}
	};
	int x, y, i, r;
	for (i = 0; i < w * h; i++) {
		img[i * ch + 0] = pal[0][0];
		img[i * ch + 1] = pal[0][1];
		img[i * ch + 2] = pal[0][2];
		if (ch == 4) {
			img[i * ch + 3] = 255;
		}
	}
	for (r = 0; r < 48; r++) {
		int rw = 8 + (int)(rng() % 40);
		int rh = 8 + (int)(rng() % 40);
		int rx = (int)(rng() % (unsigned int)(w - rw));
		int ry = (int)(rng() % (unsigned int)(h - rh));
		const unsigned char *col = pal[1 + rng() % 11];
		for (y = ry; y < ry + rh; y++) {
			for (x = rx; x < rx + rw; x++) {
				int p = (y * w + x) * ch;
				img[p + 0] = col[0];
				img[p + 1] = col[1];
				img[p + 2] = col[2];
			}
		}
	}
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if ((x / 8 + y / 8) % 2 == 0 && x > w / 2 && y > h / 2) {
				int p = (y * w + x) * ch;
				img[p + 0] = pal[1][0];
				img[p + 1] = pal[1][1];
				img[p + 2] = pal[1][2];
			}
		}
	}
}

static void bench(const char *name, const unsigned char *img,
	int w, int h, int ch)
{
	qoi_desc desc, dd;
	void *fixed, *adaptive, *plain, *dec;
	int flen, alen, qlen, i, blocks, ok_fixed, ok_adaptive;
	double t0, t1, enc_fixed, enc_adaptive, dec_fixed, dec_adaptive;

	desc.width = w;
	desc.height = h;
	desc.channels = ch;
	desc.colorspace = QOI_SRGB;

	fixed = qoi_encode_predictive(img, &desc, QOI_PREDICTOR_LEFT, &flen);
	adaptive = qoi_encode_adaptive(img, &desc, BLOCK_SIZE, &alen);
	plain = qoi_encode(img, &desc, &qlen);
	if (!fixed || !adaptive || !plain) {
		printf("%s: encode failed\n", name);
		exit(1);
	}

	dec = qoi_decode_predictive(fixed, flen, &dd, 0);
	ok_fixed = dec && memcmp(dec, img, (size_t)w * h * ch) == 0;
	free(dec);
	dec = qoi_decode_predictive(adaptive, alen, &dd, 0);
	ok_adaptive = dec && memcmp(dec, img, (size_t)w * h * ch) == 0;
	free(dec);

	t0 = now_ms();
	for (i = 0; i < REPS; i++) {
		int l;
		void *e = qoi_encode_predictive(img, &desc, QOI_PREDICTOR_LEFT, &l);
		free(e);
	}
	t1 = now_ms();
	enc_fixed = (t1 - t0) / REPS;

	t0 = now_ms();
	for (i = 0; i < REPS; i++) {
		int l;
		void *e = qoi_encode_adaptive(img, &desc, BLOCK_SIZE, &l);
		free(e);
	}
	t1 = now_ms();
	enc_adaptive = (t1 - t0) / REPS;

	t0 = now_ms();
	for (i = 0; i < REPS; i++) {
		void *d = qoi_decode_predictive(fixed, flen, &dd, 0);
		free(d);
	}
	t1 = now_ms();
	dec_fixed = (t1 - t0) / REPS;

	t0 = now_ms();
	for (i = 0; i < REPS; i++) {
		void *d = qoi_decode_predictive(adaptive, alen, &dd, 0);
		free(d);
	}
	t1 = now_ms();
	dec_adaptive = (t1 - t0) / REPS;

	blocks = ((w + BLOCK_SIZE - 1) / BLOCK_SIZE) *
		((h + BLOCK_SIZE - 1) / BLOCK_SIZE);

	printf("=== %s (%dx%d, %d channels) ===\n", name, w, h, ch);
	printf("size fixed predictor (left)   : %7d bytes\n", flen);
	printf("size adaptive (%2dx%2d blocks)  : %7d bytes\n",
		BLOCK_SIZE, BLOCK_SIZE, alen);
	printf("size plain qoi (reference)    : %7d bytes\n", qlen);
	printf("size ratio adaptive / fixed   : %7.4f\n",
		(double)alen / flen);
	printf("block header overhead         : %7d bytes "
		"(%d blocks x 1 byte, %.2f%% of adaptive stream)\n",
		blocks, blocks, 100.0 * blocks / alen);
	printf("encode time fixed / adaptive  : %.3f / %.3f ms\n",
		enc_fixed, enc_adaptive);
	printf("decode time fixed / adaptive  : %.3f / %.3f ms\n",
		dec_fixed, dec_adaptive);
	printf("roundtrip fixed / adaptive    : %s / %s\n\n",
		ok_fixed ? "PASS" : "FAIL", ok_adaptive ? "PASS" : "FAIL");

	free(fixed);
	free(adaptive);
	free(plain);

	if (!ok_fixed || !ok_adaptive) {
		exit(1);
	}
}

int main(void) {
	int w1 = 256, h1 = 256, ch = 3;
	int w2 = 200, h2 = 152;
	unsigned char *photo = malloc((size_t)w1 * h1 * ch);
	unsigned char *pixelart = malloc((size_t)w2 * h2 * ch);
	qoi_desc desc;
	int len, i, ok;
	static const int bad_sizes[] = {-1, 0, 257, 1000000};
	static const int good_sizes[] = {1, 7, 16, 256};

	if (!photo || !pixelart) {
		return 1;
	}

	gen_photo(photo, w1, h1, ch);
	gen_pixelart(pixelart, w2, h2, ch);

	bench("photo", photo, w1, h1, ch);
	bench("pixel art", pixelart, w2, h2, ch);

	printf("=== block size validation ===\n");
	desc.width = w1;
	desc.height = h1;
	desc.channels = ch;
	desc.colorspace = QOI_SRGB;
	for (i = 0; i < 4; i++) {
		void *e = qoi_encode_adaptive(photo, &desc, bad_sizes[i], &len);
		printf("block_size %7d -> %s\n", bad_sizes[i],
			e ? "accepted (ERROR)" : "rejected");
		if (e) {
			free(e);
			return 1;
		}
	}
	for (i = 0; i < 4; i++) {
		qoi_desc dd;
		void *e = qoi_encode_adaptive(photo, &desc, good_sizes[i], &len);
		void *d = e ? qoi_decode_predictive(e, len, &dd, 0) : NULL;
		ok = d && memcmp(d, photo, (size_t)w1 * h1 * ch) == 0;
		printf("block_size %7d -> roundtrip %s\n", good_sizes[i],
			ok ? "PASS" : "FAIL");
		free(e);
		free(d);
		if (!ok) {
			return 1;
		}
	}

	free(photo);
	free(pixelart);
	return 0;
}
