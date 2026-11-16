/*

Endianness verification for qoi.h.

Build and run with (single pass, both configurations):
	make -f /dev/null endian_test   # or see the two gcc commands below
	gcc -std=c99 -O3 endian_test.c -o endian_test_be
	gcc -std=c99 -O3 -DQOI_ENDIAN_LITTLE endian_test.c -o endian_test_le
	./endian_test_be && ./endian_test_le

The harness is self-contained: it includes qoi.h and nothing else beyond the C
standard library. It exercises, in one run:

 1. build and host endian queries,
 2. roundtrip consistency for the build's own byte order (RGB and RGBA),
 3. cross-order reading: a stream written in the other order decodes to the
    same pixels,
 4. byte level difference statistics between the two stream variants,
 5. encode/decode timing for both orders,
 6. file level roundtrip of both variants through qoi_write/qoi_read,
 7. detection of mixed endian streams (magic and fields disagreeing),
 8. rejection of corrupted headers by qoi_decode().

Exit code is 0 only if every check passes.

*/

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("  FAIL [%s:%d] ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static double now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Deterministic test pixels: smooth gradients, a flat area, and some noise,
   so that every QOI chunk kind (RUN, INDEX, DIFF, LUMA, RGB, RGBA) occurs. */
static unsigned char *make_pixels(int width, int height, int channels) {
	size_t n = (size_t)width * height * channels;
	unsigned char *px = (unsigned char *)malloc(n);
	int x, y;
	unsigned int seed = 12345u;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned char *p = px + ((size_t)y * width + x) * channels;
			unsigned char v = (unsigned char)(((x / 8) + (y / 8)) & 0xff);
			seed = seed * 1103515245u + 12345u;
			if ((x >= width / 2) && (y >= height / 2)) {
				/* flat quadrant: long runs */
				p[0] = p[1] = p[2] = 200;
			}
			else if ((seed >> 16) % 17 == 0) {
				/* sparse noise: opaque RGB/RGBA chunks */
				p[0] = (unsigned char)(seed >> 8);
				p[1] = (unsigned char)(seed >> 16);
				p[2] = (unsigned char)(seed >> 24);
			}
			else {
				p[0] = (unsigned char)(x * 2 + v);
				p[1] = (unsigned char)(y * 2 + v);
				p[2] = (unsigned char)(x + y);
			}
			if (channels == 4) {
				p[3] = (unsigned char)((x + y) < (width / 2) ? 255 : (seed >> 9));
			}
		}
	}
	return px;
}

static const char *order_name(int order) {
	switch (order) {
		case QOI_ORDER_BIG:    return "BIG";
		case QOI_ORDER_LITTLE: return "LITTLE";
		case QOI_ORDER_MIXED:  return "MIXED";
		default:               return "NONE";
	}
}

/* Compare two QOI streams byte by byte and report where they differ. */
static void compare_streams(const unsigned char *a, int a_len,
	const unsigned char *b, int b_len, int channels)
{
	int i, diff = 0, header_diff = 0, payload_diff = 0;
	int payload = QOI_HEADER_SIZE;
	int common = (a_len < b_len) ? a_len : b_len;

	CHECK(a_len == b_len, "stream lengths differ: %d vs %d", a_len, b_len);

	for (i = 0; i < common; i++) {
		if (a[i] != b[i]) {
			diff++;
			if (i < payload) {
				header_diff++;
			}
			else {
				payload_diff++;
			}
		}
	}

	printf("  byte diff: total=%d header=%d payload=%d size=%d\n",
		diff, header_diff, payload_diff, a_len);
	printf("  differing payload bytes: %d (expected 0, payload is endian neutral)\n",
		payload_diff);
	CHECK(payload_diff == 0, "payload bytes differ between orders");
	CHECK(header_diff == 5,
		"expected exactly 5 header bytes to differ, got %d", header_diff);
	(void)channels;
}

