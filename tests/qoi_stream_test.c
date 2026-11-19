/* Correctness tests for the QOI streaming encoder and the qoistream driver.

Two layers:

  * the library: qoi_stream_* must reproduce qoi_encode() byte for byte,
    whatever the input is split into, and save/load must round-trip the
    encoder state;
  * the driver: on an 8K image, every quota in QUOTAS must succeed, the output
    must be byte-identical for all of them, the process must stay inside the
    quota, and a killed run must resume to the same bytes.

Run with: tests/run_tests.sh  (builds into a temp dir, needs only a C compiler)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include "materials.h"

static int checks, failures;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_MSG(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* --------------------------------------------------------------------------
Collecting sink */

typedef struct {
	unsigned char *p;
	size_t len, cap;
	int fail_at; /* fail the call after this many bytes, -1 to never fail */
	int calls;
} collector;

static int collect_sink(void *user, const void *bytes, int len) {
	collector *c = (collector *)user;
	c->calls++;
	if (c->fail_at >= 0 && c->len + (size_t)len > (size_t)c->fail_at) {
		return 1;
	}
	if (c->len + (size_t)len > c->cap) {
		size_t want = c->cap ? c->cap : 4096;
		while (want < c->len + (size_t)len) {
			want *= 2;
		}
		c->p = (unsigned char *)realloc(c->p, want);
		c->cap = want;
	}
	memcpy(c->p + c->len, bytes, (size_t)len);
	c->len += (size_t)len;
	return 0;
}

/* --------------------------------------------------------------------------
Reference: the original whole-image encoder */

static unsigned char *reference(const unsigned char *px, const qoi_desc *d, int *len) {
	return (unsigned char *)qoi_encode(px, d, len);
}

/* --------------------------------------------------------------------------
Library: identity with qoi_encode() for adversarial split patterns */

static void test_split_identity(void) {
	static const int sizes[][2] = {
		{1, 1}, {1, 2}, {2, 1}, {3, 1}, {1, 3}, {7, 5}, {8, 8}, {13, 7},
		{64, 33}, {100, 3}, {256, 2}, {1, 300}, {255, 255}, {320, 240}, {63, 65}
	};
	static const int stage_sizes[] = {32, 33, 64, 1000, 65536};
	int si, ch, kind, sp, ss;
	int cases = 0;

	for (si = 0; si < (int)(sizeof sizes / sizeof sizes[0]); si++) {
		for (ch = 3; ch <= 4; ch++) {
			for (kind = 0; kind < QOI_TEST_KINDS; kind++) {
				int w = sizes[si][0], h = sizes[si][1];
				unsigned char *px = qoi_test_image(w, h, ch, kind);
				qoi_desc d;
				int ref_len = 0;
				unsigned char *ref;
				d.width = (unsigned)w;
				d.height = (unsigned)h;
				d.channels = (unsigned char)ch;
				d.colorspace = QOI_SRGB;
				ref = reference(px, &d, &ref_len);
				CHECK(ref != NULL);
				if (!ref) {
					free(px);
					continue;
				}

				for (ss = 0; ss < (int)(sizeof stage_sizes / sizeof stage_sizes[0]); ss++) {
					for (sp = 0; sp < 7; sp++) {
						unsigned char *stage = (unsigned char *)malloc((size_t)stage_sizes[ss]);
						collector c;
						qoi_stream_encoder enc;
						size_t off = 0, n = (size_t)w * h * ch;
						unsigned int rng = 0x1234u + (unsigned int)(sp * 977 + ss);
						int rc;

						memset(&c, 0, sizeof c);
						c.fail_at = -1;
						rc = qoi_stream_init(&enc, &d, stage,
							stage_sizes[ss], collect_sink, &c);
						CHECK_MSG(rc == 0, "init failed for %dx%d stage %d",
							w, h, stage_sizes[ss]);
						if (rc != 0) {
							free(stage);
							continue;
						}
						while (off < n) {
							int take;
							int consumed = 0;
							switch (sp) {
							case 0: take = (int)n; break;
							case 1: take = 1; break;
							case 2: take = ch; break;
							case 3: take = ch * 7 + 1; break;
							case 4: take = (int)(n / 3 + 1); break;
							case 5: take = 5; break;
							default: take = (int)(qoi_test_rand(&rng) % 103u) + 1; break;
							}
							if ((size_t)take > n - off) {
								take = (int)(n - off);
							}
							rc = qoi_stream_feed(&enc, px + off, take, &consumed);
							CHECK_MSG(rc == 0 && consumed == take,
								"feed %dx%d ch%d kind%d split%d: rc=%d consumed=%d/%d",
								w, h, ch, kind, sp, rc, consumed, take);
							if (rc != 0 || consumed != take) {
								break;
							}
							off += (size_t)take;
						}
						rc = qoi_stream_end(&enc);
						CHECK_MSG(rc == 0, "end %dx%d", w, h);
						CHECK_MSG(c.len == (size_t)ref_len &&
							memcmp(c.p, ref, (size_t)ref_len) == 0,
							"%dx%d ch%d kind%d stage %d split %d: %zu vs %d bytes",
							w, h, ch, kind, stage_sizes[ss], sp,
							c.len, ref_len);
						cases++;
						free(c.p);
						free(stage);
					}
				}
				free(ref);
				free(px);
			}
		}
	}
	CHECK(cases > 1000);
	printf("split identity: %d cases\n", cases);
}

