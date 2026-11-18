/* Correctness tests for the configurable QOI encoder cache.

Build (from the repository root):
	cc -std=gnu99 -O2 -I. tests/qoi_cache_test.c -o /tmp/qoi_cache_test

Checks:
  1. qoi_encode() is byte-identical to the pre-change implementation
     (golden SHA-256 values generated from git HEAD's pristine qoi.h).
  2. Default qoi_encode_options() (NULL cfg, all-zero cfg, and
     {64, FIXED, 0}) is byte-identical to qoi_encode().
  3. Every valid configuration round-trips losslessly through qoi_decode().
  4. Stats are coherent: lookups <= pixels; hits <= lookups; hit rate ==
     hits/lookups; cache_bytes is constant across all configs and images.
  5. Invalid configurations are rejected (NULL, stats zeroed, out_len
     untouched), while qoi_encode()'s existing argument validation is intact.
  6. Cache memory is a constant: exactly one allocation (the output buffer),
     the cache itself is on the stack, and cache_bytes == 512 for every
     configuration and image size.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocation counting, wired into the library through QOI_MALLOC/QOI_FREE.
   Must be declared before qoi.h. */
static size_t alloc_live_count = 0;
static size_t alloc_peak_count = 0;

static void *counting_malloc(size_t sz) {
	void *p = malloc(sz);
	if (p) {
		alloc_live_count++;
		if (alloc_live_count > alloc_peak_count) alloc_peak_count = alloc_live_count;
	}
	return p;
}

static void counting_free(void *p) {
	if (p) alloc_live_count--;
	free(p);
}

#define QOI_MALLOC(sz) counting_malloc(sz)
#define QOI_FREE(p)    counting_free(p)
#define QOI_IMPLEMENTATION
#include "qoi.h"
#include "materials.h"

/* ---------------------------------------------------------------------------
Minimal SHA-256, test-only, used to compare against golden digests. */

typedef struct {
	unsigned int h[8];
	unsigned long long len;
	unsigned char buf[64];
	size_t buflen;
} sha256_ctx;