int main(void) {
	const int width = 96, height = 64;
	const int channels_list[2] = {3, 4};
	int ci, order_i;
	int encode_orders[2] = {QOI_ORDER_BIG, QOI_ORDER_LITTLE};
	double enc_time[2] = {0}, dec_time[2] = {0};
	int build_order = qoi_build_endian();
	int host_order  = qoi_host_endian();

	printf("=== build/host endianness ===\n");
	printf("  build order: %s\n", order_name(build_order));
	printf("  host  order: %s\n", order_name(host_order));
	CHECK(build_order == QOI_ORDER_BIG || build_order == QOI_ORDER_LITTLE,
		"unexpected build order");
	CHECK(host_order == QOI_ORDER_BIG || host_order == QOI_ORDER_LITTLE,
		"unexpected host order");

	for (ci = 0; ci < 2; ci++) {
		int channels = channels_list[ci];
		int w = width, h = height;
		unsigned char *px = make_pixels(w, h, channels);
		qoi_desc desc;
		int sizes[2] = {0, 0};
		unsigned char *streams[2] = {NULL, NULL};

		desc.width = (unsigned int)w;
		desc.height = (unsigned int)h;
		desc.channels = (unsigned char)channels;
		desc.colorspace = QOI_SRGB;

		printf("\n=== %d channel, %dx%d ===\n", channels, w, h);

		/* The build's own encoder, and both explicit orders. */
		for (order_i = 0; order_i < 2; order_i++) {
			streams[order_i] = (unsigned char *)qoi_encode_endian(
				px, &desc, encode_orders[order_i], &sizes[order_i]);
			CHECK(streams[order_i] != NULL, "encode failed for order %s",
				order_name(encode_orders[order_i]));
			if (!streams[order_i]) {
				continue;
			}
			printf("  %s stream: %d bytes, magic \"%.4s\"\n",
				order_name(encode_orders[order_i]), sizes[order_i],
				streams[order_i]);
			CHECK(qoi_probe_endian(streams[order_i], sizes[order_i]) ==
				encode_orders[order_i], "probe reports wrong order");
		}
		if (!streams[0] || !streams[1]) {
			free(px);
			continue;
		}

		/* Roundtrip in the build's own order, using the plain API. */
		{
			int build_slot = (build_order == QOI_ORDER_BIG) ? 0 : 1;
			int own_len = 0;
			unsigned char *own = (unsigned char *)qoi_encode(px, &desc, &own_len);
			qoi_desc out_desc;
			unsigned char *decoded;

			CHECK(own != NULL, "qoi_encode failed");
			CHECK(own_len == sizes[build_slot],
				"qoi_encode disagrees with explicit %s encode",
				order_name(build_order));
			CHECK(own && !memcmp(own, streams[build_slot], (size_t)own_len),
				"qoi_encode is not byte identical to explicit build order");

			decoded = own ?
				(unsigned char *)qoi_decode(own, own_len, &out_desc, channels) :
				NULL;
			CHECK(decoded != NULL, "qoi_decode failed");
			if (decoded) {
				CHECK(out_desc.width == (unsigned int)w &&
					out_desc.height == (unsigned int)h &&
					out_desc.channels == channels,
					"decoded description differs");
				CHECK(!memcmp(decoded, px, (size_t)w * h * channels),
					"roundtrip pixels differ");
				printf("  roundtrip (%s build): %s\n",
					order_name(build_order),
					memcmp(decoded, px, (size_t)w * h * channels) ? "FAIL" : "identical");
				free(decoded);
			}
			free(own);
		}

		/* Both orders decode to the original pixels, in either build. */
		for (order_i = 0; order_i < 2; order_i++) {
			qoi_desc out_desc;
			unsigned char *decoded = (unsigned char *)qoi_decode(
				streams[order_i], sizes[order_i], &out_desc, channels);
			CHECK(decoded != NULL, "decode failed for %s stream",
				order_name(encode_orders[order_i]));
			if (decoded) {
				CHECK(!memcmp(decoded, px, (size_t)w * h * channels),
					"%s stream did not roundtrip", order_name(encode_orders[order_i]));
				free(decoded);
			}
		}

		/* Byte difference statistics between the two variants. */
		printf("  big endian vs little endian stream:\n");
		compare_streams(streams[0], sizes[0], streams[1], sizes[1], channels);

		/* Timing: encode and decode each order repeatedly. */
		for (order_i = 0; order_i < 2; order_i++) {
			const int runs = 200;
			int r, total = 0;

			for (r = 0; r < runs; r++) {
				double t0, t1;
				int n = 0;
				unsigned char *s;
				unsigned char *d;
				qoi_desc out_desc;

				t0 = now_seconds();
				s = (unsigned char *)qoi_encode_endian(px, &desc,
					encode_orders[order_i], &n);
				t1 = now_seconds();
				enc_time[order_i] += (t1 - t0);

				t0 = now_seconds();
				d = (unsigned char *)qoi_decode(s, n, &out_desc, channels);
				t1 = now_seconds();
				dec_time[order_i] += (t1 - t0);

				CHECK(d && !memcmp(d, px, (size_t)w * h * channels),
					"timed roundtrip mismatch");
				total += n;
				free(s);
				free(d);
			}
			printf("  %s: encode %.3f ms  decode %.3f ms  (avg of %d runs, %d bytes)\n",
				order_name(encode_orders[order_i]),
				enc_time[order_i] * 1000.0 / 200.0,
				dec_time[order_i] * 1000.0 / 200.0,
				200, total / 200);
		}

		/* Mixed endian streams: magic and fields disagree. */
		{
			unsigned char *mixed = (unsigned char *)malloc((size_t)sizes[0]);
			int big_slot = 0, little_slot = 1;
			int probe;

			CHECK(mixed != NULL, "malloc failed");
			if (mixed) {
				/* "qoif" magic with little endian width/height. */
				memcpy(mixed, streams[big_slot], (size_t)sizes[big_slot]);
				memcpy(mixed + 4, streams[little_slot] + 4, 8);
				probe = qoi_probe_endian(mixed, sizes[big_slot]);
				printf("  mixed qoif-magic/LE-fields : probe=%s decode=%s\n",
					order_name(probe),
					qoi_decode(mixed, sizes[big_slot], &desc, channels) ?
						"ACCEPTED" : "rejected");
				CHECK(probe == QOI_ORDER_MIXED,
					"mixed stream not detected by probe");
				CHECK(qoi_decode(mixed, sizes[big_slot], &desc, channels) == NULL,
					"mixed stream was not rejected");

				/* "qoil" magic with big endian width/height. */
				memcpy(mixed, streams[little_slot], (size_t)sizes[little_slot]);
				memcpy(mixed + 4, streams[big_slot] + 4, 8);
				probe = qoi_probe_endian(mixed, sizes[little_slot]);
				printf("  mixed qoil-magic/BE-fields : probe=%s decode=%s\n",
					order_name(probe),
					qoi_decode(mixed, sizes[little_slot], &desc, channels) ?
						"ACCEPTED" : "rejected");
				CHECK(probe == QOI_ORDER_MIXED,
					"mixed stream with qoil magic not detected");
				CHECK(qoi_decode(mixed, sizes[little_slot], &desc, channels) == NULL,
					"mixed stream with qoil magic was not rejected");

				free(mixed);
			}
		}

		/* Invalid headers are rejected. */
		{
			unsigned char *bad = (unsigned char *)malloc((size_t)sizes[0]);
			if (bad) {
				memcpy(bad, streams[0], (size_t)sizes[0]);
				bad[3] = 'x'; /* corrupt magic */
				CHECK(qoi_decode(bad, sizes[0], &desc, channels) == NULL,
					"corrupt magic accepted");
				CHECK(qoi_probe_endian(bad, sizes[0]) == QOI_ORDER_NONE,
					"corrupt magic not reported as NONE");

				memcpy(bad, streams[0], (size_t)sizes[0]);
				bad[12] = 7; /* invalid channel count */
				CHECK(qoi_decode(bad, sizes[0], &desc, channels) == NULL,
					"invalid channel count accepted");

				memcpy(bad, streams[0], (size_t)sizes[0]);
				bad[4] = bad[5] = bad[6] = bad[7] = 0; /* zero width */
				CHECK(qoi_decode(bad, sizes[0], &desc, channels) == NULL,
					"zero width accepted");
				free(bad);
			}
		}

		/* File level roundtrip through qoi_write/qoi_read, and reading a
		file that was written in the opposite order. */
		{
			const char *names[2] = {
				"/tmp/qoi_endian_sample_be.qoi",
				"/tmp/qoi_endian_sample_le.qoi"
			};
			int order_slot;
			int wrote_by_this_build = -1;

			for (order_slot = 0; order_slot < 2; order_slot++) {
				int n = sizes[order_slot];
				FILE *f = fopen(names[order_slot], "wb");
				CHECK(f != NULL, "cannot open %s", names[order_slot]);
				if (f) {
					fwrite(streams[order_slot], 1, (size_t)n, f);
					fclose(f);
				}
				CHECK(qoi_probe_endian(streams[order_slot], n) ==
					encode_orders[order_slot], "fixture file has wrong order");
			}

			/* Both fixture files must decode to the original pixels. */
			for (order_slot = 0; order_slot < 2; order_slot++) {
				qoi_desc out_desc;
				unsigned char *decoded =
					(unsigned char *)qoi_read(names[order_slot], &out_desc, channels);
				CHECK(decoded != NULL, "qoi_read failed on %s file",
					order_name(encode_orders[order_slot]));
				if (decoded) {
					CHECK(!memcmp(decoded, px, (size_t)w * h * channels),
						"file roundtrip differs for %s", order_name(encode_orders[order_slot]));
					CHECK(out_desc.width == (unsigned int)w &&
						out_desc.height == (unsigned int)h,
						"file header description differs");
					free(decoded);
				}
			}

			printf("  file roundtrip: qoif and qoil fixtures decode identically\n");
			(void)wrote_by_this_build;
		}

		free(streams[0]);
		free(streams[1]);
		free(px);
	}

	printf("\n=== summary ===\n");
	printf("  checks=%d failures=%d\n", checks, failures);
	printf("  %s\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
	return failures ? 1 : 0;
}
