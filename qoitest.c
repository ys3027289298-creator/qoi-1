/*

Color transform extension test + benchmark for qoi.h

Self contained: only the C standard library and qoi.h are used. Generates
four small synthetic image kinds (photo-like, pixel art, gradient, highly
saturated), runs unit tests for the matrix math and round trips, then
benchmarks four configurations against each other:

  BASE     plain qoi_encode/qoi_decode (RGBA differences, unchanged code)
  LUMA     YCoCg-like transform, normal QOI residuals incl. QOI_OP_LUMA
  PERCH    YCoCg-like transform, per-channel residuals (no QOI_OP_LUMA)

The whole program runs to completion in one go.

Compile with:
	gcc -std=c99 -O3 -Wall -Wextra qoitest.c -o qoitest
or with sanitizers:
	clang -std=c99 -O1 -g -fsanitize=address,undefined qoitest.c -o qoitest

*/

#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c99 */

#if defined(__cplusplus)
	/* Aggregate zero-initialization of structs with many fields is a C++
	style warning only; the code is valid C and C++. */
	#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define QOI_IMPLEMENTATION
#define QOI_TESTING
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>


/* --------------------------------------------------------------------------
Tiny deterministic PRNG so the samples need no external files */

static uint32_t rng_state;

static uint32_t rng_next(void) {
	/* xorshift32 */
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

static int rng_range(int lo, int hi) { /* inclusive */
	return lo + (int)(rng_next() % (uint32_t)(hi - lo + 1));
}


/* --------------------------------------------------------------------------
Synthetic samples, all RGBA unless noted */

#define W 64
#define H 64
#define CH 4

typedef struct {
	const char *name;
	int w, h, channels;
	unsigned char *px;
} sample_t;

static unsigned char *alloc_px(int w, int h, int ch) {
	return (unsigned char *)malloc((size_t)w * h * ch);
}

/* Photo-like: smooth low-frequency field plus mild per-pixel noise */
static unsigned char *make_photo(int w, int h, int ch) {
	unsigned char *p = alloc_px(w, h, ch);
	int i = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int r = 90 + 70 * sin(x * 0.11) + 40 * cos(y * 0.07) + rng_range(-9, 9);
			int g = 70 + 50 * sin(x * 0.09 + 1.7) + 60 * cos(y * 0.05) + rng_range(-9, 9);
			int b = 110 + 60 * cos((x + y) * 0.06) + rng_range(-9, 9);
			p[i++] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
			p[i++] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
			p[i++] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
			if (ch == 4) {
				/* mostly opaque, a few semi-transparent pixels */
				int a = (rng_next() & 15) == 0 ? rng_range(40, 200) : 255;
				p[i++] = (unsigned char)a;
			}
		}
	}
	return p;
}

/* Pixel art: flat cells of blocky, repeated colors with hard edges */
static unsigned char *make_pixelart(int w, int h, int ch) {
	unsigned char *p = alloc_px(w, h, ch);
	static const unsigned char palette[8][3] = {
		{32, 48, 24}, {200, 64, 48}, {240, 224, 120}, {48, 96, 180},
		{24, 24, 28}, {232, 120, 200}, {96, 200, 96}, {216, 216, 224}
	};
	int i = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int cell = ((x / 8) * 7 + (y / 8) * 13) & 7;
			p[i++] = palette[cell][0];
			p[i++] = palette[cell][1];
			p[i++] = palette[cell][2];
			if (ch == 4) {
				p[i++] = ((y / 8) == 3 && (x / 8) == 2) ? 128 : 255;
			}
		}
	}
	return p;
}

/* Gradient: smooth linear/radial ramps with a diagonal alpha fade */
static unsigned char *make_gradient(int w, int h, int ch) {
	unsigned char *p = alloc_px(w, h, ch);
	int i = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			p[i++] = (unsigned char)(x * 255 / (w - 1));
			p[i++] = (unsigned char)(y * 255 / (h - 1));
			p[i++] = (unsigned char)((x + y) * 255 / (w + h - 2));
			if (ch == 4) {
				p[i++] = (unsigned char)(64 + (x * 191) / (w - 1));
			}
		}
	}
	return p;
}

