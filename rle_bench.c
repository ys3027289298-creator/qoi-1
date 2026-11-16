/*

Sample, verification and benchmark for the run-length preprocessing layer
(qoi_encode_rle / qoi_decode_rle) in qoi.h.

The two sample images are generated in memory, so this program needs no
image files and no third-party dependencies. It reports, for pixel-art and
photo-like material, the compressed size ratio, the roundtrip consistency,
the encode/decode time and the run coverage of the preprocessor, and it
checks that invalid thresholds are rejected and that malformed streams do
not decode.

Compile and run with:
	gcc -std=c99 -O2 rle_bench.c -o rle_bench && ./rle_bench

*/

#define _POSIX_C_SOURCE 199309L

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
	if (cond) { printf("  PASS: "); } \
	else { printf("  FAIL: "); failures++; } \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

/* xorshift32; deterministic so every run measures the same pixels. */
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

/* --------------------------------------------------------------------------
Sample material */

/* Pixel art: large solid color areas with hard palette edges, so that the
preprocessor has long runs to collapse. RGBA. */
static void gen_pixelart(unsigned char *img, int w, int h) {
	static const unsigned char pal[10][4] = {
		{ 96, 160, 224, 255}, /* sky */
		{ 56, 140,  64, 255}, /* ground */
		{ 40,  88,  48, 255}, /* grass shadow */
		{220,  48,  48, 255}, /* red */
		{248, 216,  88, 255}, /* yellow */
		{ 64,  96, 224, 255}, /* blue */
		{144,  72, 184, 255}, /* purple */
		{ 24,  24,  40, 255}, /* outline */
		{255, 255, 255, 255}, /* white */
		{  0,   0,   0,   0}  /* transparent */
	};
	int x, y, i;

	/* Sky fills the whole image, ground covers the bottom third and a
	grass band sits right above it: every one of those rows is a full
	width solid run. */
	for (i = 0; i < w * h; i++) {
		memcpy(img + i * 4, pal[0], 4);
	}
	for (y = h * 2 / 3; y < h; y++) {
		for (x = 0; x < w; x++) {
			memcpy(img + (y * w + x) * 4, pal[1], 4);
		}
	}
	for (y = h * 2 / 3; y < h * 2 / 3 + 6; y++) {
		for (x = 0; x < w; x++) {
			memcpy(img + (y * w + x) * 4, pal[2], 4);
		}
	}

	/* A few large solid blocks with a one pixel outline. */
	{
		static const int blocks[4][4] = {
			{ 20, 14, 60, 30},
			{100, 10, 40, 24},
			{ 12, 52, 34, 26},
			{118, 48, 30, 30}
		};
		int b;
		for (b = 0; b < 4; b++) {
			int bx = blocks[b][0], by = blocks[b][1];
			int bw = blocks[b][2], bh = blocks[b][3];
			const unsigned char *c = pal[3 + b % 5];

			for (y = by; y < by + bh; y++) {
				for (x = bx; x < bx + bw; x++) {
					int edge = x == bx || y == by ||
						x == bx + bw - 1 ||
						y == by + bh - 1;
					memcpy(img + (y * w + x) * 4,
						edge ? pal[7] : c, 4);
				}
			}
		}
	}

	/* A transparent band and isolated fully transparent pixels: a long
	zero run plus escapes in the middle of solid areas. */
	for (y = 6; y < 12; y++) {
		for (x = 40; x < w - 24; x++) {
			memcpy(img + (y * w + x) * 4, pal[9], 4);
		}
	}
	for (i = 0; i < 12; i++) {
		int px = (int)(rng() % (unsigned int)(w * h));
		memcpy(img + px * 4, pal[9], 4);
	}
}