/* --------------------------------------------------------------------------
Library: state save/load round-trip and exact resume */

static void test_resume_state(void) {
	static const int sizes[][2] = {{1, 1}, {2, 1}, {9, 9}, {64, 64}, {200, 3}};
	int si, ch, kind;
	int cases = 0;

	for (si = 0; si < (int)(sizeof sizes / sizeof sizes[0]); si++) {
		for (ch = 3; ch <= 4; ch++) {
			for (kind = 0; kind < QOI_TEST_KINDS; kind++) {
				int w = sizes[si][0], h = sizes[si][1];
				size_t n = (size_t)w * h * ch;
				unsigned char *px = qoi_test_image(w, h, ch, kind);
				qoi_desc d;
				int ref_len = 0;
				unsigned char *ref;
				size_t cut;

				d.width = (unsigned)w;
				d.height = (unsigned)h;
				d.channels = (unsigned char)ch;
				d.colorspace = QOI_LINEAR;
				ref = reference(px, &d, &ref_len);
				CHECK(ref != NULL);
				if (!ref) {
					free(px);
					continue;
				}

				for (cut = 0; cut <= n; cut += (n / 7) + 1) {
					unsigned char *stage_a = (unsigned char *)malloc(64);
					unsigned char *stage_b = (unsigned char *)malloc(4096);
					unsigned char state[QOI_STREAM_STATE_SIZE];
					collector a, b;
					qoi_stream_encoder enc_a, enc_b;
					int state_len, rc;
					int consumed;

					memset(&a, 0, sizeof a);
					memset(&b, 0, sizeof b);
					a.fail_at = -1;
					b.fail_at = -1;

					CHECK(qoi_stream_init(&enc_a, &d, stage_a, 64,
						collect_sink, &a) == 0);
					rc = qoi_stream_feed(&enc_a, px, (int)cut, &consumed);
					CHECK(rc == 0 && (size_t)consumed == cut);
					CHECK(qoi_stream_consumed(&enc_a) == (int)cut);

					/* Saving flushes: everything encoded so far is in a. */
					memset(state, 0xa5, sizeof state);
					state_len = qoi_stream_save_state(&enc_a, state, sizeof state);
					CHECK(state_len > 0 && state_len <= QOI_STREAM_STATE_SIZE);

					/* A fresh encoder continues from the state. */
					CHECK(qoi_stream_init(&enc_b, &d, stage_b, 4096,
						collect_sink, &b) == 0);
					CHECK(qoi_stream_load_state(&enc_b, state, state_len) == 0);
					CHECK(qoi_stream_consumed(&enc_b) == (int)cut);
					if (cut < n) {
						CHECK(qoi_stream_feed(&enc_b, px + cut, (int)(n - cut),
							&consumed) == 0);
						CHECK((size_t)consumed == n - cut);
					}
					CHECK(qoi_stream_end(&enc_b) == 0);

					/* Bytes already emitted plus the continuation must be
					the reference stream, with no gap and no overlap. */
					CHECK_MSG(a.len + b.len == (size_t)ref_len,
						"%dx%d ch%d kind%d cut %zu: %zu + %zu != %d",
						w, h, ch, kind, cut, a.len, b.len, ref_len);
					if (a.len + b.len == (size_t)ref_len) {
						CHECK(memcmp(a.p, ref, a.len) == 0);
						CHECK(memcmp(b.p, ref + a.len, b.len) == 0);
					}
					cases++;
					free(a.p);
					free(b.p);
					free(stage_a);
					free(stage_b);
				}
				free(ref);
				free(px);
			}
		}
	}
	printf("resume state: %d cuts\n", cases);
}

/* --------------------------------------------------------------------------
Library: a chain of save/load round-trips must equal one straight stream

A real resumed job may be interrupted (and checkpointed) many times. Every
round here saves the state, throws the encoder away, and continues in a fresh
encoder with a *different* staging buffer, which is what the driver does. */

