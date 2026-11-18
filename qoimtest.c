/*

QOI-Mask (qoim) test & benchmark harness.

Self contained: generates four small synthetic RGBA images in memory, no
external files or third party libraries. Run with:

	gcc -std=c99 -O2 -Wall -Wextra qoimtest.c -o qoimtest && ./qoimtest

Exit code is non-zero if any check fails.

*/

#define _POSIX_C_SOURCE 199309L

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------------- */
/* Tiny deterministic PRNG so the samples are reproducible.                  */

static uint32_t rng_state = 0x12345678u;
static uint32_t xrnd(void) {
	uint32_t x = rng_state;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	rng_state = x;
	return x;
}

/* ------------------------------------------------------------------------- */
/* Timing (monotonic nanoseconds).                                           */

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------- */
/* The four sample materials, 64x64 RGBA (= 16 KiB raw each).               */

#define IMG_W 64
#define IMG_H 64
#define IMG_CH 4
#define IMG_PX (IMG_W * IMG_H)

typedef enum {
	MAT_GRAY_COLOR = 0, /* color image converted to grayscale: r=g=b        */
	MAT_PURE_ALPHA,     /* rgb constant (0,0,0), only alpha varies          */
	MAT_RGB_EQUAL,      /* r=g=b per pixel but the gray level varies a lot  */
	MAT_PHOTO,          /* ordinary photo-like noise/smooth gradients        */
	MAT_COUNT
} material_t;

static const char *mat_name[MAT_COUNT] = {
	[MAT_GRAY_COLOR] = "grayscale-color (r=g=b; flat bands + ramp, alpha 255)",
	[MAT_PURE_ALPHA] = "pure-alpha (rgb fixed, alpha varies, small noise)",
	[MAT_RGB_EQUAL]  = "rgb-equal noisy gray (r=g=b, random levels)",
	[MAT_PHOTO]      = "ordinary photo-like (all four channels vary)",
};

static unsigned char *make_material(material_t m) {
	unsigned char *px = (unsigned char *)malloc((size_t)IMG_PX * IMG_CH);
	int x, y;
	if (!px) { exit(1); }
	for (y = 0; y < IMG_H; y++) {
		for (x = 0; x < IMG_W; x++) {
			int i = (y * IMG_W + x) * IMG_CH;
			switch (m) {
				case MAT_GRAY_COLOR: {
					/* Left half: a few perfectly flat gray bands (whole blocks
					constant). Right half: a smooth per-column ramp so blocks on
					the boundary vary. r=g=b throughout, alpha fixed at 255. */
					int g = (x < IMG_W / 2)
						? (y / 16) * 40
						: ((x * 3 + y * 2) & 0xff);
					px[i] = px[i+1] = px[i+2] = (unsigned char)g;
					px[i+3] = 255;
					break;
				}
				case MAT_PURE_ALPHA: {
					/* RGB is globally constant. Alpha is a slow ramp with
					+-3 noise: constant under tolerance >= 6, varying under 0. */
					px[i] = 10; px[i+1] = 20; px[i+2] = 30;
					px[i+3] = (unsigned char)(((x * 2 + y) & 0xc0)
						+ (int)(xrnd() % 7) - 3);
					break;
				}
				case MAT_RGB_EQUAL: {
					/* Random gray level per pixel; r=g=b but blocks almost
					never constant. Alpha fixed at 255. */
					unsigned char g = (unsigned char)(xrnd() & 0xff);
					px[i] = px[i+1] = px[i+2] = g;
					px[i+3] = 255;
					break;
				}
				case MAT_PHOTO: {
					/* Smooth gradients + independent noise on every channel;
					alpha also moves, so essentially nothing is constant. */
					px[i]   = (unsigned char)((x * 4 + (xrnd() & 7)) & 0xff);
					px[i+1] = (unsigned char)((y * 4 + (xrnd() & 7)) & 0xff);
					px[i+2] = (unsigned char)(((x + y) * 2 + (xrnd() & 7)) & 0xff);
					px[i+3] = (unsigned char)(200 + (xrnd() & 31));
					break;
				}
				default:
					memset(px, 0, (size_t)IMG_PX * IMG_CH);
			}
		}
	}
	return px;
}

/* ------------------------------------------------------------------------- */
/* Test bookkeeping.                                                         */

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { \
		tests_failed++; \
		printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
	} \
} while (0)

/* ------------------------------------------------------------------------- */
/* Roundtrip error metrics.                                                  */