/* Photo-like material: smooth two dimensional gradients plus a little
per-pixel noise. RGB (3 channels). */
static void gen_photo(unsigned char *img, int w, int h) {
	int x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned char *p = img + (y * w + x) * 3;
			int noise = (int)(rng() % 9u) - 4;
			p[0] = (unsigned char)clamp8((x * 255) / (w - 1) + noise);
			p[1] = (unsigned char)clamp8((y * 255) / (h - 1) + noise / 2);
			p[2] = (unsigned char)clamp8(
				((x + y) * 255) / (w + h - 2) + noise);
		}
	}
}

/* --------------------------------------------------------------------------
Run analysis: same maximal-run scan the preprocessor does. */

typedef struct {
	long run_pixels;   /* pixels covered by runs longer than threshold */
	long total_pixels;
	long runs;         /* number of preprocessed runs */
	long longest_run;
} run_stats_t;

static run_stats_t analyze_runs(
	const unsigned char *px, int w, int h, int ch, int threshold
) {
	run_stats_t st;
	int px_count = w * h;
	int pos = 0;

	memset(&st, 0, sizeof(st));
	st.total_pixels = px_count;

	while (pos < px_count) {
		int base = pos * ch;
		int len = 1;

		while (pos + len < px_count &&
			memcmp(px + base, px + (pos + len) * ch, (size_t)ch) == 0) {
			len++;
		}

		if (len > threshold) {
			st.run_pixels += len;
			st.runs++;
			if (len > st.longest_run) {
				st.longest_run = len;
			}
		}
		pos += len;
	}

	return st;
}

/* --------------------------------------------------------------------------
Roundtrip and metrics for one sample */