static void test_resume_chain(void) {
	static const int sizes[][2] = {{1, 1}, {5, 3}, {64, 64}, {200, 3}};
	static const int stage_sizes[] = {32, 33, 4096};
	int si, ch, kind, ss;
	int cases = 0;

	for (si = 0; si < (int)(sizeof sizes / sizeof sizes[0]); si++) {
		for (ch = 3; ch <= 4; ch++) {
			for (kind = 0; kind < QOI_TEST_KINDS; kind++) {
				int w = sizes[si][0], h = sizes[si][1];
				size_t n = (size_t)w * h * ch;
				unsigned char *px = qoi_test_image(w, h, ch, kind);
				qoi_desc d;
				int ref_len = 0;
				unsigned char *ref;
				collector acc;
				size_t off = 0;
				unsigned int rng = 0x51ed270bu + (unsigned)kind * 977u + (unsigned)ch;
				unsigned char state[QOI_STREAM_STATE_SIZE];
				int state_len = 0;
				int steps = 0;

				d.width = (unsigned)w;
				d.height = (unsigned)h;
				d.channels = (unsigned char)ch;
				d.colorspace = QOI_SRGB;
				ref = reference(px, &d, &ref_len);
				CHECK(ref != NULL);
				if (!ref) {
					free(px);
					continue;
				}

				memset(&acc, 0, sizeof acc);
				acc.fail_at = -1;

				while (off < n) {
					unsigned char stage[8192];
					qoi_stream_encoder enc;
					int take = (int)(qoi_test_rand(&rng) % 97u) + 1;

					if ((size_t)take > n - off) {
						take = (int)(n - off);
					}
					ss = (int)(qoi_test_rand(&rng) % 3u);
					CHECK(qoi_stream_init(&enc, &d, stage,
						stage_sizes[ss], collect_sink, &acc) == 0);
					if (steps > 0) {
						CHECK(qoi_stream_load_state(&enc, state, state_len) == 0);
					}
					{
						int consumed = 0;
						CHECK(qoi_stream_feed(&enc, px + off, take, &consumed) == 0);
						CHECK(consumed == take);
						CHECK(qoi_stream_consumed(&enc) == (int)(off + (size_t)take));
					}
					/* Saving flushes, so acc holds every byte produced so far. */
					state_len = qoi_stream_save_state(&enc, state, sizeof state);
					CHECK(state_len == QOI_STREAM_STATE_SIZE);
					off += (size_t)take;
					steps++;
				}

				/* An encoder that has not been fed everything must refuse to
				finish; this is checked on a throw-away encoder so the real
				chain is unaffected. */
				{
					unsigned char stage[8192];
					qoi_stream_encoder enc;
					CHECK(qoi_stream_init(&enc, &d, stage, sizeof stage,
						collect_sink, &acc) == 0);
					CHECK(qoi_stream_end(&enc) == QOI_STREAM_BAD_ARG);
				}

				/* The last encoder is gone; continue from the last checkpoint
				in one more fresh encoder with a different staging buffer. */
				{
					unsigned char stage[8192];
					qoi_stream_encoder enc;
					CHECK(qoi_stream_init(&enc, &d, stage, sizeof stage,
						collect_sink, &acc) == 0);
					CHECK(qoi_stream_load_state(&enc, state, state_len) == 0);
					CHECK(qoi_stream_end(&enc) == 0);
				}

				CHECK_MSG(acc.len == (size_t)ref_len &&
					memcmp(acc.p, ref, (size_t)ref_len) == 0,
					"%dx%d ch%d kind%d: chain gave %zu bytes, expected %d",
					w, h, ch, kind, acc.len, ref_len);
				cases++;
				free(acc.p);
				free(ref);
				free(px);
			}
		}
	}
	printf("resume chain: %d images\n", cases);
}

/* --------------------------------------------------------------------------
Library: invalid arguments and tampered state */

