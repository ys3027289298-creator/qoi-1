/*
qoi_endian_test.c - Endianness verification harness for the QOI codec.

SPDX-License-Identifier: MIT

Three independent code paths are compared on every input:

  PROD  production qoi.h (explicit endian helpers, byte-channel pixels)
  ALT   qoi_alt.h       (flat byte tables, never forms a 32 bit pixel)
  BE    qoi_be.h        (word-oriented code over EMULATED big endian memory,
                         all multi-byte loads/stores byte-reversed)

Checks per test image:
  1. All three encoders emit byte-identical QOI streams.
  2. All three decoders produce byte-identical pixels from each stream
     (every decoder x every stream, including cross-decoding).
  3. Round trip: decoded pixels equal the original source pixels.
  4. Emulated big endian decode equals the native (host) decode, per pixel.

Plus:
  - targeted edge-case images exercising every chunk type and wraparound,
  - 10,000 deterministic pseudo-random images,
  - malformed/truncated stream handling must agree across paths (no crash).

Build: see tests/Makefile.
*/

#define QOI_IMPLEMENTATION
#include "../qoi.h"

#include "qoi_alt.h"
#include "qoi_be.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic PRNG (xorshift32) so the 10k image corpus is reproducible. */
static uint32_t rng_state;
static uint32_t rng_next(void) {
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}
static uint8_t rng_byte(void) { return (uint8_t)(rng_next() >> 24); }

static long g_failures = 0;
static long g_checks = 0;

#define CHECK(cond, ...) do {                                              \
	g_checks++;                                                           \
	if (!(cond)) {                                                        \
		g_failures++;                                                     \
		printf("FAIL [%s:%d]: ", __FILE__, __LINE__);                     \
		printf(__VA_ARGS__);                                              \
		putchar('\n');                                                    \
	}                                                                     \
} while (0)

/* Run the full three-way comparison on one raw image. Returns 0 on pass. */
static int verify_image(const uint8_t *pixels, uint32_t w, uint32_t h,
                        int channels, const char *label) {
	int fail_before = (int)g_failures;
	qoi_desc desc;
	desc.width = w;
	desc.height = h;
	desc.channels = (unsigned char)channels;
	desc.colorspace = QOI_SRGB;
	int nbytes = (int)((uint64_t)w * h * channels);

	/* ---- encode with all three paths ---- */
	int prod_len = 0, alt_len = 0, be_len = 0;
	uint8_t *prod_stream = (uint8_t *)qoi_encode(pixels, &desc, &prod_len);
	uint8_t *alt_stream  = alt_encode(pixels, w, h, channels, &alt_len);
	uint8_t *be_stream   = alt_be_encode(pixels, w, h, channels, &be_len);

	CHECK(prod_stream && alt_stream && be_stream,
	      "%s: encoder returned NULL (w=%u h=%u c=%d)", label, w, h, channels);
	if (!prod_stream || !alt_stream || !be_stream) {
		free(prod_stream); free(alt_stream); free(be_stream);
		return (int)g_failures - fail_before;
	}

	/* 1. byte-identical streams */
	CHECK(prod_len == alt_len,
	      "%s: prod/alt stream length %d != %d", label, prod_len, alt_len);
	CHECK(prod_len == be_len,
	      "%s: prod/be  stream length %d != %d", label, prod_len, be_len);
	if (prod_len == alt_len) {
		CHECK(memcmp(prod_stream, alt_stream, (size_t)prod_len) == 0,
		      "%s: prod vs alt stream byte mismatch", label);
	}
	if (prod_len == be_len) {
		CHECK(memcmp(prod_stream, be_stream, (size_t)prod_len) == 0,
		      "%s: prod vs emulated-BE stream byte mismatch", label);
	}

	/* ---- decode each stream with each decoder (3x3 cross matrix) ---- */
	struct { const char *name; const uint8_t *s; int len; } streams[] = {
		{"prod", prod_stream, prod_len},
		{"alt",  alt_stream,  alt_len},
		{"be",   be_stream,   be_len},
	};

	for (int si = 0; si < 3; si++) {
		qoi_desc pd;
		uint8_t *prod_px = (uint8_t *)qoi_decode(streams[si].s, streams[si].len,
		                                         &pd, channels);
		uint32_t aw, ah; int ac;
		uint8_t *alt_px = alt_decode(streams[si].s, streams[si].len,
		                             &aw, &ah, &ac, channels);
		uint32_t bw, bh; int bc;
		uint8_t *be_px = alt_be_decode(streams[si].s, streams[si].len,
		                               &bw, &bh, &bc, channels);

		CHECK(prod_px && alt_px && be_px,
		      "%s/%s: a decoder returned NULL", label, streams[si].name);

		if (prod_px && alt_px) {
			/* 2. per-pixel agreement, native vs alt path */
			CHECK(memcmp(prod_px, alt_px, (size_t)nbytes) == 0,
			      "%s/%s: prod vs alt pixel mismatch", label, streams[si].name);
		}
		if (prod_px && be_px) {
			/* 4. emulated big endian vs native, per pixel */
			CHECK(memcmp(prod_px, be_px, (size_t)nbytes) == 0,
			      "%s/%s: emulated-BE vs native pixel mismatch",
			      label, streams[si].name);
			/* 3. round trip against the source pixels */
			CHECK(memcmp(be_px, pixels, (size_t)nbytes) == 0,
			      "%s/%s: emulated-BE roundtrip mismatch",
			      label, streams[si].name);
		}
		if (prod_px) {
			CHECK(pd.width == w && pd.height == h && pd.channels == channels,
			      "%s/%s: header fields wrong", label, streams[si].name);
			CHECK(memcmp(prod_px, pixels, (size_t)nbytes) == 0,
			      "%s/%s: prod roundtrip mismatch", label, streams[si].name);
		}
		free(prod_px); free(alt_px); free(be_px);
	}

	/* channel forcing: decode a 4ch image as 3ch and vice versa sanity */
	free(prod_stream); free(alt_stream); free(be_stream);
	return (int)g_failures - fail_before;
}

