/*

Test & metrics for the temporal-stable dithering helpers (qoi_td_*) in qoi.h.

Covers:
  1. Parameter validation: illegal levels / phase_step must be rejected.
  2. Single-frame qoi_encode() output must be byte-for-byte unchanged
     (length + FNV-1a hash against the pristine implementation).
  3. Phase discontinuity: per-pixel scene-cut reset and qoi_td_reset()
     at loop boundaries.
  4. Metrics on four synthetic animations (static / slow / fast / loop):
     inter-frame flicker, average error, temporal bias, encoded size -
     compared against plain rounding, static dither and per-frame random
     dither baselines.

Self-contained, no third-party dependencies. Build:
	gcc -std=c99 -O2 -Wall qoi_td_test.c -o qoi_td_test -lm

*/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

#define W 64
#define H 64
#define CH 3
#define T 40
#define LEVELS 8
#define PHASE_STEP 4

static int failures = 0;

static void gen_loop(unsigned char *f, int t);

#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static uint64_t fnv1a(const unsigned char *p, long n) {
	uint64_t h = 1469598103934665603ULL;
	long i;
	for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
	return h;
}

/* ------------------------------------------------------------------ */
/* 1. Parameter validation                                             */
/* ------------------------------------------------------------------ */

static void test_params(void) {
	qoi_td_state st;
	qoi_desc desc = {W, H, CH, QOI_SRGB};
	qoi_desc bad;
	unsigned char frame[W * H * CH], out[W * H * CH];
	int len;

	memset(frame, 128, sizeof(frame));

	/* illegal levels */
	CHECK(!qoi_td_init(&st, &desc, 0, 4),   "levels=0 accepted");
	CHECK(!qoi_td_init(&st, &desc, 1, 4),   "levels=1 accepted");
	CHECK(!qoi_td_init(&st, &desc, 256, 4), "levels=256 accepted");
	CHECK(!qoi_td_init(&st, &desc, -8, 4),  "levels=-8 accepted");
	/* illegal phase step */
	CHECK(!qoi_td_init(&st, &desc, 8, -1),  "phase_step=-1 accepted");
	CHECK(!qoi_td_init(&st, &desc, 8, 256), "phase_step=256 accepted");
	CHECK(!qoi_td_init(&st, &desc, 8, 1000),"phase_step=1000 accepted");
	/* illegal desc / NULL */
	CHECK(!qoi_td_init(&st, NULL, 8, 4),    "NULL desc accepted");
	CHECK(!qoi_td_init(NULL, &desc, 8, 4),  "NULL state accepted");
	bad = desc; bad.width = 0;
	CHECK(!qoi_td_init(&st, &bad, 8, 4),    "width=0 accepted");
	bad = desc; bad.channels = 2;
	CHECK(!qoi_td_init(&st, &bad, 8, 4),    "channels=2 accepted");
	bad = desc; bad.channels = 5;
	CHECK(!qoi_td_init(&st, &bad, 8, 4),    "channels=5 accepted");
	bad = desc; bad.colorspace = 2;
	CHECK(!qoi_td_init(&st, &bad, 8, 4),    "colorspace=2 accepted");

	/* legal boundary values */
	CHECK(qoi_td_init(&st, &desc, 2, 0),   "levels=2, phase_step=0 rejected");
	qoi_td_free(&st);
	CHECK(qoi_td_init(&st, &desc, 255, 255), "levels=255, phase_step=255 rejected");
	qoi_td_free(&st);

	/* NULL handling of quantize/encode on a valid state */
	CHECK(qoi_td_init(&st, &desc, LEVELS, PHASE_STEP), "valid init rejected");
	CHECK(!qoi_td_quantize(&st, NULL, out), "quantize accepted NULL data");
	CHECK(!qoi_td_quantize(&st, frame, NULL), "quantize accepted NULL out");
	CHECK(qoi_td_encode(&st, NULL, &len) == NULL, "encode accepted NULL data");
	CHECK(qoi_td_encode(NULL, frame, &len) == NULL, "encode accepted NULL state");
	CHECK(qoi_td_encode(&st, frame, NULL) == NULL, "encode accepted NULL out_len");
	CHECK(qoi_td_quantize(&st, frame, out), "first quantize failed");
	qoi_td_free(&st);
	qoi_td_free(&st); /* double free must be safe */

	printf("params: %s\n", failures ? "FAILURES (see above)" : "all checks passed");
}