static void print_material(
	const char *name, const unsigned char *px, int w, int h, int ch,
	int threshold, int reps
) {
	qoi_desc desc;
	qoi_desc out_desc;
	int px_len = w * h * ch;
	int len_plain = 0, len_rle = 0;
	void *enc_plain, *enc_rle, *dec;
	double t0, t1;
	int i, r, mismatches;
	unsigned char *dec_px;

	desc = (qoi_desc){
		.width = (unsigned int)w, .height = (unsigned int)h,
		.channels = (unsigned char)ch, .colorspace = QOI_SRGB
	};

	enc_plain = qoi_encode(px, &desc, &len_plain);
	enc_rle = qoi_encode_rle(px, &desc, threshold, &len_rle);
	CHECK(enc_plain != NULL && enc_rle != NULL,
		"%s: plain and run-length encodes succeed", name);
	if (!enc_plain || !enc_rle) {
		free(enc_plain);
		free(enc_rle);
		return;
	}

	/* Roundtrip: exact for all three output channel modes. */
	mismatches = 0;
	for (r = 0; r < 3; r++) {
		int out_ch = r == 0 ? 0 : (r == 1 ? 3 : 4);
		int expect_ch = out_ch == 0 ? ch : out_ch;
		dec = qoi_decode_rle(enc_rle, len_rle, &out_desc, out_ch);

		if (!dec) {
			mismatches += px_len;
			continue;
		}
		dec_px = (unsigned char *)dec;
		if (out_desc.width != (unsigned int)w ||
			out_desc.height != (unsigned int)h ||
			out_desc.channels != (unsigned char)ch ||
			out_desc.colorspace != QOI_SRGB) {
			mismatches += px_len;
		}
		for (i = 0; i < w * h; i++) {
			int c;
			for (c = 0; c < expect_ch; c++) {
				/* Source channels beyond the input's count are the
				alpha byte qoi fills in when expanding RGB to RGBA. */
				unsigned char want = c < ch ? px[i * ch + c] : 255;
				/* Forcing 3 channels drops the alpha byte. */
				if (ch == 4 && expect_ch == 3 && c == 3) {
					continue;
				}
				if (dec_px[i * expect_ch + c] != want) {
					mismatches++;
				}
			}
		}
		free(dec);
	}
	CHECK(mismatches == 0, "%s: roundtrip is pixel exact (%d mismatches)",
		name, mismatches);

	/* Timings: warm up, then average over `reps` runs. */
	for (i = 0; i < 3; i++) {
		free(qoi_encode(px, &desc, &len_plain));
		free(qoi_encode_rle(px, &desc, threshold, &len_rle));
	}
	t0 = now_ms();
	for (i = 0; i < reps; i++) {
		free(qoi_encode(px, &desc, &len_plain));
	}
	t1 = now_ms();
	{
		double enc_plain_ms = (t1 - t0) / reps;
		double enc_rle_ms, dec_plain_ms, dec_rle_ms;

		t0 = now_ms();
		for (i = 0; i < reps; i++) {
			free(qoi_encode_rle(px, &desc, threshold, &len_rle));
		}
		t1 = now_ms();
		enc_rle_ms = (t1 - t0) / reps;

		t0 = now_ms();
		for (i = 0; i < reps; i++) {
			dec = qoi_decode(enc_plain, len_plain, &out_desc, 0);
			free(dec);
		}
		t1 = now_ms();
		dec_plain_ms = (t1 - t0) / reps;

		t0 = now_ms();
		for (i = 0; i < reps; i++) {
			dec = qoi_decode_rle(enc_rle, len_rle, &out_desc, 0);
			free(dec);
		}
		t1 = now_ms();
		dec_rle_ms = (t1 - t0) / reps;

		{
			run_stats_t runs = analyze_runs(px, w, h, ch, threshold);
			double raw = (double)(w * h * ch);
			double coverage = 100.0 * (double)runs.run_pixels /
				(double)runs.total_pixels;

			printf("\n-- %s: %dx%d, %d channels, %d raw bytes, "
				"threshold %d --\n", name, w, h, ch, w * h * ch,
				threshold);
			printf("   plain qoi        : %7d bytes  ratio %6.4f  "
				"enc %7.3f ms  dec %7.3f ms\n",
				len_plain, (double)len_plain / raw,
				enc_plain_ms, dec_plain_ms);
			printf("   qoi + runs       : %7d bytes  ratio %6.4f  "
				"enc %7.3f ms  dec %7.3f ms\n",
				len_rle, (double)len_rle / raw,
				enc_rle_ms, dec_rle_ms);
			printf("   size vs plain    : %+6.2f%%\n",
				100.0 * (len_rle - len_plain) / len_plain);
			printf("   run coverage     : %5.2f%% of pixels "
				"(%ld runs, longest %ld px)\n",
				coverage, runs.runs, runs.longest_run);
		}
	}

	free(enc_plain);
	free(enc_rle);
}

/* --------------------------------------------------------------------------
Boundary and error cases */

/* Build a "qoir" stream around a caller supplied token pixel array. */
static unsigned char *make_stream(
	const unsigned char *tokens, int n_tokens, int w, int h,
	int channels, int threshold, int *out_size
) {
	qoi_desc td = {(unsigned int)n_tokens, 1, 4, QOI_SRGB};
	int inner_len = 0;
	void *inner = qoi_encode(tokens, &td, &inner_len);
	unsigned char *stream;
	int p = 0;

	if (!inner) {
		return NULL;
	}
	stream = (unsigned char *)malloc(
		(size_t)QOI_RLE_HEADER_SIZE + (size_t)inner_len);
	if (!stream) {
		free(inner);
		return NULL;
	}

	stream[p++] = 'q'; stream[p++] = 'o';
	stream[p++] = 'i'; stream[p++] = 'r';
	stream[p++] = (unsigned char)((w >> 24) & 0xff);
	stream[p++] = (unsigned char)((w >> 16) & 0xff);
	stream[p++] = (unsigned char)((w >>  8) & 0xff);
	stream[p++] = (unsigned char)( w        & 0xff);
	stream[p++] = (unsigned char)((h >> 24) & 0xff);
	stream[p++] = (unsigned char)((h >> 16) & 0xff);
	stream[p++] = (unsigned char)((h >>  8) & 0xff);
	stream[p++] = (unsigned char)( h        & 0xff);
	stream[p++] = (unsigned char)channels;
	stream[p++] = QOI_SRGB;
	stream[p++] = 0;
	stream[p++] = 0;
	stream[p++] = (unsigned char)(((unsigned int)threshold >> 24) & 0xff);
	stream[p++] = (unsigned char)(((unsigned int)threshold >> 16) & 0xff);
	stream[p++] = (unsigned char)(((unsigned int)threshold >>  8) & 0xff);
	stream[p++] = (unsigned char)( (unsigned int)threshold        & 0xff);
	memcpy(stream + p, inner, (size_t)inner_len);
	free(inner);

	*out_size = p + inner_len;
	return stream;
}

