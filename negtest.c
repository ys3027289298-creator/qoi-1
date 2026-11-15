/*

Test for the QOI_NEGATIVE flag (negative/inverted image storage):

  1. pixel-exact roundtrip: qoi_encode_negative -> qoi_decode == original
  2. size change vs. normal qoi_encode
  3. behavior when a decoder ignores the flag (decodes the stored negative)
  4. encode timing: qoi_encode vs. qoi_encode_negative
  5. illegal flag bits in the colorspace header byte are rejected

*/

#define _POSIX_C_SOURCE 199309L
#define QOI_IMPLEMENTATION
#include "qoi.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
	if (cond) { printf("  PASS: "); } \
	else { printf("  FAIL: "); failures++; } \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

static unsigned int rng_state = 12345;
static unsigned int rng(void) {
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static void gen_a(unsigned char *px, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			unsigned char *p = px + (y * w + x) * 3;
			p[0] = (unsigned char)(x * 4);
			p[1] = (unsigned char)(y * 4);
			p[2] = (unsigned char)((x + y) * 2);
		}
	}
}

static void gen_b(unsigned char *px, int w, int h) {
	for (int i = 0; i < w * h; i++) {
		unsigned char *p = px + i * 4;
		p[0] = (unsigned char)rng();
		p[1] = (unsigned char)rng();
		p[2] = (unsigned char)rng();
		p[3] = (unsigned char)(128 + (rng() & 127));
	}
}

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int count_diff(const unsigned char *a, const unsigned char *b, int n) {
	int diff = 0;
	for (int i = 0; i < n; i++) {
		diff += a[i] != b[i];
	}
	return diff;
}

static void test_image(const char *name, unsigned char *img, int w, int h,
		int channels, int colorspace, int iterations) {
	qoi_desc desc = {(unsigned int)w, (unsigned int)h,
		(unsigned char)channels, (unsigned char)colorspace};
	qoi_desc out_desc;
	int px_len = w * h * channels;
	int len_normal = 0, len_neg = 0;
	unsigned char *enc_normal = qoi_encode(img, &desc, &len_normal);
	unsigned char *enc_neg = qoi_encode_negative(img, &desc, &len_neg);

	printf("image %s: %dx%d, %d channels, colorspace %d\n",
		name, w, h, channels, colorspace);

	CHECK(enc_normal != NULL && enc_neg != NULL, "both encodes succeed");

	printf("  [header] normal: channels=%d colorspace=0x%02x | "
		"negative: channels=%d colorspace=0x%02x\n",
		enc_normal[12], enc_normal[13], enc_neg[12], enc_neg[13]);
	CHECK(enc_neg[12] == channels, "channels byte untouched by flag");
	CHECK(enc_neg[13] == (colorspace | QOI_NEGATIVE),
		"negative flag set in colorspace byte");
	CHECK(enc_normal[13] == colorspace, "normal encode header unchanged");

	/* 1. pixel-exact roundtrip */
	unsigned char *dec = qoi_decode(enc_neg, len_neg, &out_desc, 0);
	CHECK(dec != NULL, "decode of negative image succeeds");
	int mismatches = dec ? count_diff(dec, img, px_len) : px_len;
	printf("  [roundtrip] decoded-vs-original mismatches: %d / %d bytes\n",
		mismatches, px_len);
	CHECK(mismatches == 0, "negative roundtrip is pixel-exact");
	CHECK(out_desc.channels == channels &&
		out_desc.colorspace == colorspace,
		"desc reports original channels/colorspace (flag masked out)");

	/* 2. size change */
	printf("  [size] normal=%d bytes, negative=%d bytes, delta=%+d (%+.2f%%)\n",
		len_normal, len_neg, len_neg - len_normal,
		100.0 * (len_neg - len_normal) / len_normal);

	/* 3. behavior when the flag is ignored: clear the flag and decode.
	   The stored pixel data is the negative of the original. */
	unsigned char *patched = malloc(len_neg);
	memcpy(patched, enc_neg, len_neg);
	patched[13] &= ~QOI_NEGATIVE;
	unsigned char *dec_ignored = qoi_decode(patched, len_neg, &out_desc, 0);
	CHECK(dec_ignored != NULL, "flag-ignoring decode still succeeds");
	int mism_ignored = dec_ignored ? count_diff(dec_ignored, img, px_len) : -1;
	int inverted_ok = 1;
	if (dec_ignored) {
		for (int i = 0; i < w * h; i++) {
			for (int c = 0; c < 3; c++) {
				if (dec_ignored[i * channels + c] !=
					(unsigned char)(255 - img[i * channels + c])) {
					inverted_ok = 0;
				}
			}
			if (channels == 4 &&
				dec_ignored[i * channels + 3] != img[i * channels + 3]) {
				inverted_ok = 0;
			}
		}
	}
	printf("  [flag ignored] decoded-vs-original mismatches: %d / %d bytes "
		"(alpha matches, rgb differ)\n", mism_ignored, px_len);
	CHECK(mism_ignored == w * h * 3,
		"ignoring the flag yields the wrong (inverted) image");
	CHECK(inverted_ok, "flag-ignored output is exactly 255-v on rgb");

	/* 4. encode timing */
	double t0 = now_ms();
	for (int i = 0; i < iterations; i++) {
		void *e = qoi_encode(img, &desc, &len_normal);
		free(e);
	}
	double t1 = now_ms();
	for (int i = 0; i < iterations; i++) {
		void *e = qoi_encode_negative(img, &desc, &len_neg);
		free(e);
	}
	double t2 = now_ms();
	double mpix = (double)w * h / 1e6;
	printf("  [encode time] %d iterations: normal=%.1f ms (%.2f Mpx/s), "
		"negative=%.1f ms (%.2f Mpx/s), overhead=%+.1f%%\n",
		iterations, t1 - t0, mpix * iterations / ((t1 - t0) / 1e3),
		t2 - t1, mpix * iterations / ((t2 - t1) / 1e3),
		100.0 * ((t2 - t1) - (t1 - t0)) / (t1 - t0));

	free(enc_normal);
	free(enc_neg);
	free(dec);
	free(patched);
	free(dec_ignored);
}