/* Build images whose contents are chosen to hit every chunk type. */
static void build_edge_cases(void) {
	/* fixed small dimensions so chunk boundaries are easy to reason about */
	const uint32_t w = 37, h = 29;
	for (int channels = 3; channels <= 4; channels++) {
		int n = (int)(w * h * channels);
		uint8_t *px = (uint8_t *)malloc((size_t)n);

		/* A: solid color -> QOI_OP_RUN, including max run 62 */
		memset(px, 0, (size_t)n);
		for (int i = 3; i < n; i += channels) { px[i] = 255; }
		verify_image(px, w, h, channels, "solid");

		/* B: extreme wraparound diffs (0->255, 255->0) -> RGB/RGBA ops */
		for (int i = 0; i < n; i += channels) {
			px[i] = (i & 1) ? 255 : 0;
			px[i+1] = (i & 1) ? 0 : 255;
			px[i+2] = 128;
			if (channels == 4) px[i+3] = (i & 2) ? 255 : 0;
		}
		verify_image(px, w, h, channels, "wraparound");

		/* C: tiny +/-1..2 diffs -> QOI_OP_DIFF */
		for (int i = 0; i < n; i++) {
			px[i] = (uint8_t)((i * 7) & 0x03);
			if (channels == 4 && (i % 4 == 3)) px[i] = 200;
		}
		verify_image(px, w, h, channels, "small-diff");

		/* D: smooth gradient -> QOI_OP_LUMA */
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				int o = (int)(y * w + x) * channels;
				px[o]   = (uint8_t)(x * 3 + y);
				px[o+1] = (uint8_t)(x * 3 + y + 5);
				px[o+2] = (uint8_t)(x * 3 + y + 11);
				if (channels == 4) px[o+3] = (uint8_t)(128 + (x & 7));
			}
		}
		verify_image(px, w, h, channels, "gradient");

		/* E: repeating pattern that fills the 64-entry color table */
		for (int i = 0; i < n; i += channels) {
			uint8_t v = (uint8_t)((i / channels) & 0x3f);
			px[i] = v; px[i+1] = (uint8_t)(v * 2); px[i+2] = (uint8_t)(v * 3);
			if (channels == 4) px[i+3] = (uint8_t)(255 - v);
		}
		verify_image(px, w, h, channels, "index-table");

		/* F: 1x1 and single-row/column images */
		free(px);
		uint8_t one[4] = {10, 20, 30, 40};
		verify_image(one, 1, 1, 4, "1x1-rgba");
		uint8_t one3[3] = {10, 20, 30};
		verify_image(one3, 1, 1, 3, "1x1-rgb");

		px = (uint8_t *)malloc((size_t)(w * channels));
		for (uint32_t x = 0; x < w; x++) {
			px[x*channels]   = (uint8_t)(x);
			px[x*channels+1] = (uint8_t)(255 - x);
			px[x*channels+2] = (uint8_t)(x * 13);
			if (channels == 4) px[x*channels+3] = (uint8_t)(x * 17);
		}
		verify_image(px, w, 1, channels, "single-row");
		free(px);

		px = (uint8_t *)malloc((size_t)(h * channels));
		for (uint32_t y = 0; y < h; y++) {
			px[y*channels]   = (uint8_t)(y * 9);
			px[y*channels+1] = (uint8_t)(y * 5);
			px[y*channels+2] = (uint8_t)(y * 3);
			if (channels == 4) px[y*channels+3] = (uint8_t)(y);
		}
		verify_image(px, 1, h, channels, "single-col");
		free(px);
	}
}