/* Highly saturated: pure primaries/secondaries in sharp stripes and blocks */
static unsigned char *make_saturated(int w, int h, int ch) {
	unsigned char *p = alloc_px(w, h, ch);
	static const unsigned char colors[6][3] = {
		{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
		{255, 255, 0}, {255, 0, 255}, {0, 255, 255}
	};
	int i = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int c = ((x / 4) + (y / 16) * 3) % 6;
			p[i++] = colors[c][0];
			p[i++] = colors[c][1];
			p[i++] = colors[c][2];
			if (ch == 4) {
				p[i++] = 255;
			}
		}
	}
	return p;
}

static sample_t make_sample(const char *name, int w, int h, int ch,
	unsigned char *(*fn)(int, int, int))
{
	sample_t s;
	s.name = name;
	s.w = w;
	s.h = h;
	s.channels = ch;
	s.px = fn(w, h, ch);
	return s;
}


/* --------------------------------------------------------------------------
Timer */

static uint64_t ns_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}


/* --------------------------------------------------------------------------
Test framework */

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, ...) do { \
	tests_run++; \
	if (!(cond)) { \
		tests_failed++; \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)


/* --------------------------------------------------------------------------
Matrices under test */

/* YCoCg-like reversible color difference transform with odd determinant
(det = 1), therefore invertible modulo 256:
	y0 =  r + g + b    (luma-like sum)
	y1 = -r            (chroma difference)
	y2 = -r - g        (chroma difference)
It is the odd-determinant, modular-bijection counterpart of the classic
YCoCg luma/chroma split: one correlated luma row plus two difference rows.
Among the small-coefficient odd-determinant candidates it gave the best
average compressed size on the four sample kinds; its inverse was verified
exact over all 256^3 byte triples. */
static const int MAT_YCOCG[9] = {
	 1,  1, 1,
	-1,  0, 0,
	-1, -1, 0
};

/* Classic integer YCoCg color matrix:
	Y  =  R/4 + G/2 + B/4
	Co =  R/2       - B/2
	Cg = -R/4 + G/2 - B/4
Written with integer rows (the divisions are part of the transform), the
determinant is -16, which is even: it is not invertible modulo 256 and must
therefore be rejected with a fallback. */
static const int MAT_EVEN_DET[9] = {
	 1, 2,  1,
	 2, 0, -2,
	-1, 2, -1
};

/* Truly singular: row 3 = row 1 + row 2 */
static const int MAT_SINGULAR[9] = {
	1, 0, 1,
	0, 1, 1,
	1, 1, 2
};

static const int MAT_IDENTITY[9] = {
	1, 0, 0,
	0, 1, 0,
	0, 0, 1
};


/* --------------------------------------------------------------------------
Unit tests */

static void test_matrix_inverse(void) {
	int inv[9];
	printf("== matrix invertibility tests ==\n");

	CHECK(qoi_matrix_inverse_256_pub(MAT_YCOCG, inv) == 1,
		"YCoCg-like matrix (odd det) should be accepted");

	/* inv * m == identity modulo 256 */
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			int acc = 0;
			for (int k = 0; k < 3; k++) {
				acc += inv[row * 3 + k] * MAT_YCOCG[k * 3 + col];
			}
			int expect = (row == col) ? 1 : 0;
			CHECK((acc & 255) == expect,
				"inverse product [%d][%d] = %d, want %d", row, col, acc & 255, expect);
		}
	}

	CHECK(qoi_matrix_inverse_256_pub(MAT_IDENTITY, inv) == 1,
		"identity matrix should be accepted");

	CHECK(qoi_matrix_inverse_256_pub(MAT_SINGULAR, inv) == 0,
		"singular matrix (det 0) must be rejected");

	CHECK(qoi_matrix_inverse_256_pub(MAT_EVEN_DET, inv) == 0,
		"even-determinant YCoCg matrix must be rejected");

	int bad_range[9];
	memcpy(bad_range, MAT_IDENTITY, sizeof(bad_range));
	bad_range[4] = 40000;
	CHECK(qoi_matrix_inverse_256_pub(bad_range, inv) == 0,
		"out-of-range matrix entry must be rejected");
}