/* ------------------------------------------------------------------ */
/* 2. Single-frame encoding must be byte-for-byte unchanged            */
/* ------------------------------------------------------------------ */

static void test_single_frame_unchanged(void) {
	unsigned char px[64 * 64 * 3];
	qoi_desc desc = {64, 64, 3, QOI_SRGB}, d2;
	int x, y, len = 0, ok;
	void *enc, *dec;

	for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) {
		int i = (y * 64 + x) * 3;
		px[i+0] = (unsigned char)(x*4 + ((x^y)&1)*32);
		px[i+1] = (unsigned char)(y*4 + (x*y)%17);
		px[i+2] = (unsigned char)((x+y)*2 + ((x>32&&y<32)?80:0));
	}

	enc = qoi_encode(px, &desc, &len);
	CHECK(enc != NULL, "qoi_encode failed");
	/* reference values from the unmodified qoi.h (git HEAD) */
	CHECK(len == 16403, "qoi_encode length changed");
	CHECK(fnv1a(enc, len) == 0xa5f74bc6c9b51cd0ULL, "qoi_encode bytes changed");

	dec = qoi_decode(enc, len, &d2, 3);
	ok = dec != NULL && d2.width == 64 && d2.height == 64;
	if (ok) { ok = memcmp(dec, px, sizeof(px)) == 0; }
	CHECK(ok, "single-frame roundtrip broken");

	printf("single-frame: len=%d hash=%016llx roundtrip=%s\n",
		len, (unsigned long long)fnv1a(enc, len), ok ? "ok" : "FAIL");
	free(enc);
	free(dec);
}

/* ------------------------------------------------------------------ */
/* 3. Phase discontinuity: scene-cut reset and loop reset              */
/* ------------------------------------------------------------------ */

static void test_scene_cut(void) {
	qoi_td_state st;
	qoi_desc desc = {1, 1, 3, QOI_SRGB};
	unsigned char a[3] = {200, 200, 200};
	unsigned char b[3] = {100, 100, 100};
	unsigned char out[3];
	int c;

	CHECK(qoi_td_init(&st, &desc, 2, 4), "cut-test init failed");

	/* Frame A (200): builds up err = 200-255 = -55 per channel.
	   Frame B (100): |100-200| = 100 > QOI_TD_CUT_THRESHOLD, so the
	   accumulated error must be dropped. Hand-computed with reset:
	   seed=158, phase=(158+1*4+0)&255=162, off=34, v=100+0+34=134,
	   k=(134+127)/255=1, quant=255, err=100-255=-155.
	   Without the reset the stale err=-55 would give v=25 -> quant 0. */
	CHECK(qoi_td_quantize(&st, a, out), "cut-test frame A failed");
	CHECK(st.err[0] == -55, "cut-test frame A err != -55");
	CHECK(qoi_td_quantize(&st, b, out), "cut-test frame B failed");
	for (c = 0; c < 3; c++) {
		CHECK(out[c] == 255, "scene cut: stale error bled into new scene");
		CHECK(st.err[c] == -155, "scene cut: accumulator not reset");
	}
	qoi_td_free(&st);
	printf("scene-cut: accumulator reset verified (out=255, err=-155 as computed)\n");
}

static void test_loop_reset(void) {
	qoi_td_state st;
	qoi_desc desc = {W, H, CH, QOI_SRGB};
	unsigned char *frame = malloc(W * H * CH);
	unsigned char *enc_a = NULL, *enc_b = NULL;
	int len_a = 0, len_b = 0, t, identical_without_reset;

	CHECK(qoi_td_init(&st, &desc, LEVELS, PHASE_STEP), "loop-test init failed");

	/* Encode one full loop of the pulsing animation (see gen_loop). */
	for (t = 0; t < T; t++) {
		int len;
		void *enc;
		gen_loop(frame, t);
		enc = qoi_td_encode(&st, frame, &len);
		CHECK(enc != NULL, "loop-test encode failed");
		if (t == 0) { enc_a = enc; len_a = len; } else { free(enc); }
	}

	/* Re-encode frame 0 without reset: the dither state has moved on,
	   so the loop seam does not match. */
	gen_loop(frame, 0);
	enc_b = qoi_td_encode(&st, frame, &len_b);
	identical_without_reset =
		(len_a == len_b && memcmp(enc_a, enc_b, len_a) == 0);
	free(enc_b);

	/* With qoi_td_reset() at the loop point the sequence restarts
	   deterministically: identical input must give identical bytes. */
	qoi_td_reset(&st);
	gen_loop(frame, 0);
	enc_b = qoi_td_encode(&st, frame, &len_b);
	CHECK(len_a == len_b && memcmp(enc_a, enc_b, len_a) == 0,
		"loop reset: identical frame did not reproduce identical bytes");

	printf("loop: seam without reset %s; with qoi_td_reset() byte-identical\n",
		identical_without_reset ? "matched (unexpected)" : "differed");

	free(enc_a);
	free(enc_b);
	free(frame);
	qoi_td_free(&st);
}