static void test_invalid_thresholds(void) {
	static const unsigned char px[12] = {
		1, 2, 3,  4, 5, 6,  7, 8, 9,  10, 11, 12
	};
	qoi_desc desc = {2, 2, 3, QOI_SRGB};
	void *enc;
	int len;

	printf("\n-- invalid thresholds --\n");
	CHECK(qoi_encode_rle(px, &desc,  0, &len) == NULL, "threshold 0 rejected");
	CHECK(qoi_encode_rle(px, &desc, -1, &len) == NULL, "threshold -1 rejected");
	CHECK(qoi_encode_rle(px, &desc, INT_MIN, &len) == NULL,
		"threshold INT_MIN rejected");
	CHECK(qoi_encode_rle(px, &desc, QOI_RLE_MAX_THRESHOLD + 1, &len) == NULL,
		"threshold max+1 rejected");
	CHECK(qoi_encode_rle(px, &desc, INT_MAX, &len) == NULL,
		"threshold INT_MAX rejected");

	enc = qoi_encode_rle(px, &desc, 1, &len);
	CHECK(enc != NULL, "threshold 1 accepted");
	free(enc);
}

static void test_threshold_boundary(void) {
	static const unsigned char px[12] = {
		1, 2, 3,  4, 5, 6,  7, 8, 9,  10, 11, 12
	};
	qoi_desc desc = {2, 2, 3, QOI_SRGB};
	void *enc;
	int len;

	printf("\n-- valid threshold boundary --\n");
	enc = qoi_encode_rle(px, &desc, QOI_RLE_MAX_THRESHOLD, &len);
	CHECK(enc != NULL, "threshold QOI_RLE_MAX_THRESHOLD accepted");
	free(enc);
}

static void test_escape_cases(void) {
	/* 8 RGBA pixels mixing alpha-0 literals, the all-zero pixel, and a
	long transparent run, so escapes, doubled markers and runs all occur. */
	static const unsigned char px[8 * 4] = {
		1, 2, 3, 255,
		0, 0, 0, 0,      /* all-zero pixel: doubled marker */
		9, 8, 7, 0,      /* alpha 0, non-zero rgb: escaped literal */
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,      /* run of three zero pixels (len 4 total) */
		5, 6, 7, 255,
		0, 0, 0, 0
	};
	qoi_desc desc = {8, 1, 4, QOI_SRGB};
	qoi_desc out;
	int len = 0;
	void *enc, *dec;

	printf("\n-- alpha 0 escapes and runs --\n");
	enc = qoi_encode_rle(px, &desc, 3, &len);
	CHECK(enc != NULL, "encode with escaped literals");
	dec = enc ? qoi_decode_rle(enc, len, &out, 0) : NULL;
	CHECK(dec && memcmp(dec, px, sizeof(px)) == 0,
		"escaped roundtrip is pixel exact");
	free(dec);
	free(enc);
}

/* Decode has to fail. Frees a buffer on an unexpected success so the
sanitizer does not report it as a leak. */
#define EXPECT_DECODE_FAIL(cond, msg) do { \
	void *r_ = (cond); \
	if (r_ != NULL) { free(r_); } \
	CHECK(r_ == NULL, "%s", msg); \
} while (0)