static const unsigned int sha_k[64] = {
	0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
	0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
	0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
	0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
	0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
	0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
	0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
	0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
	0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
	0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
	0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const unsigned char *p) {
	unsigned int w[64], a, b, cc, d, e, f, g, h, t1, t2;
	int i;
	for (i = 0; i < 16; i++) {
		w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
			((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
	}
	for (i = 16; i < 64; i++) {
		unsigned int s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
		unsigned int s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (i = 0; i < 64; i++) {
		unsigned int s1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
		unsigned int ch = (e & f) ^ (~e & g);
		unsigned int s0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
		unsigned int maj = (a & b) ^ (a & cc) ^ (b & cc);
		t1 = h + s1 + ch + sha_k[i] + w[i];
		t2 = s0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c) {
	c->h[0]=0x6a09e667u; c->h[1]=0xbb67ae85u; c->h[2]=0x3c6ef372u; c->h[3]=0xa54ff53au;
	c->h[4]=0x510e527fu; c->h[5]=0x9b05688cu; c->h[6]=0x1f83d9abu; c->h[7]=0x5be0cd19u;
	c->len = 0; c->buflen = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n) {
	const unsigned char *p = (const unsigned char *)data;
	c->len += (unsigned long long)n;
	while (n > 0) {
		size_t take = 64 - c->buflen;
		if (take > n) take = n;
		memcpy(c->buf + c->buflen, p, take);
		c->buflen += take; p += take; n -= take;
		if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
	}
}

static void sha256_final(sha256_ctx *c, unsigned char out[32]) {
	unsigned long long bits = c->len * 8;
	unsigned char pad = 0x80;
	int i;
	sha256_update(c, &pad, 1);
	pad = 0;
	while (c->buflen != 56) sha256_update(c, &pad, 1);
	for (i = 7; i >= 0; i--) {
		unsigned char b = (unsigned char)(bits >> (i * 8));
		sha256_update(c, &b, 1);
	}
	for (i = 0; i < 8; i++) {
		out[i*4+0] = (unsigned char)(c->h[i] >> 24);
		out[i*4+1] = (unsigned char)(c->h[i] >> 16);
		out[i*4+2] = (unsigned char)(c->h[i] >> 8);
		out[i*4+3] = (unsigned char)(c->h[i]);
	}
}

static void sha256_hex(const void *data, size_t n, char out[65]) {
	sha256_ctx c;
	unsigned char d[32];
	int i;
	sha256_init(&c);
	sha256_update(&c, data, n);
	sha256_final(&c, d);
	for (i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
	out[64] = 0;
}

/* ---------------------------------------------------------------------------
Golden digests of qoi_encode() output from the pristine qoi.h at git HEAD. */

typedef struct {
	int material, w, h, channels;
	const char *sha256;
} golden_t;

static const golden_t goldens[] = {
	{0, 64, 64, 4, "b452ed4ba29441e2f5cde5490b70848a050ca876b36e71bfe1e139574b97f7bf"},
	{0, 97, 61, 3, "74296d7552b61f3ace5988d52a18cbe2886c7b2bb193ab2f87abd7b2384cfd4c"},
	{1, 64, 64, 4, "305381fcede7873a0b6e035aba1f33d5e2d007274b13c82f51dd218cdd917364"},
	{1, 129, 33, 3, "af838c10db7d7de2968c8ee6a2bb0e22594ac815b6a36ca83c03683e25236492"},
	{2, 64, 64, 4, "18944f3140e9777ce076534339ca3be6c70e04d39de0e2aaaddc67e395fcd4be"},
	{2, 200, 37, 3, "96b031a382516f127997cd7b87c2a21ba76e7cb8ca5979b9cafad0f058ce3028"},
	{3, 64, 64, 4, "a5fb19ebde4ee184dbf112ecd8966f096df6e23f7200f51339133e6fd0f48b34"},
	{3, 31, 31, 4, "793032cbaf5af60494c410d8f68a67e282637af62eb990ee1991ed5d119da01d"},
	{1, 1, 1, 4, "88bedec88aa67cff16940711ada65fbd379017bdb5535768d90213580bd4abee"},
	{2, 3, 1, 4, "cb660baa84cd74ddeb482e18b1599bcd42e4721dd0cf7a570a1db9ddc834ee94"}
};

static int n_goldens = (int)(sizeof(goldens) / sizeof(goldens[0]));

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static qoi_desc make_desc(int w, int h, int ch) {
	qoi_desc d;
	d.width = (unsigned int)w;
	d.height = (unsigned int)h;
	d.channels = (unsigned char)ch;
	d.colorspace = QOI_SRGB;
	return d;
}

static void test_byte_identity(void) {
	int i, cfg_i;
	for (i = 0; i < n_goldens; i++) {
		const golden_t *g = &goldens[i];
		unsigned char *px = qoi_test_gen(g->material)(g->w, g->h, g->channels);
		qoi_desc d = make_desc(g->w, g->h, g->channels);
		int len = 0, len2 = 0;
		void *enc = qoi_encode(px, &d, &len);
		char hex[65];
		CHECK(enc != NULL && len > 0, "qoi_encode failed for golden %d", i);
		if (enc) {
			sha256_hex(enc, (size_t)len, hex);
			CHECK(strcmp(hex, g->sha256) == 0,
				"qoi_encode digest changed for %s %dx%d ch%d: %s != %s",
				qoi_test_name(g->material), g->w, g->h, g->channels, hex, g->sha256);

			{
				qoi_encode_cfg zero; memset(&zero, 0, sizeof(zero));
				qoi_encode_cfg explicit_default;
				const qoi_encode_cfg *cfgs[3];
				explicit_default.cache_size = 64;
				explicit_default.cache_policy = QOI_CACHE_FIXED;
				explicit_default.report = 0;
				cfgs[0] = NULL; cfgs[1] = &zero; cfgs[2] = &explicit_default;
				for (cfg_i = 0; cfg_i < 3; cfg_i++) {
					void *e2 = qoi_encode_options(px, &d, &len2, cfgs[cfg_i], NULL);
					CHECK(e2 != NULL && len2 == len && memcmp(e2, enc, (size_t)len) == 0,
						"default options %d differ from qoi_encode for golden %d", cfg_i, i);
					free(e2);
				}
			}
		}
		free(enc);
		free(px);
	}
}

static void test_roundtrip_and_stats(void) {
	static const int sizes[3] = {16, 32, 64};
	static const int policies[2] = {QOI_CACHE_FIXED, QOI_CACHE_LRU};
	static const struct { int w, h; } dims[] = {
		{1, 1}, {2, 2}, {17, 5}, {64, 64}, {97, 61}
	};
	int m, si, pi, ch, di;
	unsigned int cache_bytes = 0;
	int cache_bytes_seen = 0;

	for (di = 0; di < (int)(sizeof(dims) / sizeof(dims[0])); di++) {
		for (m = 0; m < 4; m++) {
			for (ch = 3; ch <= 4; ch++) {
				int w = dims[di].w, h = dims[di].h;
				unsigned char *px = qoi_test_gen(m)(w, h, ch);
				qoi_desc d = make_desc(w, h, ch);
				for (si = 0; si < 3; si++) {
					for (pi = 0; pi < 2; pi++) {
						qoi_encode_cfg cfg;
						qoi_encode_stats st;
						int len = 0;
						void *enc, *dec;
						qoi_desc dd;

						cfg.cache_size = sizes[si];
						cfg.cache_policy = policies[pi];
						cfg.report = 1;

						memset(&st, 0xa5, sizeof(st));
						enc = qoi_encode_options(px, &d, &len, &cfg, &st);
						CHECK(enc != NULL && len > 0, "encode failed");
						if (!enc) { free(px); continue; }

						dec = qoi_decode(enc, len, &dd, ch);
						CHECK(dec != NULL, "decode failed");
						CHECK(dec && dd.width == (unsigned)w && dd.height == (unsigned)h &&
							dd.channels == ch, "decoded desc mismatch");
						CHECK(dec && memcmp(dec, px, (size_t)w * h * ch) == 0,
							"round-trip mismatch %s %dx%d ch%d size%d policy%d",
							qoi_test_name(m), w, h, ch, sizes[si], policies[pi]);

						CHECK(st.pixels == (unsigned long long)w * h,
							"pixels %llu != %d", st.pixels, w * h);
						CHECK(st.index_lookups <= st.pixels, "lookups > pixels");
						CHECK(st.index_hits <= st.index_lookups, "hits > lookups");
						CHECK(st.cache_size == (unsigned)sizes[si], "reported size");
						CHECK(st.cache_policy == (unsigned)policies[pi], "reported policy");
						{
							double rate = st.index_lookups
								? (double)st.index_hits / (double)st.index_lookups : 0.0;
							double diff = st.index_hit_rate - rate;
							CHECK(diff > -1e-12 && diff < 1e-12, "hit rate mismatch");
						}
						CHECK(st.cache_bytes > 0 && st.cache_bytes <= QOI_CACHE_BYTES_MAX,
							"cache_bytes %u out of bounds", st.cache_bytes);
						if (!cache_bytes_seen) {
							cache_bytes = st.cache_bytes;
							cache_bytes_seen = 1;
						}
						CHECK(st.cache_bytes == cache_bytes,
							"cache_bytes varies: %u vs %u", st.cache_bytes, cache_bytes);

						free(dec);
						free(enc);
					}
				}
				free(px);
			}
		}
	}
}

/* Fixed/64 must reproduce the original encoder byte for byte. */
static void test_fixed64_equals_reference(void) {
	int m, ch;
	for (m = 0; m < 4; m++) {
		for (ch = 3; ch <= 4; ch++) {
			int w = 64, h = 64;
			unsigned char *px = qoi_test_gen(m)(w, h, ch);
			qoi_desc d = make_desc(w, h, ch);
			int len = 0, len2 = 0;
			void *a = qoi_encode(px, &d, &len);
			qoi_encode_cfg cfg;
			qoi_encode_stats st;
			void *b;
			cfg.cache_size = 64;
			cfg.cache_policy = QOI_CACHE_FIXED;
			cfg.report = 1;
			CHECK(a != NULL, "reference encode failed");
			b = qoi_encode_options(px, &d, &len2, &cfg, &st);
			CHECK(b != NULL && len2 == len && memcmp(a, b, (size_t)len) == 0,
				"fixed/64 differs from qoi_encode for %s ch%d", qoi_test_name(m), ch);
			free(a); free(b); free(px);
		}
	}
}

static void test_invalid_params(void) {
	static const int bad_sizes[] = {-1, 1, 8, 15, 17, 24, 31, 33, 63, 65, 128, 1000,
		0x7fffffff, 0x80000000u};
	static const int bad_policies[] = {-1, 2, 3, 99, 0x7fffffff};
	static const int bad_reports[] = {-1, 2, 99, 0x7fffffff};
	int i, w = 8, h = 8, ch = 4;
	unsigned char *pxbuf = qoi_test_gen(2)(w, h, ch);
	qoi_desc d = make_desc(w, h, ch);

	CHECK(qoi_encode_cfg_valid(NULL) == 1, "NULL cfg must be valid");

	for (i = 0; i < (int)(sizeof(bad_sizes) / sizeof(bad_sizes[0])); i++) {
		qoi_encode_cfg cfg;
		qoi_encode_stats st;
		void *e;
		int len;
		cfg.cache_size = bad_sizes[i];
		cfg.cache_policy = QOI_CACHE_FIXED;
		cfg.report = 0;
		CHECK(qoi_encode_cfg_valid(&cfg) == 0, "size %d accepted", bad_sizes[i]);
		memset(&st, 0xa5, sizeof(st));
		len = 123;
		e = qoi_encode_options(pxbuf, &d, &len, &cfg, &st);
		CHECK(e == NULL, "size %d not rejected", bad_sizes[i]);
		CHECK(len == 123, "out_len modified on cfg error");
		CHECK(st.pixels == 0 && st.index_hits == 0 && st.cache_bytes == 0 &&
			st.index_hit_rate == 0.0, "stats not zeroed on cfg error");
		free(e);
	}
	for (i = 0; i < (int)(sizeof(bad_policies) / sizeof(bad_policies[0])); i++) {
		qoi_encode_cfg cfg;
		int len = 0;
		cfg.cache_size = 32;
		cfg.cache_policy = bad_policies[i];
		cfg.report = 0;
		CHECK(qoi_encode_cfg_valid(&cfg) == 0, "policy %d accepted", bad_policies[i]);
		CHECK(qoi_encode_options(pxbuf, &d, &len, &cfg, NULL) == NULL,
			"policy %d not rejected", bad_policies[i]);
	}
	for (i = 0; i < (int)(sizeof(bad_reports) / sizeof(bad_reports[0])); i++) {
		qoi_encode_cfg cfg;
		cfg.cache_size = 32;
		cfg.cache_policy = QOI_CACHE_LRU;
		cfg.report = bad_reports[i];
		CHECK(qoi_encode_cfg_valid(&cfg) == 0, "report %d accepted", bad_reports[i]);
	}

	{
		qoi_encode_cfg ok;
		qoi_desc bad;
		int l = 0;
		ok.cache_size = 16;
		ok.cache_policy = QOI_CACHE_LRU;
		ok.report = 1;
		CHECK(qoi_encode_options(NULL, &d, &l, &ok, NULL) == NULL, "data NULL");
		CHECK(qoi_encode_options(pxbuf, NULL, &l, &ok, NULL) == NULL, "desc NULL");
		CHECK(qoi_encode_options(pxbuf, &d, NULL, &ok, NULL) == NULL, "out_len NULL");
		bad = make_desc(0, 8, 4);
		CHECK(qoi_encode_options(pxbuf, &bad, &l, &ok, NULL) == NULL, "width 0");
		bad = make_desc(8, 0, 4);
		CHECK(qoi_encode_options(pxbuf, &bad, &l, &ok, NULL) == NULL, "height 0");
		bad = make_desc(8, 8, 2);
		CHECK(qoi_encode_options(pxbuf, &bad, &l, &ok, NULL) == NULL, "channels 2");
		bad = make_desc(8, 8, 5);
		CHECK(qoi_encode_options(pxbuf, &bad, &l, &ok, NULL) == NULL, "channels 5");
		bad = make_desc(8, 8, 4);
		bad.colorspace = 2;
		CHECK(qoi_encode_options(pxbuf, &bad, &l, &ok, NULL) == NULL, "colorspace 2");
		bad = make_desc((int)(QOI_PIXELS_MAX / 2), 4, 4);
		CHECK(qoi_encode_options(pxbuf, &bad, &l, &ok, NULL) == NULL, "too large");

		CHECK(qoi_encode(NULL, &d, &l) == NULL, "qoi_encode NULL data");
		CHECK(qoi_encode(pxbuf, NULL, &l) == NULL, "qoi_encode NULL desc");
		CHECK(qoi_decode(NULL, 10, NULL, 4) == NULL, "qoi_decode NULL");
	}

	free(pxbuf);
}

static void test_memory_ceiling(void) {
	typedef char ceiling_check[(QOI_CACHE_BYTES_MAX <= 512) ? 1 : -1];
	(void)sizeof(ceiling_check);

	{
		qoi_encode_cfg cfg;
		qoi_encode_stats st;
		unsigned char *px = qoi_test_gen(0)(1, 1, 4);
		qoi_desc d = make_desc(1, 1, 4);
		int len = 0;
		void *e;

		cfg.cache_size = 64;
		cfg.cache_policy = QOI_CACHE_LRU;
		cfg.report = 1;

		alloc_live_count = alloc_peak_count = 0;
		e = qoi_encode_options(px, &d, &len, &cfg, &st);
		/* Only the output buffer is heap-allocated; cache state is on the
		   stack and never scales with image size or configuration. */
		CHECK(alloc_peak_count == 1, "expected 1 allocation, saw %lu",
			(unsigned long)alloc_peak_count);
		CHECK(e != NULL && len > 0, "encode failed in memory test");
		CHECK(st.cache_bytes == 512, "cache_bytes is not 512 (%u)", st.cache_bytes);
		CHECK(st.cache_bytes <= QOI_CACHE_BYTES_MAX, "cache above ceiling");
		CHECK(alloc_live_count == 1, "allocation leaked or extra allocations");
		QOI_FREE(e);
		CHECK(alloc_live_count == 0, "free did not balance");
		free(px);
	}
}

static void test_randomized_differential(void) {
	/* Sweep many pseudo-random images and geometries through every
	   configuration. Each must (a) round-trip losslessly and (b) use only
	   indices the decoder can resolve; (c) the default config must match
	   qoi_encode() byte for byte. */
	unsigned int seed = 0xC0FFEEu;
	int iter, si, pi;
	for (iter = 0; iter < 40; iter++) {
		int w = 1 + (int)(qoi_test_rand(&seed) % 41);
		int h = 1 + (int)(qoi_test_rand(&seed) % 41);
		int ch = 3 + (int)(qoi_test_rand(&seed) % 2);
		int kind = (int)(qoi_test_rand(&seed) % 4);
		unsigned char *px = qoi_test_alloc(w, h, ch);
		unsigned char *enc_ref;
		qoi_desc d = make_desc(w, h, ch);
		int ref_len = 0, i, n = w * h;
		/* Mix pure noise with structured runs so hits are exercised too. */
		for (i = 0; i < n; i++) {
			unsigned int v = qoi_test_rand(&seed);
			if ((qoi_test_rand(&seed) % 5) != 0 && i > 0) {
				memcpy(px + i * ch, px + (i - 1) * ch, (size_t)ch);
			}
			else {
				px[i * ch + 0] = (unsigned char)v;
				px[i * ch + 1] = (unsigned char)(v >> 8);
				px[i * ch + 2] = (unsigned char)(v >> 16);
				if (ch == 4) px[i * ch + 3] = (unsigned char)(kind == 3 ? 255 : v >> 24);
			}
		}
		enc_ref = qoi_encode(px, &d, &ref_len);
		CHECK(enc_ref != NULL, "reference encode failed");
		for (si = 0; si < 3; si++) {
			static const int sizes[3] = {16, 32, 64};
			for (pi = 0; pi < 2; pi++) {
				qoi_encode_cfg cfg;
				qoi_encode_stats st;
				qoi_desc dd;
				int len = 0;
				void *enc, *dec;
				cfg.cache_size = sizes[si];
				cfg.cache_policy = pi ? QOI_CACHE_LRU : QOI_CACHE_FIXED;
				cfg.report = 1;
				enc = qoi_encode_options(px, &d, &len, &cfg, &st);
				CHECK(enc != NULL && len > 0, "encode failed iter %d", iter);
				if (!enc) continue;
				dec = qoi_decode(enc, len, &dd, ch);
				CHECK(dec && memcmp(dec, px, (size_t)n * ch) == 0,
					"round-trip iter %d %dx%d ch%d size%d policy%d",
					iter, w, h, ch, sizes[si], pi);
				if (sizes[si] == 64 && pi == 0 && enc_ref) {
					CHECK(len == ref_len && memcmp(enc, enc_ref, (size_t)len) == 0,
						"default config differs iter %d", iter);
				}
				free(dec);
				free(enc);
			}
		}
		/* Report off: stats must be all zero even though encoding succeeded. */
		{
			qoi_encode_cfg cfg;
			qoi_encode_stats st;
			int len = 0;
			void *enc;
			cfg.cache_size = 32;
			cfg.cache_policy = QOI_CACHE_LRU;
			cfg.report = 0;
			memset(&st, 0xa5, sizeof(st));
			enc = qoi_encode_options(px, &d, &len, &cfg, &st);
			CHECK(enc != NULL, "report-off encode failed");
			CHECK(st.pixels == 0 && st.index_lookups == 0 && st.index_hits == 0 &&
				st.cache_bytes == 0 && st.index_hit_rate == 0.0,
				"stats written while report == 0 (iter %d)", iter);
			free(enc);
		}
		free(enc_ref);
		free(px);
	}
}

int main(void) {
	printf("=== configurable QOI cache: correctness ===\n");

	test_byte_identity();
	test_fixed64_equals_reference();
	test_roundtrip_and_stats();
	test_randomized_differential();
	test_invalid_params();
	test_memory_ceiling();

	printf("checks: %d, failures: %d\n", checks, failures);
	printf("%s\n", failures ? "=== FAIL ===" : "=== PASS ===");
	return failures ? 1 : 0;
}