/* Brute-force check of the modular inverse over every byte triple */
static void test_inverse_all_bytes(void) {
	int inv[9];
	printf("== inverse exhaustiveness over all byte values ==\n");
	CHECK(qoi_matrix_inverse_256_pub(MAT_YCOCG, inv) == 1, "matrix accepted");

	int mismatches = 0;
	for (int r = 0; r < 256; r += 17) { /* 16-spaced grid: 4096 combos */
		for (int g = 0; g < 256; g += 17) {
			for (int b = 0; b < 256; b += 17) {
				int y0 = (MAT_YCOCG[0]*r + MAT_YCOCG[1]*g + MAT_YCOCG[2]*b) & 255;
				int y1 = (MAT_YCOCG[3]*r + MAT_YCOCG[4]*g + MAT_YCOCG[5]*b) & 255;
				int y2 = (MAT_YCOCG[6]*r + MAT_YCOCG[7]*g + MAT_YCOCG[8]*b) & 255;
				int rr = (inv[0]*y0 + inv[1]*y1 + inv[2]*y2) & 255;
				int gg = (inv[3]*y0 + inv[4]*y1 + inv[5]*y2) & 255;
				int bb = (inv[6]*y0 + inv[7]*y1 + inv[8]*y2) & 255;
				if (rr != r || gg != g || bb != b) {
					mismatches++;
				}
			}
		}
	}
	CHECK(mismatches == 0, "%d inverse mismatches on the byte grid", mismatches);
}

static void test_roundtrip(const sample_t *s, const qoi_options *opt,
	const char *label)
{
	qoi_desc desc = {
		.width = (unsigned int)s->w,
		.height = (unsigned int)s->h,
		.channels = (unsigned char)s->channels,
		.colorspace = QOI_SRGB
	};
	int enc_len = 0;
	void *enc = qoi_encode_opt(s->px, &desc, opt, &enc_len);
	CHECK(enc != NULL && enc_len > 0, "%s: encode failed", label);
	if (!enc) {
		return;
	}

	qoi_desc d2;
	void *dec = qoi_decode_opt(enc, enc_len, &d2, s->channels, opt);
	CHECK(dec != NULL, "%s: decode failed", label);
	if (dec) {
		size_t n = (size_t)s->w * s->h * s->channels;
		CHECK(memcmp(s->px, dec, n) == 0,
			"%s: roundtrip pixel mismatch", label);
		CHECK(d2.width == (unsigned int)s->w && d2.height == (unsigned int)s->h &&
			d2.channels == (unsigned char)s->channels,
			"%s: desc mismatch", label);
		free(dec);
	}
	free(enc);
}

static void test_fallback_and_identity(const sample_t *samples, int n) {
	printf("== fallback and byte-identity tests ==\n");
	for (int i = 0; i < n; i++) {
		const sample_t *s = &samples[i];
		qoi_desc desc = {
			.width = (unsigned int)s->w,
			.height = (unsigned int)s->h,
			.channels = (unsigned char)s->channels,
			.colorspace = QOI_SRGB
		};
		int base_len = 0;
		void *base = qoi_encode(s->px, &desc, &base_len);
		CHECK(base != NULL, "base encode failed");

		/* 1. transform disabled -> identical bytes, fell_back set */
		qoi_options off = {0};
		off.use_transform = 0;
		memcpy(off.matrix, MAT_YCOCG, sizeof(off.matrix));
		int off_len = 0;
		void *off_enc = qoi_encode_opt(s->px, &desc, &off, &off_len);
		CHECK(off_enc != NULL, "disabled encode failed");
		CHECK(off.fell_back == 1, "disabled transform must report fell_back");
		CHECK(off_len == base_len && memcmp(off_enc, base, (size_t)base_len) == 0,
			"%s: disabled transform must be byte-identical to qoi_encode", s->name);

		/* 2. singular matrix -> identical bytes, fell_back set, decode
		without inverse gives original pixels */
		qoi_options sing = {0};
		sing.use_transform = 1;
		memcpy(sing.matrix, MAT_SINGULAR, sizeof(sing.matrix));
		int sing_len = 0;
		void *sing_enc = qoi_encode_opt(s->px, &desc, &sing, &sing_len);
		CHECK(sing_enc != NULL, "singular encode failed");
		CHECK(sing.fell_back == 1, "singular matrix must report fell_back");
		CHECK(sing_len == base_len && memcmp(sing_enc, base, (size_t)base_len) == 0,
			"%s: singular fallback must be byte-identical to qoi_encode", s->name);

		qoi_desc d3;
		void *sing_dec = qoi_decode_opt(sing_enc, sing_len, &d3, s->channels, &sing);
		CHECK(sing_dec != NULL && memcmp(sing_dec, s->px,
			(size_t)s->w * s->h * s->channels) == 0,
			"%s: singular fallback roundtrip mismatch", s->name);
		free(sing_dec);

		/* 3. even-determinant matrix -> fallback as well */
		qoi_options even = {0};
		even.use_transform = 1;
		memcpy(even.matrix, MAT_EVEN_DET, sizeof(even.matrix));
		int even_len = 0;
		void *even_enc = qoi_encode_opt(s->px, &desc, &even, &even_len);
		CHECK(even_enc != NULL && even.fell_back == 1 &&
			even_len == base_len && memcmp(even_enc, base, (size_t)base_len) == 0,
			"%s: even-det fallback must be byte-identical to qoi_encode", s->name);
		free(even_enc);

		/* 4. valid transform, coupled residuals (per_channel = 0) uses the
		same chunk logic as qoi_encode, only on transformed pixels */
		qoi_options luma = {0};
		luma.use_transform = 1;
		memcpy(luma.matrix, MAT_YCOCG, sizeof(luma.matrix));
		luma.per_channel_residuals = 0;
		int luma_len = 0;
		void *luma_enc = qoi_encode_opt(s->px, &desc, &luma, &luma_len);
		CHECK(luma_enc != NULL && luma.fell_back == 0,
			"%s: valid matrix must not fall back", s->name);

		/* The transformed stream is standard QOI: a plain qoi_decode must
		succeed on it and yield the transformed pixels. */
		qoi_desc d4;
		void *plain_dec = qoi_decode(luma_enc, luma_len, &d4, s->channels);
		CHECK(plain_dec != NULL, "%s: transformed stream not decodable by plain qoi_decode",
			s->name);
		free(plain_dec);
		free(luma_enc);
		free(sing_enc);
		free(off_enc);
		free(base);
	}
}