static void test_malformed_streams(void) {
	unsigned char *stream;
	unsigned char *copy;
	int size = 0;
	qoi_desc out;

	printf("\n-- malformed streams --\n");

	/* Valid reference stream for a 4x1 RGBA image. */
	{
		unsigned char px[16] = {
			10, 20, 30, 255,  10, 20, 30, 255,
			40, 50, 60, 255,  70, 80, 90, 255
		};
		qoi_desc desc = {4, 1, 4, QOI_SRGB};
		stream = qoi_encode_rle(px, &desc, 1, &size);
		CHECK(stream != NULL, "reference stream built");
		if (!stream) {
			return;
		}
	}

	copy = (unsigned char *)malloc((size_t)size);
	memcpy(copy, stream, (size_t)size);
	copy[0] = 'x';
	EXPECT_DECODE_FAIL(qoi_decode_rle(copy, size, &out, 0),
		"bad magic rejected");

	memcpy(copy, stream, (size_t)size);
	EXPECT_DECODE_FAIL(qoi_decode_rle(copy, QOI_RLE_HEADER_SIZE - 1,
		&out, 0), "truncated header rejected");

	/* Reserved field is at offsets 14..15, right before the threshold. */
	memcpy(copy, stream, (size_t)size);
	copy[QOI_RLE_HEADER_SIZE - 6] = 1;
	EXPECT_DECODE_FAIL(qoi_decode_rle(copy, size, &out, 0),
		"non-zero reserved byte rejected");

	memcpy(copy, stream, (size_t)size);
	copy[QOI_RLE_HEADER_SIZE - 4] = 0;
	copy[QOI_RLE_HEADER_SIZE - 3] = 0;
	copy[QOI_RLE_HEADER_SIZE - 2] = 0;
	copy[QOI_RLE_HEADER_SIZE - 1] = 0;
	EXPECT_DECODE_FAIL(qoi_decode_rle(copy, size, &out, 0),
		"zero threshold in header rejected");

	memcpy(copy, stream, (size_t)size);
	copy[QOI_RLE_HEADER_SIZE + 3] = 'x';
	EXPECT_DECODE_FAIL(qoi_decode_rle(copy, size, &out, 0),
		"bad inner qoi magic rejected");

	/* A run token without its value pixel. */
	{
		unsigned char tok[8] = {0, 0, 2, 0,   9, 9, 9, 255};
		int s = 0;
		unsigned char *bad = make_stream(tok, 1, 2, 1, 3, 1, &s);
		EXPECT_DECODE_FAIL(bad ? qoi_decode_rle(bad, s, &out, 0) : NULL,
			"truncated run token rejected");
		free(bad);
	}

	/* A run longer than the remaining pixels. */
	{
		unsigned char tok[8] = {0, 0, 9, 0,   9, 9, 9, 255};
		int s = 0;
		unsigned char *bad = make_stream(tok, 2, 2, 1, 3, 1, &s);
		EXPECT_DECODE_FAIL(bad ? qoi_decode_rle(bad, s, &out, 0) : NULL,
			"run longer than image rejected");
		free(bad);
	}

	/* Too few tokens for the declared image. */
	{
		unsigned char tok[4] = {9, 9, 9, 255};
		int s = 0;
		unsigned char *bad = make_stream(tok, 1, 2, 1, 3, 1, &s);
		EXPECT_DECODE_FAIL(bad ? qoi_decode_rle(bad, s, &out, 0) : NULL,
			"short token stream rejected");
		free(bad);
	}

	/* More tokens than the declared image. */
	{
		unsigned char tok[8] = {9, 9, 9, 255,  8, 8, 8, 255};
		int s = 0;
		unsigned char *bad = make_stream(tok, 2, 1, 1, 3, 1, &s);
		EXPECT_DECODE_FAIL(bad ? qoi_decode_rle(bad, s, &out, 0) : NULL,
			"trailing tokens rejected");
		free(bad);
	}

	/* Lone escape marker at the end of the token stream. */
	{
		unsigned char tok[4] = {0, 0, 0, 0};
		int s = 0;
		unsigned char *bad = make_stream(tok, 1, 1, 1, 3, 1, &s);
		EXPECT_DECODE_FAIL(bad ? qoi_decode_rle(bad, s, &out, 0) : NULL,
			"lone escape marker rejected");
		free(bad);
	}

	free(copy);
	free(stream);
}