/* Random image with mixed structure: runs, gradients and salt-pepper noise,
so all chunk types appear in one stream. */
static void build_random_image(uint32_t w, uint32_t h, int channels, int mode) {
	int n = (int)((uint64_t)w * h * channels);
	uint8_t *px = (uint8_t *)malloc((size_t)n);

	uint8_t r = rng_byte(), g = rng_byte(), b = rng_byte(), a = 255;
	for (int i = 0; i < n; i += channels) {
		switch (mode) {
			case 0: { /* pure noise */
				r = rng_byte(); g = rng_byte(); b = rng_byte();
				if (channels == 4) a = rng_byte();
				break;
			}
			case 1: { /* noisy runs: repeat a color a random number of times */
				static int repeat = 0;
				if (repeat-- <= 0) {
					r = rng_byte(); g = rng_byte(); b = rng_byte();
					if (channels == 4) a = rng_byte();
					repeat = (int)(rng_next() % 70);
				}
				break;
			}
			default: { /* bounded random walk -> DIFF/LUMA heavy */
				r = (uint8_t)(r + (int)(rng_next() % 7) - 3);
				g = (uint8_t)(g + (int)(rng_next() % 7) - 3);
				b = (uint8_t)(b + (int)(rng_next() % 7) - 3);
				if (channels == 4 && (rng_next() & 7) == 0) {
					a = (uint8_t)(a + (int)(rng_next() % 5) - 2);
				}
			}
		}
		px[i] = r; px[i+1] = g; px[i+2] = b;
		if (channels == 4) px[i+3] = a;
	}

	char label[64];
	snprintf(label, sizeof(label), "random mode=%d %ux%u c=%d",
	         mode, w, h, channels);
	verify_image(px, w, h, channels, label);
	free(px);
}