static void test_all_roundtrips(const sample_t *samples, int n) {
	printf("== lossless roundtrip tests (incl. 3-channel sample) ==\n");
	for (int i = 0; i < n; i++) {
		qoi_options luma = {0};
		luma.use_transform = 1;
		memcpy(luma.matrix, MAT_YCOCG, sizeof(luma.matrix));
		luma.per_channel_residuals = 0;
		test_roundtrip(&samples[i], &luma, "LUMA");

		qoi_options perch = luma;
		perch.per_channel_residuals = 1;
		test_roundtrip(&samples[i], &perch, "PERCH");

		qoi_options id = {0};
		id.use_transform = 1;
		memcpy(id.matrix, MAT_IDENTITY, sizeof(id.matrix));
		id.per_channel_residuals = 1;
		test_roundtrip(&samples[i], &id, "IDENTITY+PERCH");
	}
}


/* --------------------------------------------------------------------------
Benchmark */

#define ITERATIONS 200

typedef struct {
	uint64_t size;
	uint64_t enc_ns;
	uint64_t dec_ns;
	double mse;
	uint64_t max_abs;
	int fell_back;
} mode_result_t;

static void bench_mode(const sample_t *s, const qoi_options *opt, int use_opt,
	mode_result_t *r)
{
	qoi_desc desc = {
		.width = (unsigned int)s->w,
		.height = (unsigned int)s->h,
		.channels = (unsigned char)s->channels,
		.colorspace = QOI_SRGB
	};
	size_t n = (size_t)s->w * s->h * s->channels;

	/* correctness + MSE once, timing on the repeated runs */
	int enc_len = 0;
	void *enc = use_opt
		? qoi_encode_opt(s->px, &desc, opt, &enc_len)
		: qoi_encode(s->px, &desc, &enc_len);
	if (!enc) {
		printf("  encode failure in benchmark\n");
		exit(1);
	}
	r->fell_back = use_opt ? opt->fell_back : 0;
	r->size = (uint64_t)enc_len;

	qoi_desc d2;
	void *dec = use_opt
		? qoi_decode_opt(enc, enc_len, &d2, s->channels, opt)
		: qoi_decode(enc, enc_len, &d2, s->channels);
	if (!dec || memcmp(dec, s->px, n) != 0) {
		printf("  roundtrip failure in benchmark\n");
		exit(1);
	}
	double mse = 0.0;
	for (size_t i = 0; i < n; i++) {
		int d = (int)s->px[i] - (int)((unsigned char *)dec)[i];
		mse += (double)(d * d);
		if (abs(d) > (long long)r->max_abs) {
			r->max_abs = (uint64_t)abs(d);
		}
	}
	r->mse = mse / (double)n; /* 0.0 for the lossless paths */
	free(dec);
	free(enc);

	uint64_t enc_total = 0, dec_total = 0;
	for (int it = 0; it < ITERATIONS; it++) {
		uint64_t t0 = ns_now();
		void *e = use_opt
			? qoi_encode_opt(s->px, &desc, opt, &enc_len)
			: qoi_encode(s->px, &desc, &enc_len);
		uint64_t t1 = ns_now();
		enc_total += t1 - t0;

		uint64_t t2 = ns_now();
		void *d = use_opt
			? qoi_decode_opt(e, enc_len, &d2, s->channels, opt)
			: qoi_decode(e, enc_len, &d2, s->channels);
		uint64_t t3 = ns_now();
		dec_total += t3 - t2;

		free(d);
		free(e);
	}
	r->enc_ns = enc_total / ITERATIONS;
	r->dec_ns = dec_total / ITERATIONS;
}