static void test_long_run_split(void) {
	const int w = 4096, h = 4097; /* 16,781,312 px > QOI_RLE_MAX_RUN */
	const int ch = 3;
	int px_len = w * h * ch;
	unsigned char *px = (unsigned char *)malloc((size_t)px_len);
	unsigned char *enc;
	void *dec;
	qoi_desc desc = {(unsigned int)w, (unsigned int)h, ch, QOI_SRGB};
	qoi_desc out;
	int len = 0;

	printf("\n-- run longer than the 24 bit token count --\n");
	CHECK(px != NULL, "allocate %d x %d RGB sample", w, h);
	if (!px) {
		return;
	}
	memset(px, 0x5a, (size_t)px_len);

	enc = qoi_encode_rle(px, &desc, 1, &len);
	CHECK(enc != NULL, "encode run of %d pixels", w * h);
	if (!enc) {
		free(px);
		return;
	}
	printf("   encoded size: %d bytes for %d raw bytes\n", len, px_len);

	dec = qoi_decode_rle(enc, len, &out, 0);
	CHECK(dec != NULL, "decode split-run stream");
	CHECK(dec && memcmp(dec, px, (size_t)px_len) == 0,
		"split-run roundtrip is pixel exact");

	free(dec);
	free(enc);
	free(px);
}

int main(void) {
	const int w = 320, h = 240;
	const int reps = 200;
	unsigned char *art = (unsigned char *)malloc((size_t)(w * h * 4));
	unsigned char *photo = (unsigned char *)malloc((size_t)(w * h * 3));

	printf("== qoi run-length preprocessing layer ==\n");
	rng_state = 0x9e3779b9u;
	gen_pixelart(art, w, h);
	gen_photo(photo, w, h);

	print_material("pixel art", art, w, h, 4, 32, reps);
	print_material("photo", photo, w, h, 3, 32, reps);

	printf("\n-- size ratio by threshold (pixel art / photo) --\n");
	printf("   threshold  pixel-art ratio  photo ratio\n");
	{
		int thresholds[6] = {1, 8, 32, 62, 128, 256};
		int t;
		for (t = 0; t < 6; t++) {
			qoi_desc d4 = {(unsigned int)w, (unsigned int)h, 4, QOI_SRGB};
			qoi_desc d3 = {(unsigned int)w, (unsigned int)h, 3, QOI_SRGB};
			int la = 0, lp = 0;
			void *ea = qoi_encode_rle(art, &d4, thresholds[t], &la);
			void *ep = qoi_encode_rle(photo, &d3, thresholds[t], &lp);
			if (ea && ep) {
				printf("   %9d  %15.4f  %11.4f\n", thresholds[t],
					(double)la / (w * h * 4),
					(double)lp / (w * h * 3));
			}
			else {
				printf("   %9d  encode failed\n", thresholds[t]);
				failures++;
			}
			free(ea);
			free(ep);
		}
	}

	test_invalid_thresholds();
	test_threshold_boundary();
	test_escape_cases();
	test_malformed_streams();
	test_long_run_split();

	printf("\n%s (%d failures)\n", failures == 0 ? "ALL CHECKS PASSED" :
		"FAILURES", failures);

	free(art);
	free(photo);
	return failures == 0 ? 0 : 1;
}
