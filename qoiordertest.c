/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT


Tests for the channel order parameter of qoi_encode_order()/qoi_decode_order()

Checks that
 - BGR / BGRA input produces a stream byte-identical to the r,g,b[,a] input
   (i.e. the stream does not depend on the channel order),
 - decoding with either order yields exactly the canonical pixels, for 3 and 4
   channels and for forced output channel counts,
 - an unknown order value is rejected by both functions.

Compile and run with:
	gcc qoiordertest.c -std=c99 -O2 -o qoiordertest && ./qoiordertest

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

static int failures = 0;

#define CHECK(COND, ...) do { \
	if (!(COND)) { \
		printf("FAIL line %d: ", __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while (0)

/* Build a BGR(A) copy of an RGB(A) buffer. */
static void rgb_to_bgr(const unsigned char *src, unsigned char *dst,
	int pixels, int channels)
{
	int i;
	for (i = 0; i < pixels; i++) {
		dst[i * channels + 0] = src[i * channels + 2];
		dst[i * channels + 1] = src[i * channels + 1];
		dst[i * channels + 2] = src[i * channels + 0];
		if (channels == 4) {
			dst[i * channels + 3] = src[i * channels + 3];
		}
	}
}

/* Deterministic pseudo random test image with flat runs (QOI_OP_RUN), small
   gradients (QOI_OP_DIFF/LUMA) and noise (QOI_OP_RGB/RGBA). */
static void make_image(unsigned char *rgb, int w, int h, int channels) {
	unsigned int seed = 12345;
	int x, y, c;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned char *p = rgb + ((size_t)y * w + x) * channels;
			if (x < w / 3) {
				p[0] = 10; p[1] = 200; p[2] = 30;
			}
			else if (x < 2 * w / 3) {
				p[0] = (unsigned char)(100 + x / 2);
				p[1] = (unsigned char)(x * 3);
				p[2] = (unsigned char)(255 - x);
			}
			else {
				for (c = 0; c < 3; c++) {
					seed = seed * 1664525u + 1013904223u;
					p[c] = (unsigned char)(seed >> 24);
				}
			}
			if (channels == 4) {
				seed = seed * 1664525u + 1013904223u;
				p[3] = (x < 2 * w / 3) ? 255 : (unsigned char)(seed >> 24);
			}
		}
	}
}

static void test_channels(int channels) {
	const int w = 137, h = 91, pixels = w * h;
	const int size = pixels * channels;
	unsigned char *rgb = malloc(size);
	unsigned char *bgr = malloc(size);
	qoi_desc desc = {w, h, channels, QOI_SRGB};
	int len_rgb = 0, len_bgr = 0, expected_len = 0;
	void *enc_rgb, *enc_bgr, *enc_plain;
	void *dec_rgb, *dec_bgr;
	qoi_desc d1, d2;

	make_image(rgb, w, h, channels);
	rgb_to_bgr(rgb, bgr, pixels, channels);

	enc_rgb = qoi_encode_order(rgb, &desc, QOI_ORDER_RGB, &len_rgb);
	enc_bgr = qoi_encode_order(bgr, &desc, QOI_ORDER_BGR, &len_bgr);
	enc_plain = qoi_encode(rgb, &desc, &expected_len);

	CHECK(enc_rgb && enc_bgr && enc_plain,
		"encode failed for %d channels", channels);
	if (!enc_rgb || !enc_bgr || !enc_plain) {
		free(rgb); free(bgr);
		return;
	}

	CHECK(len_bgr == len_rgb,
		"BGR stream length %d != RGB stream length %d", len_bgr, len_rgb);
	CHECK(memcmp(enc_rgb, enc_bgr, len_rgb) == 0,
		"BGR stream differs from RGB stream for %d channels", channels);
	CHECK(len_rgb == expected_len && memcmp(enc_rgb, enc_plain, len_rgb) == 0,
		"qoi_encode_order(RGB) differs from qoi_encode()");

	dec_rgb = qoi_decode_order(enc_rgb, len_rgb, &d1, channels, QOI_ORDER_RGB);
	dec_bgr = qoi_decode_order(enc_rgb, len_rgb, &d2, channels, QOI_ORDER_BGR);
	CHECK(dec_rgb && dec_bgr, "decode failed for %d channels", channels);
	if (dec_rgb && dec_bgr) {
		CHECK(memcmp(dec_rgb, rgb, size) == 0,
			"RGB decode does not reproduce the source, %d channels", channels);
		CHECK(memcmp(dec_bgr, bgr, size) == 0,
			"BGR decode does not reproduce the BGR source, %d channels", channels);
		CHECK(d1.width == (unsigned)w && d1.height == (unsigned)h &&
			d1.channels == channels, "desc not filled for %d channels", channels);
	}
	free(dec_rgb);
	free(dec_bgr);

	/* Forcing the output channel count must also honor the order. */
	{
		int out_channels = (channels == 3) ? 4 : 3;
		unsigned char *dec = qoi_decode_order(enc_rgb, len_rgb, &d1,
			out_channels, QOI_ORDER_BGR);
		CHECK(dec != NULL, "forced %d channel BGR decode failed", out_channels);
		if (dec) {
			int i;
			for (i = 0; i < pixels; i++) {
				unsigned char *p = dec + i * out_channels;
				CHECK(p[2] == rgb[i * channels + 0] &&
					p[1] == rgb[i * channels + 1] &&
					p[0] == rgb[i * channels + 2],
					"forced %d channel BGR pixel %d has wrong color",
					out_channels, i);
				if (out_channels == 4 && channels == 4) {
					CHECK(p[3] == rgb[i * channels + 3],
						"forced output lost alpha at pixel %d", i);
				}
			}
			free(dec);
		}
	}

	/* channels == 0 picks the channel count from the file header. */
	{
		unsigned char *dec = qoi_decode_order(enc_rgb, len_rgb, &d1, 0,
			QOI_ORDER_BGR);
		CHECK(dec && memcmp(dec, bgr, size) == 0,
			"BGR decode with channels = 0 wrong for %d channels", channels);
		free(dec);
	}

	/* Unknown order values must be rejected on both sides. */
	{
		int n = 0;
		CHECK(qoi_encode_order(rgb, &desc, 2, &n) == NULL,
			"encode accepted order 2");
		CHECK(qoi_encode_order(rgb, &desc, -1, &n) == NULL,
			"encode accepted order -1");
		CHECK(qoi_decode_order(enc_rgb, len_rgb, &d1, channels, 2) == NULL,
			"decode accepted order 2");
		CHECK(qoi_decode_order(enc_rgb, len_rgb, &d1, channels, -1) == NULL,
			"decode accepted order -1");
	}

	free(enc_rgb);
	free(enc_bgr);
	free(enc_plain);
	free(rgb);
	free(bgr);
}

int main(void) {
	/* Single pixel edge case: no run, no previous pixel. */
	{
		unsigned char rgb[4] = {1, 2, 3, 4};
		unsigned char bgr[4] = {3, 2, 1, 4};
		qoi_desc desc = {1, 1, 4, QOI_SRGB};
		int a = 0, b = 0;
		void *ea = qoi_encode_order(rgb, &desc, QOI_ORDER_RGB, &a);
		void *eb = qoi_encode_order(bgr, &desc, QOI_ORDER_BGR, &b);
		CHECK(ea && eb && a == b && memcmp(ea, eb, a) == 0,
			"1x1 BGR stream differs from RGB stream");
		free(ea);
		free(eb);
	}

	test_channels(3);
	test_channels(4);

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all channel order tests passed\n");
	return 0;
}