/* ------------------------------------------------------------------ */
/* 4. Animations and metrics                                           */
/* ------------------------------------------------------------------ */

static double base_value(int x, int y, int c) {
	double v = (c == 0) ? x * 4.0 : (c == 1) ? y * 4.0 : (x + y) * 2.0;
	return v + 8.0 * sin(x * 0.7) * sin(y * 0.9);
}

static void put(unsigned char *f, int x, int y, int c, double v) {
	if (v < 0) { v = 0; }
	if (v > 255) { v = 255; }
	f[(y * W + x) * CH + c] = (unsigned char)(v + 0.5);
}

static void gen_static(unsigned char *f, int t) {
	int x, y, c;
	(void)t;
	for (y = 0; y < H; y++) for (x = 0; x < W; x++) for (c = 0; c < CH; c++) {
		put(f, x, y, c, base_value(x, y, c));
	}
}

static void gen_blob(unsigned char *f, int t, double speed) {
	double cx = 32.0 + t * speed, cy = 32.0 - t * speed * 0.4;
	int x, y, c;
	for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
		double dx = x - cx, dy = y - cy;
		double blob = 60.0 * exp(-(dx*dx + dy*dy) / 128.0);
		for (c = 0; c < CH; c++) {
			put(f, x, y, c, base_value(x, y, c) + blob);
		}
	}
}

static void gen_slow(unsigned char *f, int t) { gen_blob(f, t, 0.3); }
static void gen_fast(unsigned char *f, int t) { gen_blob(f, t, 3.0); }

static void gen_loop(unsigned char *f, int t) {
	double pulse = 40.0 * sin(2.0 * 3.14159265358979 * t / T);
	int x, y, c;
	for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
		double dx = x - 32.0, dy = y - 32.0;
		double ring = sin(sqrt(dx*dx + dy*dy) * 0.5);
		for (c = 0; c < CH; c++) {
			put(f, x, y, c, base_value(x, y, c) + pulse * ring);
		}
	}
}

/* quantization helpers for the baseline schemes */
static int quant_round(int v, int q) {
	int k = (v * q + 127) / 255;
	return (k * 255 + q / 2) / q;
}

static int dither_off(unsigned int phase, int q) {
	return ((int)(2 * phase + 1 - 256) * 255) / (512 * q);
}

static int quant_dither(int v, int q, unsigned int phase) {
	v += dither_off(phase, q);
	if (v < 0) { v = 0; }
	if (v > 255) { v = 255; }
	return quant_round(v, q);
}