static void test_invalid(void) {
	unsigned char stage[64];
	unsigned char state[QOI_STREAM_STATE_SIZE];
	unsigned char blob[QOI_STREAM_STATE_SIZE];
	unsigned char px[16 * 16 * 4];
	collector c;
	qoi_stream_encoder enc, enc2;
	qoi_desc d;
	int consumed = 12345;
	int len;

	memset(&c, 0, sizeof c);
	c.fail_at = -1;
	{
		unsigned int seed = 99;
		int i;
		for (i = 0; i < (int)sizeof px; i += 4) {
			unsigned int r = qoi_test_rand(&seed);
			px[i + 0] = (unsigned char)r;
			px[i + 1] = (unsigned char)(r >> 8);
			px[i + 2] = (unsigned char)(r >> 16);
			px[i + 3] = (unsigned char)(r >> 24);
		}
	}

	d.width = 16; d.height = 16; d.channels = 4; d.colorspace = 0;

	CHECK(qoi_stream_init(NULL, &d, stage, 64, collect_sink, &c) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_init(&enc, NULL, stage, 64, collect_sink, &c) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_init(&enc, &d, NULL, 64, collect_sink, &c) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_init(&enc, &d, stage, 64, NULL, &c) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_init(&enc, &d, stage, QOI_STREAM_MIN_BUF - 1,
		collect_sink, &c) == QOI_STREAM_BAD_ARG);
	d.width = 0;
	CHECK(qoi_stream_init(&enc, &d, stage, 64, collect_sink, &c) == QOI_STREAM_BAD_ARG);
	d.width = 16; d.channels = 2;
	CHECK(qoi_stream_init(&enc, &d, stage, 64, collect_sink, &c) == QOI_STREAM_BAD_ARG);
	d.channels = 4; d.colorspace = 2;
	CHECK(qoi_stream_init(&enc, &d, stage, 64, collect_sink, &c) == QOI_STREAM_BAD_ARG);
	d.colorspace = 0;
	CHECK(qoi_stream_init(&enc, &d, stage, 64, collect_sink, &c) == 0);

	CHECK(qoi_stream_feed(NULL, px, 4, &consumed) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_feed(&enc, NULL, 4, &consumed) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_feed(&enc, px, -1, &consumed) == QOI_STREAM_BAD_ARG);

	/* Ending before all pixels were fed is refused. */
	CHECK(qoi_stream_feed(&enc, px, 100, &consumed) == 0);
	CHECK(qoi_stream_end(&enc) == QOI_STREAM_BAD_ARG);

	/* Finish this encoder, then check that extra input is rejected. */
	CHECK(qoi_stream_feed(&enc, px + 100, (int)sizeof(px) - 100, &consumed) == 0);
	CHECK(qoi_stream_end(&enc) == 0);
	CHECK(qoi_stream_feed(&enc, px, 4, &consumed) == QOI_STREAM_BAD_ARG);

	/* One byte too many is refused, and nothing is consumed beyond the image. */
	CHECK(qoi_stream_init(&enc, &d, stage, 64, collect_sink, &c) == 0);
	consumed = 0;
	CHECK(qoi_stream_feed(&enc, px, (int)sizeof(px) + 1, &consumed) == QOI_STREAM_BAD_ARG);
	CHECK(consumed == (int)sizeof(px));

	/* Save/load argument checks. */
	CHECK(qoi_stream_init(&enc, &d, stage, 64, collect_sink, &c) == 0);
	CHECK(qoi_stream_feed(&enc, px, (int)sizeof(px) / 2, &consumed) == 0);
	CHECK(qoi_stream_save_state(&enc, NULL, sizeof state) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_save_state(&enc, state, QOI_STREAM_STATE_SIZE - 1) == QOI_STREAM_BAD_ARG);
	len = qoi_stream_save_state(&enc, state, sizeof state);
	CHECK(len > 0);
	CHECK(qoi_stream_init(&enc2, &d, stage, 64, collect_sink, &c) == 0);
	CHECK(qoi_stream_load_state(NULL, state, len) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_load_state(&enc2, NULL, len) == QOI_STREAM_BAD_ARG);
	CHECK(qoi_stream_load_state(&enc2, state, len - 1) == QOI_STREAM_BAD_ARG);
	/* Any flipped bit must be caught. */
	{
		int i, detected = 0;
		for (i = 0; i < len; i++) {
			memcpy(blob, state, (size_t)len);
			blob[i] ^= 0x01;
			if (qoi_stream_load_state(&enc2, blob, len) != 0) {
				detected++;
			}
		}
		CHECK_MSG(detected == len, "only %d of %d single-bit flips detected",
			detected, len);
	}
	/* State for another image is refused. */
	d.width = 15;
	CHECK(qoi_stream_init(&enc2, &d, stage, 64, collect_sink, &c) == 0);
	CHECK(qoi_stream_load_state(&enc2, state, len) == QOI_STREAM_BAD_ARG);

	/* A failing sink stops the stream and is reported. */
	d.width = 16;
	free(c.p);          /* discard everything the collector holds so far */
	memset(&c, 0, sizeof c);
	c.fail_at = 100;
	CHECK(qoi_stream_init(&enc, &d, stage, 32, collect_sink, &c) == 0);
	{
		int rc = qoi_stream_feed(&enc, px, (int)sizeof(px), &consumed);
		CHECK(rc == QOI_STREAM_SINK_FAILED || rc == 0);
		if (rc == 0) {
			CHECK(qoi_stream_end(&enc) == QOI_STREAM_SINK_FAILED);
		}
	}
	free(c.p);
}

/* --------------------------------------------------------------------------
Driver helpers */

int main(int argc, char **argv) {
	printf("== library ==\n");
	test_split_identity();
	test_resume_state();
	test_resume_chain();
	test_invalid();
	printf("library: %d checks, %d failures\n", checks, failures);
	(void)argc;
	(void)argv;
	return failures ? 1 : 0;
}