int main(void) {
	static unsigned char img_a[64 * 64 * 3];
	static unsigned char img_b[48 * 48 * 4];
	gen_a(img_a, 64, 64);
	gen_b(img_b, 48, 48);

	test_image("A (smooth gradient)", img_a, 64, 64, 3, QOI_SRGB, 20000);
	test_image("B (random noise)", img_b, 48, 48, 4, QOI_LINEAR, 20000);

	/* 5. illegal flag bits must be rejected */
	printf("illegal/valid colorspace header bytes:\n");
	qoi_desc desc = {64, 64, 3, QOI_SRGB};
	int len = 0;
	unsigned char *enc = qoi_encode(img_a, &desc, &len);
	unsigned char *buf = malloc(len);
	qoi_desc out_desc;
	static const int valid[] = {0x00, 0x01, 0x80, 0x81};
	static const int invalid[] = {0x02, 0x03, 0x40, 0x7f, 0x82, 0xff};
	for (int i = 0; i < 4; i++) {
		memcpy(buf, enc, len);
		buf[13] = (unsigned char)valid[i];
		void *d = qoi_decode(buf, len, &out_desc, 0);
		CHECK(d != NULL, "colorspace byte 0x%02x accepted", valid[i]);
		free(d);
	}
	for (int i = 0; i < 6; i++) {
		memcpy(buf, enc, len);
		buf[13] = (unsigned char)invalid[i];
		void *d = qoi_decode(buf, len, &out_desc, 0);
		CHECK(d == NULL, "colorspace byte 0x%02x rejected", invalid[i]);
		free(d);
	}

	free(buf);
	free(enc);

	printf("%s\n", failures ? "RESULT: FAILURES" : "RESULT: ALL PASS");
	return failures ? 1 : 0;
}