static uint32_t rng_state = 0x12345678u;
static uint32_t xorshift(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

enum { SCHEME_ROUND, SCHEME_STATIC, SCHEME_RANDOM, SCHEME_TD, N_SCHEMES };
static const char *scheme_names[N_SCHEMES] = {
	"round", "static-dither", "random-dither", "temporal-td"
};

typedef struct {
	double flicker; /* mean |Ddec - Dsrc| between consecutive frames */
	double avgerr;  /* mean |dec - src| */
	double bias;    /* mean over pixels of |mean over time of (dec-src)| */
	long   size;    /* total qoi bytes */
} metrics_t;

static void run_scheme(int scheme, void (*gen)(unsigned char *, int),
                       int phase_step, metrics_t *m) {
	unsigned char *src[2], *dec[2], *quant;
	double *acc; /* per pixel+channel temporal error accumulator for bias */
	double flick = 0, err = 0, bias = 0;
	long size = 0;
	long nstatic;
	int t, i;
	long n = W * H * CH;
	qoi_td_state st;
	qoi_desc desc = {W, H, CH, QOI_SRGB};

	src[0] = malloc(n); src[1] = malloc(n);
	dec[0] = malloc(n); dec[1] = malloc(n);
	quant = malloc(n);
	acc = calloc(n, sizeof(double));

	if (scheme == SCHEME_TD) {
		if (!qoi_td_init(&st, &desc, LEVELS, phase_step)) {
			printf("td init failed\n");
			exit(1);
		}
	}

	gen(src[0], 0);
	for (t = 0; t < T; t++) {
		qoi_desc dd;
		int len = 0, di = t & 1;
		void *enc, *decf;

		gen(src[di], t);

		switch (scheme) {
		case SCHEME_ROUND:
			for (i = 0; i < n; i++) {
				quant[i] = (unsigned char)quant_round(src[di][i], LEVELS - 1);
			}
			break;
		case SCHEME_STATIC:
			for (i = 0; i < n; i += CH) {
				unsigned int seed = ((i / CH + 1) * 2654435761u) >> 24;
				int c;
				for (c = 0; c < CH; c++) {
					quant[i+c] = (unsigned char)quant_dither(src[di][i+c], LEVELS - 1, seed);
				}
			}
			break;
		case SCHEME_RANDOM:
			for (i = 0; i < n; i += CH) {
				unsigned int phase = xorshift() & 255;
				int c;
				for (c = 0; c < CH; c++) {
					quant[i+c] = (unsigned char)quant_dither(src[di][i+c], LEVELS - 1, phase);
				}
			}
			break;
		default: /* SCHEME_TD */
			if (!qoi_td_quantize(&st, src[di], quant)) {
				printf("td quantize failed\n");
				exit(1);
			}
			break;
		}

		enc = qoi_encode(quant, &desc, &len);
		if (!enc) { printf("encode failed\n"); exit(1); }
		size += len;
		decf = qoi_decode(enc, len, &dd, CH);
		if (!decf) { printf("decode failed\n"); exit(1); }
		memcpy(dec[di], decf, n);
		free(enc);
		free(decf);

		for (i = 0; i < n; i++) {
			double e = (double)dec[di][i] - (double)src[di][i];
			err += fabs(e);
			acc[i] += e;
			if (t > 0) {
				double ds = (double)src[di][i] - (double)src[di^1][i];
				double dq = (double)dec[di][i] - (double)dec[di^1][i];
				flick += fabs(dq - ds);
			}
		}
	}

	nstatic = (long)(T - 1) * n;
	for (i = 0; i < n; i++) { bias += fabs(acc[i] / T); }

	m->flicker = flick / nstatic;
	m->avgerr = err / ((long)T * n);
	m->bias = bias / n;
	m->size = size;

	if (scheme == SCHEME_TD) { qoi_td_free(&st); }
	free(src[0]); free(src[1]); free(dec[0]); free(dec[1]);
	free(quant); free(acc);
}

static void test_metrics(void) {
	struct { const char *name; void (*gen)(unsigned char *, int); } anims[] = {
		{"static", gen_static},
		{"slow",   gen_slow},
		{"fast",   gen_fast},
		{"loop",   gen_loop},
	};
	int a, s;

	printf("\nmetrics (%dx%dx%d, %d frames, %d levels, td phase_step=%d/256):\n",
		W, H, CH, T, LEVELS, PHASE_STEP);
	printf("%-8s %-15s %10s %10s %10s %10s\n",
		"anim", "scheme", "flicker", "avgerr", "bias", "qoi-bytes");
	for (a = 0; a < 4; a++) {
		for (s = 0; s < N_SCHEMES; s++) {
			metrics_t m;
			run_scheme(s, anims[a].gen, PHASE_STEP, &m);
			printf("%-8s %-15s %10.3f %10.3f %10.3f %10ld\n",
				anims[a].name, scheme_names[s],
				m.flicker, m.avgerr, m.bias, m.size);
		}
	}

	/* configurable inter-frame phase delta, swept on the static clip */
	printf("\nphase_step sweep (static clip, temporal-td):\n");
	printf("%-12s %10s %10s %10s %10s\n",
		"phase_step", "flicker", "avgerr", "bias", "qoi-bytes");
	{
		int steps[] = {0, 1, 4, 16, 64};
		for (s = 0; s < 5; s++) {
			metrics_t m;
			run_scheme(SCHEME_TD, gen_static, steps[s], &m);
			printf("%-12d %10.3f %10.3f %10.3f %10ld\n",
				steps[s], m.flicker, m.avgerr, m.bias, m.size);
		}
	}
}

int main(void) {
	test_params();
	test_single_frame_unchanged();
	test_scene_cut();
	test_loop_reset();
	test_metrics();

	printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
	return failures != 0;
}