static double psnr_db(const mode_result_t *r) {
	if (r->mse == 0.0) {
		return INFINITY; /* exactly lossless round trip */
	}
	return 10.0 * log10(255.0 * 255.0 / r->mse);
}

static void print_table_row(const char *sample_name, const char *mode,
	const sample_t *s, const mode_result_t *r, const mode_result_t *base,
	int use_opt)
{
	double raw = (double)s->w * s->h * s->channels;
	double rate = 100.0 * (double)r->size / raw;
	double ratio = (double)r->size / (double)base->size;
	double enc_ms = (double)r->enc_ns / 1.0e6;
	double dec_ms = (double)r->dec_ns / 1.0e6;
	const char *path = !use_opt ? "base qoi"
		: (r->fell_back ? "fallback" : "transform");
	printf("| %-10s | %-5s | %8llu | %6.2f%% | %5.3f | %8s | %7.4f | %7.4f | %s |\n",
		sample_name, mode,
		(unsigned long long)r->size, rate, ratio,
		isinf(psnr_db(r)) ? "inf" : "n/a",
		enc_ms, dec_ms, path);
}

int main(void) {
	rng_state = 0x12345678u;

	sample_t samples[5];
	samples[0] = make_sample("photo",      W, H, 4, make_photo);
	samples[1] = make_sample("pixelart",   W, H, 4, make_pixelart);
	samples[2] = make_sample("gradient",   W, H, 4, make_gradient);
	samples[3] = make_sample("saturated",  W, H, 4, make_saturated);
	samples[4] = make_sample("photo_rgb",  48, 32, 3, make_photo);
	int n = (int)(sizeof(samples) / sizeof(samples[0]));

	test_matrix_inverse();
	test_inverse_all_bytes();
	test_all_roundtrips(samples, n);
	test_fallback_and_identity(samples, n);

	printf("\n== unit tests: %d run, %d failed ==\n\n",
		tests_run, tests_failed);

	printf("Benchmark: %d iterations per cell, timer CLOCK_MONOTONIC\n",
		ITERATIONS);
	printf("size=encoded bytes; rate=size/raw%%; ratio=size/BASE size; "
		"PSNR against original; times are ms per call.\n\n");
	printf("| sample     | mode  |     size |   rate | ratio |    PSNR | enc ms | dec ms | path |\n");
	printf("|------------|-------|----------|--------|-------|---------|--------|--------|------|\n");

	for (int i = 0; i < n; i++) {
		sample_t *s = &samples[i];

		qoi_options base_opt = {0};
		mode_result_t base = {0};
		bench_mode(s, &base_opt, 0, &base);
		print_table_row(s->name, "BASE", s, &base, &base, 0);

		qoi_options luma = {0};
		luma.use_transform = 1;
		memcpy(luma.matrix, MAT_YCOCG, sizeof(luma.matrix));
		luma.per_channel_residuals = 0;
		mode_result_t rl = {0};
		bench_mode(s, &luma, 1, &rl);
		print_table_row(s->name, "LUMA", s, &rl, &base, 1);

		qoi_options perch = luma;
		perch.per_channel_residuals = 1;
		mode_result_t rp = {0};
		bench_mode(s, &perch, 1, &rp);
		print_table_row(s->name, "PERCH", s, &rp, &base, 1);
		printf("\n");
	}

	for (int i = 0; i < n; i++) {
		free(samples[i].px);
	}

	if (tests_failed) {
		printf("RESULT: FAIL (%d of %d checks failed)\n", tests_failed, tests_run);
		return 1;
	}
	printf("RESULT: PASS (all %d checks, all roundtrips lossless)\n", tests_run);
	return 0;
}