static void pixel_errors(const unsigned char *a, const unsigned char *b,
	int n, int ch, long long *max_abs, double *sum_sq) {
	int i;
	*max_abs = 0;
	*sum_sq = 0.0;
	for (i = 0; i < n * ch; i++) {
		long long d = (long long)a[i] - (long long)b[i];
		long long ad = d < 0 ? -d : d;
		if (ad > *max_abs) { *max_abs = ad; }
		*sum_sq += (double)d * (double)d;
	}
}

/* Integer-free sqrt via Newton iteration, so no -lm is needed. */
static double isqrt(double x) {
	double r = x > 0.0 ? x : 1.0;
	int k;
	if (x <= 0.0) { return 0.0; }
	for (k = 0; k < 32; k++) {
		double nr = 0.5 * (r + x / r);
		if (nr == r) { break; }
		r = nr;
	}
	return r;
}

/* ------------------------------------------------------------------------- */
/* Corruption tests: a mask that disagrees with the block data must error.   */

static void test_corruption(const unsigned char *good, int good_len) {
	unsigned char *buf;
	qoi_desc desc;
	qoim_status st;
	int hc, tc, hb, tb;

	/* 1. truncated stream */
	buf = (unsigned char *)malloc((size_t)good_len - 9);
	memcpy(buf, good, (size_t)good_len - 9);
	CHECK(qoim_decode(buf, good_len - 9, &desc, 0, &st) == NULL
		&& st == QOIM_ERR_TRUNCATED, "truncated stream rejected");
	CHECK(qoim_check(buf, good_len - 9) != QOIM_OK, "check() rejects truncation");
	free(buf);

	/* 2. bad magic */
	buf = (unsigned char *)malloc((size_t)good_len);
	memcpy(buf, good, (size_t)good_len);
	buf[0] = 'x';
	CHECK(qoim_decode(buf, good_len, &desc, 0, &st) == NULL
		&& st == QOIM_ERR_MAGIC, "bad magic rejected");
	free(buf);

	/* 3. garbage appended after end marker */
	buf = (unsigned char *)malloc((size_t)good_len + 3);
	memcpy(buf, good, (size_t)good_len);
	buf[good_len] = buf[good_len+1] = buf[good_len+2] = 0;
	CHECK(qoim_decode(buf, good_len + 3, &desc, 0, &st) == NULL
		&& st == QOIM_ERR_ENDMARKER, "trailing garbage rejected");
	free(buf);

	/* 4. reserved mask bit set -> mask inconsistency */
	buf = (unsigned char *)malloc((size_t)good_len);
	memcpy(buf, good, (size_t)good_len);
	buf[16] |= 0x80; /* first block's mask, reserved bit */
	CHECK(qoim_decode(buf, good_len, &desc, 0, &st) == NULL
		&& st == QOIM_ERR_MASK, "illegal mask bit rejected");
	CHECK(qoim_check(buf, good_len) == QOIM_ERR_MASK, "check() flags illegal mask bit");
	free(buf);

	/* 5. mask says a channel varies but its per-pixel payload bytes are
	      removed by shortening the stream -> truncation, never a silent
	      misparse. Flip one constant bit to varying without adding data. */
	buf = (unsigned char *)malloc((size_t)good_len);
	memcpy(buf, good, (size_t)good_len);
	if (!(buf[16] & 0x01)) {
		buf[16] |= 0x01;
		/* the representative byte that followed the mask now reads as
		   pixel data; lengths no longer line up, must be caught */
		CHECK(qoim_decode(buf, good_len, &desc, 0, &st) == NULL,
			"mask/payload mismatch (extra varying channel) rejected");
		CHECK(qoim_check(buf, good_len) != QOIM_OK,
			"check() flags mask/payload mismatch");
	}
	free(buf);

	/* 6. stats on a damaged stream must report an error, never fake numbers */
	buf = (unsigned char *)malloc((size_t)good_len);
	memcpy(buf, good, (size_t)good_len);
	buf[16] |= 0x80;
	CHECK(qoim_stats(buf, good_len, &hc, &tc, &hb, &tb) == QOIM_ERR_MASK,
		"stats() rejects inconsistent mask");
	free(buf);

	/* 7. argument validation */
	CHECK(qoim_decode(NULL, good_len, &desc, 0, &st) == NULL
		&& st == QOIM_ERR_ARG, "NULL data rejected");
	{
		qoim_options bad = { 0, 0 };
		CHECK(qoim_encode(good, &(qoi_desc){IMG_W, IMG_H, 4, QOI_SRGB},
			&bad, &(int){0}) == NULL, "block_sz 0 rejected by encoder");
	}
}

/* ------------------------------------------------------------------------- */
/* Main per-material, per-configuration evaluation.                          */

typedef struct {
	int block_sz;
	int tolerance;
} config_t;

