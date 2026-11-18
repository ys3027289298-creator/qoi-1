/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT


Determinism checker for the QOI encoder and decoder.

Rationale: a buggy/undefined-behavior ridden codec can produce a byte stream
whose hash differs between machines or optimization levels, even though every
single run "looks fine". This harness proves determinism three ways:

  1. A fixed set of synthetic images (fixed-seed PRNG) is encoded N times in
     the same process; every encoded stream must hash identically, byte for
     byte. The decoded pixels are hashed as well.
  2. The encoded fixtures can be dumped to disk (`dump` mode); streams dumped
     by different builds (e.g. -O0 / -O2 / -O3) are compared byte for byte.
  3. `cmp` mode (and the in-process check) report the exact byte offset of the
     first divergence, plus total size, so a mismatch can be located without a
     hex editor.

Usage:
  qoidet [iters]              run `iters` encode/decode iterations (default
                              10000) and check intra-process determinism
  qoidet dump <out-dir>       write one .qoi fixture per test image
  qoidet cmp <fileA> <fileB>  compare two files, report first byte offset of
                              difference; exit 1 if they differ

Compile, three optimization levels plus an undefined-behavior build:
  gcc -std=c99 -O0 qoidet.c -o qoidet-O0
  gcc -std=c99 -O2 qoidet.c -o qoidet-O2
  gcc -std=c99 -O3 qoidet.c -o qoidet-O3
  clang -std=c99 -O1 -g -fsanitize=undefined,address -fno-sanitize-recover=all \
      qoidet.c -o qoidet-ubsan

See tests/determinism.sh for the full cross-build driver.

*/

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


/* -----------------------------------------------------------------------------
SHA-256 (FIPS 180-4), self contained so the harness has no external deps. */

typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t  buf[64];
	size_t   buflen;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(sha256_ctx *ctx, const uint8_t block[64]) {
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) |
		       ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) |
		       ((uint32_t)block[i * 4 + 3]);
	}
	for (i = 16; i < 64; i++) {
		uint32_t s0 = SHA256_ROTR(w[i - 15], 7) ^
		              SHA256_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = SHA256_ROTR(w[i - 2], 17) ^
		              SHA256_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = ctx->state[0]; b = ctx->state[1];
	c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5];
	g = ctx->state[6]; h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		uint32_t s1 = SHA256_ROTR(e, 6) ^ SHA256_ROTR(e, 11) ^ SHA256_ROTR(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = SHA256_ROTR(a, 2) ^ SHA256_ROTR(a, 13) ^ SHA256_ROTR(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->state[0] += a; ctx->state[1] += b;
	ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f;
	ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx) {
	ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
	ctx->bitlen = 0;
	ctx->buflen = 0;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
	size_t i = 0;
	if (ctx->buflen > 0) {
		while (i < len && ctx->buflen < 64) {
			ctx->buf[ctx->buflen++] = data[i++];
		}
		if (ctx->buflen == 64) {
			sha256_transform(ctx, ctx->buf);
			ctx->bitlen += 512;
			ctx->buflen = 0;
		}
	}
	while (i + 64 <= len) {
		sha256_transform(ctx, data + i);
		ctx->bitlen += 512;
		i += 64;
	}
	while (i < len) {
		ctx->buf[ctx->buflen++] = data[i++];
	}
}

static void sha256_final(sha256_ctx *ctx, uint8_t out[32]) {
	uint64_t bitlen = ctx->bitlen + (uint64_t)ctx->buflen * 8;
	int i;

	ctx->buf[ctx->buflen++] = 0x80;
	if (ctx->buflen > 56) {
		while (ctx->buflen < 64) {
			ctx->buf[ctx->buflen++] = 0;
		}
		sha256_transform(ctx, ctx->buf);
		ctx->buflen = 0;
	}
	while (ctx->buflen < 56) {
		ctx->buf[ctx->buflen++] = 0;
	}
	for (i = 7; i >= 0; i--) {
		ctx->buf[ctx->buflen++] = (uint8_t)(bitlen >> (i * 8));
	}
	sha256_transform(ctx, ctx->buf);

	for (i = 0; i < 8; i++) {
		out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

static void sha256_buf(const uint8_t *data, size_t len, uint8_t out[32]) {
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, out);
}

static void hash_to_hex(const uint8_t hash[32], char out[65]) {
	static const char digits[] = "0123456789abcdef";
	int i;
	for (i = 0; i < 32; i++) {
		out[i * 2]     = digits[hash[i] >> 4];
		out[i * 2 + 1] = digits[hash[i] & 0x0f];
	}
	out[64] = '\0';
}


/* -----------------------------------------------------------------------------
Fixed input: deterministic synthetic images.

The fixtures deliberately stress every encoder branch and every value range
that could expose undefined/implementation-defined behavior:
  - full-range random values, including signed-char wrap boundaries (127/128)
    for the DIFF/LUMA arithmetic in qoi_encode
  - long runs that hit the run-length flush limit (62) and the end-of-image
    flush path
  - smooth gradients sitting on the DIFF/LUMA range thresholds (-2..1,
    -32..31, -8..7)
  - index-array collisions to exercise QOI_OP_INDEX
  - alpha transitions to exercise QOI_OP_RGBA
Everything is generated from fixed seeds, so the input is bit-identical on
every machine. */

typedef struct {
	const char *name;
	int width;
	int height;
	int channels;
	uint8_t *pixels;
} fixture;

/* Two independent fixed-seed generators so neighboring channels/rows do not
share an obvious LCG pattern. */

static uint32_t lcg_state;
static uint32_t lcg_next(void) {
	lcg_state = lcg_state * 1103515245u + 12345u;
	return lcg_state;
}

static uint32_t xs_state;
static uint32_t xorshift_next(void) {
	uint32_t x = xs_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	xs_state = x;
	return x;
}

static fixture make_fixture_random_rgba(int w, int h) {
	fixture f;
	int n = w * h * 4;
	int i;
	f.name = "random_rgba";
	f.width = w; f.height = h; f.channels = 4;
	f.pixels = (uint8_t *)malloc((size_t)n);
	if (!f.pixels) { exit(2); }

	lcg_state = 0x12345678u;
	xs_state  = 0x9abcdef0u;
	for (i = 0; i < n; i += 4) {
		/* Bias some samples at the signed-char wrap boundaries so the
		encoder's signed char difference math (qoi.h) is exercised. */
		uint32_t pick = xorshift_next() % 8;
		static const uint8_t boundary[4] = {0, 127, 128, 255};
		f.pixels[i]     = (pick < 4) ? boundary[pick] : (uint8_t)(lcg_next() >> 16);
		f.pixels[i + 1] = (uint8_t)(xorshift_next() >> 16);
		f.pixels[i + 2] = (uint8_t)(lcg_next() >> 24);
		f.pixels[i + 3] = (pick == 5) ? (uint8_t)(lcg_next() >> 22) : 255;
	}
	return f;
}

static fixture make_fixture_random_rgb(int w, int h) {
	fixture f;
	int n = w * h * 3;
	int i;
	f.name = "random_rgb";
	f.width = w; f.height = h; f.channels = 3;
	f.pixels = (uint8_t *)malloc((size_t)n);
	if (!f.pixels) { exit(2); }

	lcg_state = 0xdeadbeefu;
	xs_state  = 0x0badf00du;
	for (i = 0; i < n; i += 3) {
		f.pixels[i]     = (uint8_t)(lcg_next() >> 16);
		f.pixels[i + 1] = (uint8_t)(xorshift_next() >> 16);
		f.pixels[i + 2] = (uint8_t)(lcg_next() >> 24);
	}
	return f;
}

static fixture make_fixture_runs_rgba(int w, int h) {
	fixture f;
	int npix = w * h;
	int i, j;
	f.name = "runs_rgba";
	f.width = w; f.height = h; f.channels = 4;
	f.pixels = (uint8_t *)malloc((size_t)npix * 4);
	if (!f.pixels) { exit(2); }

	lcg_state = 0x77777777u;
	/* Runs of varied length: many cross the 62-pixel flush boundary and
	some end exactly on the last pixel. */
	i = 0;
	while (i < npix) {
		int run = 1 + (int)(lcg_next() % 130);
		uint8_t r = (uint8_t)(lcg_next() >> 16);
		uint8_t g = (uint8_t)(lcg_next() >> 20);
		uint8_t b = (uint8_t)(lcg_next() >> 24);
		uint8_t a = (i % 500 == 0) ? (uint8_t)(lcg_next() >> 24) : 255;
		if (i + run > npix) { run = npix - i; }
		for (j = 0; j < run; j++) {
			f.pixels[(i + j) * 4]     = r;
			f.pixels[(i + j) * 4 + 1] = g;
			f.pixels[(i + j) * 4 + 2] = b;
			f.pixels[(i + j) * 4 + 3] = a;
		}
		i += run;
	}
	return f;
}

static fixture make_fixture_gradient_rgb(int w, int h) {
	fixture f;
	int npix = w * h;
	int i;
	f.name = "gradient_rgb";
	f.width = w; f.height = h; f.channels = 3;
	f.pixels = (uint8_t *)malloc((size_t)npix * 3);
	if (!f.pixels) { exit(2); }

	/* Per-channel ramps with steps around the DIFF/LUMA thresholds, plus
	wrap-around at 0/255 to exercise unsigned channel wraparound. */
	for (i = 0; i < npix; i++) {
		f.pixels[i * 3]     = (uint8_t)((i * 3) & 0xff);
		f.pixels[i * 3 + 1] = (uint8_t)((i / 30) * 2 + 40);
		f.pixels[i * 3 + 2] = (uint8_t)((255 - i) & 0xff);
	}
	return f;
}

static fixture make_fixture_tiny_rgba(void) {
	/* Minimum-size geometry: run flush on the final pixel, index array
	starting from its zero-initialized state. */
	static const uint8_t px[24] = {
		10, 20, 30, 255,
		10, 20, 30, 255,
		10, 21, 30, 200,
		11, 21, 30, 200,
		250, 251, 252, 0,
		0, 1, 2, 255
	};
	fixture f;
	f.name = "tiny_rgba";
	f.width = 3; f.height = 2; f.channels = 4;
	f.pixels = (uint8_t *)malloc(sizeof(px));
	if (!f.pixels) { exit(2); }
	memcpy(f.pixels, px, sizeof(px));
	return f;
}

#define FIXTURE_COUNT 5

static fixture *make_fixtures(void) {
	fixture *fs = (fixture *)malloc(sizeof(fixture) * FIXTURE_COUNT);
	if (!fs) { exit(2); }
	fs[0] = make_fixture_random_rgba(96, 96);
	fs[1] = make_fixture_random_rgb(96, 64);
	fs[2] = make_fixture_runs_rgba(80, 80);
	fs[3] = make_fixture_gradient_rgb(128, 48);
	fs[4] = make_fixture_tiny_rgba();
	return fs;
}

static void free_fixtures(fixture *fs) {
	int i;
	for (i = 0; i < FIXTURE_COUNT; i++) {
		free(fs[i].pixels);
	}
	free(fs);
}


/* -----------------------------------------------------------------------------
Byte-level comparison with offset reporting. Returns 0 when identical. */

static int compare_bytes(const uint8_t *a, size_t na, const uint8_t *b,
		size_t nb, const char *label_a, const char *label_b) {
	size_t common = na < nb ? na : nb;
	size_t i;

	for (i = 0; i < common; i++) {
		if (a[i] != b[i]) {
			fprintf(stderr,
				"MISMATCH [%s vs %s]: first differing byte at offset %zu "
				"(0x%zx): 0x%02x != 0x%02x\n",
				label_a, label_b, i, i, a[i], b[i]);
			return 1;
		}
	}
	if (na != nb) {
		fprintf(stderr,
			"MISMATCH [%s vs %s]: contents identical up to %zu bytes but "
			"sizes differ: %zu != %zu (first extra offset %zu / 0x%zx)\n",
			label_a, label_b, common, na, nb, common, common);
		return 1;
	}
	return 0;
}

static uint8_t *read_file(const char *path, size_t *out_len) {
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long len;
	size_t got;
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	if (len < 0) { fclose(f); return NULL; }
	rewind(f);
	buf = (uint8_t *)malloc((size_t)len);
	if (!buf) { fclose(f); return NULL; }
	got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) {
		free(buf);
		return NULL;
	}
	*out_len = (size_t)len;
	return buf;
}


/* -----------------------------------------------------------------------------
Modes */

/* One full encode + decode cycle. The output buffers are freshly allocated on
every call, so any reliance on stale heap/stack contents would show up as
inter-iteration hash drift. */
static int run_once(const fixture *fs, uint8_t enc_hashes[][32],
		uint8_t dec_hashes[][32], int *enc_sizes) {
	int i;
	for (i = 0; i < FIXTURE_COUNT; i++) {
		qoi_desc desc;
		int enc_len = 0, dec_len = 0;
		uint8_t *enc, *dec;
		qoi_desc dec_desc;

		desc.width = (unsigned int)fs[i].width;
		desc.height = (unsigned int)fs[i].height;
		desc.channels = (unsigned char)fs[i].channels;
		desc.colorspace = QOI_SRGB;

		enc = (uint8_t *)qoi_encode(fs[i].pixels, &desc, &enc_len);
		if (!enc) {
			fprintf(stderr, "qoi_encode failed for %s\n", fs[i].name);
			return 1;
		}
		sha256_buf(enc, (size_t)enc_len, enc_hashes[i]);
		enc_sizes[i] = enc_len;

		dec = (uint8_t *)qoi_decode(enc, enc_len, &dec_desc, fs[i].channels);
		if (!dec) {
			fprintf(stderr, "qoi_decode failed for %s\n", fs[i].name);
			free(enc);
			return 1;
		}
		dec_len = fs[i].width * fs[i].height * fs[i].channels;
		if (dec_desc.width != desc.width || dec_desc.height != desc.height ||
			dec_desc.channels != desc.channels) {
			fprintf(stderr, "decoded header mismatch for %s\n", fs[i].name);
			free(enc); free(dec);
			return 1;
		}
		if (compare_bytes(dec, (size_t)dec_len, fs[i].pixels,
				(size_t)dec_len, "decoded", "input") != 0) {
			fprintf(stderr, "roundtrip pixel mismatch for %s\n", fs[i].name);
			free(enc); free(dec);
			return 1;
		}
		sha256_buf(dec, (size_t)dec_len, dec_hashes[i]);

		free(dec);
		free(enc);
	}
	return 0;
}

static int mode_test(int iters) {
	fixture *fs = make_fixtures();
	/* Reference hashes/sizes from the first iteration. */
	uint8_t ref_enc[FIXTURE_COUNT][32];
	uint8_t ref_dec[FIXTURE_COUNT][32];
	uint8_t cur_enc[FIXTURE_COUNT][32];
	uint8_t cur_dec[FIXTURE_COUNT][32];
	int ref_sizes[FIXTURE_COUNT];
	int cur_sizes[FIXTURE_COUNT];
	int iter, i, failures = 0;

	if (iters < 1) {
		fprintf(stderr, "iters must be >= 1\n");
		free_fixtures(fs);
		return 2;
	}

	if (run_once(fs, ref_enc, ref_dec, ref_sizes) != 0) {
		free_fixtures(fs);
		return 1;
	}

	for (iter = 1; iter < iters; iter++) {
		if (run_once(fs, cur_enc, cur_dec, cur_sizes) != 0) {
			free_fixtures(fs);
			return 1;
		}
		for (i = 0; i < FIXTURE_COUNT; i++) {
			if (cur_sizes[i] != ref_sizes[i] ||
				memcmp(cur_enc[i], ref_enc[i], 32) != 0) {
				char a[65], b[65];
				hash_to_hex(ref_enc[i], a);
				hash_to_hex(cur_enc[i], b);
				fprintf(stderr,
					"ENCODE NON-DETERMINISM in %s at iteration %d: "
					"size %d -> %d, sha256 %s -> %s\n",
					fs[i].name, iter, ref_sizes[i], cur_sizes[i], a, b);
				failures++;
			}
			if (memcmp(cur_dec[i], ref_dec[i], 32) != 0) {
				char a[65], b[65];
				hash_to_hex(ref_dec[i], a);
				hash_to_hex(cur_dec[i], b);
				fprintf(stderr,
					"DECODE NON-DETERMINISM in %s at iteration %d: "
					"sha256 %s -> %s\n",
					fs[i].name, iter, a, b);
				failures++;
			}
		}
		if (failures > 0) {
			free_fixtures(fs);
			return 1;
		}
	}

	printf("intra-process determinism: %d iterations x %d fixtures, all "
			"hashes identical\n", iters, FIXTURE_COUNT);
	for (i = 0; i < FIXTURE_COUNT; i++) {
		char hex[65];
		hash_to_hex(ref_enc[i], hex);
		printf("  %-13s enc=%5d bytes sha256=%s\n",
			fs[i].name, ref_sizes[i], hex);
	}

	free_fixtures(fs);
	return failures == 0 ? 0 : 1;
}

static int mode_dump(const char *out_dir) {
	fixture *fs = make_fixtures();
	int i, rc = 0;
	for (i = 0; i < FIXTURE_COUNT; i++) {
		char path[1024];
		qoi_desc desc;
		int written;
		snprintf(path, sizeof(path), "%s/%s.qoi", out_dir, fs[i].name);
		desc.width = (unsigned int)fs[i].width;
		desc.height = (unsigned int)fs[i].height;
		desc.channels = (unsigned char)fs[i].channels;
		desc.colorspace = QOI_SRGB;
		written = qoi_write(path, fs[i].pixels, &desc);
		if (written == 0) {
			fprintf(stderr, "failed writing %s\n", path);
			rc = 1;
		}
		else {
			printf("wrote %s (%d bytes)\n", path, written);
		}
	}
	free_fixtures(fs);
	return rc;
}

static int mode_cmp(const char *path_a, const char *path_b) {
	size_t na, nb;
	uint8_t *a = read_file(path_a, &na);
	uint8_t *b = read_file(path_b, &nb);
	int rc;
	if (!a || !b) {
		free(a); free(b);
		return 2;
	}
	rc = compare_bytes(a, na, b, nb, path_a, path_b);
	if (rc == 0) {
		char ha[65], hb[65];
		uint8_t ha_raw[32], hb_raw[32];
		sha256_buf(a, na, ha_raw);
		sha256_buf(b, nb, hb_raw);
		hash_to_hex(ha_raw, ha);
		hash_to_hex(hb_raw, hb);
		printf("identical: %zu bytes, sha256=%s\n", na, ha);
	}
	free(a); free(b);
	return rc;
}

int main(int argc, char **argv) {
	if (argc >= 3 && strcmp(argv[1], "dump") == 0) {
		return mode_dump(argv[2]);
	}
	if (argc >= 4 && strcmp(argv[1], "cmp") == 0) {
		return mode_cmp(argv[2], argv[3]);
	}
	if (argc == 1) {
		return mode_test(10000);
	}
	if (argc == 2) {
		char *end;
		long iters = strtol(argv[1], &end, 10);
		if (*end != '\0' || iters < 1 || iters > 100000000L) {
			fprintf(stderr,
				"usage: %s [iters] | dump <out-dir> | cmp <a> <b>\n",
				argv[0]);
			return 2;
		}
		return mode_test((int)iters);
	}
	fprintf(stderr, "usage: %s [iters] | dump <out-dir> | cmp <a> <b>\n",
		argv[0]);
	return 2;
}