/* Malformed inputs: all decoders must fail safely (NULL) or behave the same,
never crash. Run under ASan/UBSan in the Makefile. */
static void build_malformed(void) {
	uint8_t good_hdr[29] = {
		'q','o','i','f', 0,0,0,2, 0,0,0,2, 4, 0,
		0xff, 1,2,3,4,    /* RGBA pixel */
		0xc0, 0,          /* run 1 */
		0,0,0,0,0,0,0,1
	};
	qoi_desc d;

	/* truncated header: all three must reject with NULL */
	uint32_t zw = 0, zh = 0; int zc = 0;
	CHECK(qoi_decode(good_hdr, 13, &d, 4) == NULL, "truncated header");
	CHECK(alt_decode(good_hdr, 13, &zw, &zh, &zc, 4) == NULL,
	      "alt truncated header");
	CHECK(alt_be_decode(good_hdr, 13, &zw, &zh, &zc, 4) == NULL,
	      "be truncated header");

	/* bad magic */
	uint8_t bad[29];
	memcpy(bad, good_hdr, sizeof(bad));
	bad[0] = 'x';
	CHECK(qoi_decode(bad, (int)sizeof(bad), &d, 4) == NULL, "bad magic prod");
	uint32_t tw, th; int tc;
	CHECK(alt_decode(bad, (int)sizeof(bad), &tw, &th, &tc, 4) == NULL,
	      "bad magic alt");
	CHECK(alt_be_decode(bad, (int)sizeof(bad), &tw, &th, &tc, 4) == NULL,
	      "bad magic be");

	/* Chunks cut off before all pixels are produced. The buffer stays a
	valid allocation (only the logical length is shortened, past the
	22-byte minimum), so each decoder exhausts its chunk region and fills
	the remaining pixels with the last color. The three must agree. */
	uint8_t src4[64];
	for (int i = 0; i < 64; i++) { src4[i] = (uint8_t)(i * 11 + 3); }
	qoi_desc sd = {4, 4, 4, 0};
	int slen = 0;
	uint8_t *sfull = (uint8_t *)qoi_encode(src4, &sd, &slen);
	CHECK(sfull != NULL && slen > 22, "truncation fixture setup");
	int cut = 22; /* header(14) + a few chunk bytes + end marker region */
	uint8_t *p1 = (uint8_t *)qoi_decode(sfull, cut, &d, 4);
	uint8_t *p2 = alt_decode(sfull, cut, &tw, &th, &tc, 4);
	uint8_t *p3 = alt_be_decode(sfull, cut, &tw, &th, &tc, 4);
	CHECK((p1 != NULL) == (p2 != NULL) && (p1 != NULL) == (p3 != NULL),
	      "truncated chunk: NULL-result disagreement");
	if (p1 && p2 && p3) {
		CHECK(memcmp(p1, p2, 64) == 0 && memcmp(p1, p3, 64) == 0,
		      "truncated chunk: filler pixels disagree");
	}
	free(p1); free(p2); free(p3); free(sfull);

	/* NULL/garbage parameter handling */
	int dummy_len = 0;
	qoi_desc dummy_desc = {1, 1, 4, 0};
	CHECK(qoi_decode(NULL, 100, &d, 4) == NULL, "null data prod");
	CHECK(alt_decode(NULL, 100, &tw, &th, &tc, 4) == NULL, "null data alt");
	CHECK(alt_be_decode(NULL, 100, &tw, &th, &tc, 4) == NULL, "null data be");
	CHECK(qoi_encode(NULL, &dummy_desc, &dummy_len) == NULL,
	      "null encode prod");
	CHECK(alt_encode(NULL, 1, 1, 4, &dummy_len) == NULL, "null encode alt");
	CHECK(alt_be_encode(NULL, 1, 1, 4, &dummy_len) == NULL, "null encode be");
}

int main(void) {
	rng_state = 0x12345678u;

	printf("== edge-case images ==\n");
	build_edge_cases();

	printf("== malformed inputs ==\n");
	build_malformed();

	printf("== 10,000 random images (three code paths, 3x3 decode matrix) ==\n");
	const int N = 10000;
	for (int i = 0; i < N; i++) {
		uint32_t w = 1 + (rng_next() % 48);
		uint32_t h = 1 + (rng_next() % 48);
		int channels = (rng_next() & 1) ? 3 : 4;
		int mode = (int)(rng_next() % 3);
		build_random_image(w, h, channels, mode);
		if ((i + 1) % 1000 == 0) {
			printf("  %5d images, %ld checks, %ld failures\n",
			       i + 1, g_checks, g_failures);
		}
	}

	printf("\n=== TOTAL: %ld checks, %ld FAILURES ===\n",
	       g_checks, g_failures);
	return g_failures ? 1 : 0;
}