int main(void) {
	static const config_t configs[] = {
		{ 4, 0 }, { 8, 0 }, { 8, 6 }, { 16, 0 }, { 16, 6 }
	};
	static const int nconfigs = (int)(sizeof(configs) / sizeof(configs[0]));
	const int time_iters = 200;
	int m, ci, i;
	size_t raw_size = (size_t)IMG_PX * IMG_CH;
	int overall_fail_at_start;

	printf("QOI-Mask test & benchmark -- %dx%d RGBA, raw size %zu bytes\n\n",
		IMG_W, IMG_H, raw_size);

	for (m = 0; m < MAT_COUNT; m++) {
		unsigned char *src = make_material((material_t)m);
		printf("=== %s ===\n", mat_name[m]);
		printf(" block tol | enc bytes  ratio   | maxAbs rmse   | enc ms   dec ms   | ch-hit   blk-hit\n");
		printf(" --------+----------------------+---------------+------------------+----------------\n");

		for (ci = 0; ci < nconfigs; ci++) {
			qoim_options opt = {
				(unsigned char)configs[ci].block_sz,
				(unsigned char)configs[ci].tolerance
			};
			qoi_desc edesc = { IMG_W, IMG_H, IMG_CH, QOI_SRGB };
			qoi_desc ddesc;
			qoim_status st;
			int enc_len = 0;
			unsigned char *enc;
			unsigned char *dec;
			long long max_abs;
			double sum_sq, rmse;
			int hc = 0, tc = 0, hb = 0, tb = 0;
			uint64_t t0, t1, enc_ns = 0, dec_ns = 0;

			enc = qoim_encode(src, &edesc, &opt, &enc_len);
			CHECK(enc != NULL, "encode succeeds");
			CHECK(enc_len > 16, "encoded stream has blocks");

			/* timing: warm + timed iterations */
			for (i = 0; i < time_iters; i++) {
				int L = 0;
				unsigned char *e;
				t0 = now_ns();
				e = qoim_encode(src, &edesc, &opt, &L);
				t1 = now_ns();
				enc_ns += t1 - t0;
				free(e);
			}
			for (i = 0; i < time_iters; i++) {
				t0 = now_ns();
				dec = qoim_decode(enc, enc_len, &ddesc, 0, &st);
				t1 = now_ns();
				dec_ns += t1 - t0;
				if (i < time_iters - 1) { free(dec); }
			}
			CHECK(st == QOIM_OK && dec != NULL, "decode succeeds");
			CHECK(ddesc.width == IMG_W && ddesc.height == IMG_H &&
				ddesc.channels == IMG_CH, "decoded descriptor matches");

			CHECK(qoim_check(enc, enc_len) == QOIM_OK, "check() accepts own stream");
			CHECK(qoim_stats(enc, enc_len, &hc, &tc, &hb, &tb) == QOIM_OK,
				"stats() accepts own stream");
			CHECK(tc == IMG_PX * IMG_CH && tb > 0, "stats totals sane");

			pixel_errors(src, dec, IMG_PX, IMG_CH, &max_abs, &sum_sq);
			rmse = isqrt(sum_sq / (double)(IMG_PX * IMG_CH));

			if (configs[ci].tolerance == 0) {
				CHECK(max_abs == 0, "tolerance 0 is lossless");
				CHECK(memcmp(src, dec, raw_size) == 0,
					"tolerance 0 roundtrip is byte identical");
			}
			CHECK(max_abs <= configs[ci].tolerance,
				"max roundtrip error within tolerance");

			printf(" %4d  %2d | %9d  %5.2f%% | %5lld  %6.3f | %7.3f  %7.3f | %5.1f%%  %5.1f%%\n",
				configs[ci].block_sz, configs[ci].tolerance,
				enc_len, 100.0 * enc_len / (double)raw_size,
				max_abs, rmse,
				enc_ns / 1e6 / time_iters, dec_ns / 1e6 / time_iters,
				100.0 * hc / (double)tc, 100.0 * hb / (double)tb);

			if (ci == 0) {
				/* corruption suite runs once per material on a valid stream */
				test_corruption(enc, enc_len);
			}
			free(dec);
			free(enc);
		}
		printf("\n");
		free(src);
	}

	/* --------------------------------------------------------------------- */
	/* Edge cases: partial blocks, RGB (3ch), block larger than image.        */

	overall_fail_at_start = tests_failed;
	{
		const int w = 10, h = 7, ch = 3;
		unsigned char *src3 = (unsigned char *)malloc((size_t)w * h * ch);
		unsigned char *enc3, *dec3;
		qoi_desc d3;
		qoim_status st;
		int L = 0;
		long long max_abs; double sum_sq;
		for (i = 0; i < w * h * ch; i++) { src3[i] = (unsigned char)(i * 7); }

		enc3 = qoim_encode(src3, &(qoi_desc){w, h, ch, QOI_SRGB},
			&(qoim_options){ 16, 0 }, &L);
		CHECK(enc3 != NULL, "RGB image with block larger than image encodes");
		dec3 = qoim_decode(enc3, L, &d3, 0, &st);
		CHECK(st == QOIM_OK && dec3 != NULL, "RGB partial-block image decodes");
		if (dec3) {
			pixel_errors(src3, dec3, w * h, ch, &max_abs, &sum_sq);
			CHECK(max_abs == 0, "RGB partial-block roundtrip lossless");
			free(dec3);
		}
		/* force RGBA output from a 3-channel stream */
		dec3 = qoim_decode(enc3, L, &d3, 4, &st);
		CHECK(st == QOIM_OK && dec3 != NULL && d3.channels == 3,
			"channel forcing to RGBA works");
		if (dec3) {
			int ok = 1, x, y;
			for (y = 0; y < h && ok; y++) {
				for (x = 0; x < w && ok; x++) {
					int si = (y * w + x) * ch;
					int di = (y * w + x) * 4;
					if (dec3[di] != src3[si] || dec3[di+1] != src3[si+1] ||
						dec3[di+2] != src3[si+2] || dec3[di+3] != 255) {
						ok = 0;
					}
				}
			}
			CHECK(ok, "forced RGBA pixels and filler alpha 255 correct");
			free(dec3);
		}
		free(enc3);
		free(src3);
	}

	/* RGBA stream must refuse an unsafe downconvert to RGB */
	{
		unsigned char src4[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
		int L = 0;
		unsigned char *e4 = qoim_encode(src4,
			&(qoi_desc){2, 1, 4, QOI_SRGB},
			&(qoim_options){ 2, 0 }, &L);
		qoi_desc d4; qoim_status st;
		CHECK(e4 != NULL, "RGBA fixture encodes");
		CHECK(qoim_decode(e4, L, &d4, 3, &st) == NULL && st == QOIM_ERR_ARG,
			"RGBA->RGB downconvert rejected");
		free(e4);
	}

	/* 1x1 image */
	{
		unsigned char one[4] = { 9, 8, 7, 6 };
		int L = 0;
		unsigned char *e = qoim_encode(one, &(qoi_desc){1, 1, 4, QOI_SRGB},
			&(qoim_options){ 1, 0 }, &L);
		unsigned char *d;
		qoi_desc dd; qoim_status st;
		CHECK(e != NULL, "1x1 image encodes");
		d = qoim_decode(e, L, &dd, 0, &st);
		CHECK(st == QOIM_OK && d != NULL && memcmp(d, one, 4) == 0,
			"1x1 roundtrip byte identical");
		free(d); free(e);
	}

	/* tolerance > 0 actually quantizes an approximate channel */
	{
		/* 4x4 block where alpha drifts 100..103: within tol 4 => constant */
		unsigned char *src = (unsigned char *)malloc(16 * 4);
		int L = 0, k;
		for (k = 0; k < 16; k++) {
			src[k*4] = src[k*4+1] = src[k*4+2] = 50;
			src[k*4+3] = (unsigned char)(100 + (k & 3));
		}
		unsigned char *e = qoim_encode(src, &(qoi_desc){4, 4, 4, QOI_SRGB},
			&(qoim_options){ 4, 4 }, &L);
		int hc, tc, hb, tb;
		CHECK(e != NULL && qoim_stats(e, L, &hc, &tc, &hb, &tb) == QOIM_OK,
			"approx-constant block encodes");
		CHECK(hb == 1, "whole block fully masked under tolerance");
		{
			qoi_desc dd; qoim_status st;
			unsigned char *d = qoim_decode(e, L, &dd, 0, &st);
			long long ma; double ss;
			CHECK(st == QOIM_OK && d != NULL, "approx block decodes");
			pixel_errors(src, d, 16, 4, &ma, &ss);
			CHECK(ma <= 4, "approx block error bounded by tolerance");
			free(d);
		}
		free(e); free(src);
	}

	if (tests_failed == overall_fail_at_start) {
		printf("edge-case checks: all passed\n\n");
	}

	printf("--------------------------------------------------\n");
	printf("checks run: %d, failed: %d\n", tests_run, tests_failed);
	if (tests_failed == 0) {
		printf("ALL TESTS PASSED\n");
		return 0;
	}
	printf("TESTS FAILED\n");
	return 1;
}
