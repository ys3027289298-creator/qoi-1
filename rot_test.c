/*

Self-contained verification for the pixel rotation support added to qoi.h.
No third-party dependencies: it only includes qoi.h.

Compile with:
	gcc -std=c99 -O2 rot_test.c -o /tmp/rot_test

*/

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf("  FAIL: %s\n", msg); \
		failures++; \
	} \
} while (0)

static double now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void make_pixels(unsigned char *px, int width, int height, int channels) {
	int x, y, c;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned char *p = px + (y * width + x) * channels;
			p[0] = (unsigned char)((x * 37 + y * 11) & 0xff);
			p[1] = (unsigned char)((x * 7 + y * 53) & 0xff);
			p[2] = (unsigned char)((x * 91 + y * 5 + 17) & 0xff);
			if (channels == 4) {
				p[3] = (unsigned char)(255 - ((x + y) * 13 & 0xff));
			}
			for (c = 0; c < channels; c++) {
				(void)c;
			}
		}
	}
}

static unsigned int qoi_stream_width(const unsigned char *data) {
	return ((unsigned int)data[4] << 24) | ((unsigned int)data[5] << 16) |
		((unsigned int)data[6] << 8) | (unsigned int)data[7];
}

static unsigned int qoi_stream_height(const unsigned char *data) {
	return ((unsigned int)data[8] << 24) | ((unsigned int)data[9] << 16) |
		((unsigned int)data[10] << 8) | (unsigned int)data[11];
}

int main(void) {
	const int width = 7, height = 5, channels = 4;
	const int px_count = width * height * channels;
	const int angles[3] = {90, 180, 270};
	int expected_w[3] = {height, width, height};
	int expected_h[3] = {width, height, width};
	unsigned char *original = malloc(px_count);
	qoi_desc base_desc;
	int base_len, a, i, iter;

	make_pixels(original, width, height, channels);
	base_desc = (qoi_desc){
		.width = width, .height = height,
		.channels = channels, .colorspace = QOI_SRGB
	};

	/* Reference encoding through the untouched code path. */
	unsigned char *base_encoded = qoi_encode(original, &base_desc, &base_len);
	CHECK(base_encoded != NULL, "baseline qoi_encode");

	printf("== rotation tests on a %dx%d RGBA sample ==\n\n", width, height);

	for (a = 0; a < 3; a++) {
		int angle = angles[a];
		qoi_desc rot_desc = base_desc;
		qoi_desc out_desc;
		int rot_len;
		unsigned char *rot_encoded, *upright;

		rot_encoded = qoi_encode_rotated(original, &rot_desc, angle, &rot_len);
		CHECK(rot_encoded != NULL, "qoi_encode_rotated alloc");

		printf("angle %3d: encoded stream %ux%u (input %dx%d), "
			"size %d bytes (baseline %d, delta %+d)\n",
			angle, qoi_stream_width(rot_encoded),
			qoi_stream_height(rot_encoded), width, height,
			rot_len, base_len, rot_len - base_len);
		CHECK((int)qoi_stream_width(rot_encoded) == expected_w[a],
			"swapped/stored width");
		CHECK((int)qoi_stream_height(rot_encoded) == expected_h[a],
			"swapped/stored height");

		/* Rotation must be physical: plain decode sees the rotated pixels. */
		{
			qoi_desc plain_desc;
			unsigned char *plain = qoi_decode(rot_encoded, rot_len,
				&plain_desc, channels);
			CHECK(plain != NULL, "plain decode of rotated stream");
			CHECK((int)plain_desc.width == expected_w[a] &&
				(int)plain_desc.height == expected_h[a],
				"plain decode reports rotated dimensions");
			CHECK(memcmp(plain, original, px_count) != 0,
				"stream pixels are physically rearranged, not metadata");
			free(plain);
		}

		/* Rotating back must hand the caller an upright, identical image. */
		memset(&out_desc, 0, sizeof(out_desc));
		upright = qoi_decode_rotated(rot_encoded, rot_len, &out_desc,
			channels, angle);
		CHECK(upright != NULL, "qoi_decode_rotated alloc");
		CHECK((int)out_desc.width == width && (int)out_desc.height == height,
			"decoded desc restored to original dimensions");
		CHECK(memcmp(upright, original, px_count) == 0,
			"round trip restores every pixel byte-for-byte");

		/* Applying the same rotation four times is the identity. */
		{
			unsigned char *cur = malloc(px_count);
			memcpy(cur, original, px_count);
			for (i = 0; i < 4; i++) {
				qoi_desc step_desc = base_desc;
				qoi_desc step_out;
				int step_len;
				unsigned char *step_enc, *step_dec;
				step_enc = qoi_encode_rotated(cur, &step_desc, angle,
					&step_len);
				CHECK(step_enc != NULL, "identity loop encode");
				step_dec = qoi_decode_rotated(step_enc, step_len, &step_out,
					channels, angle);
				CHECK(step_dec != NULL, "identity loop decode");
				free(step_enc);
				free(cur);
				cur = step_dec;
			}
			CHECK(memcmp(cur, original, px_count) == 0,
				"four rotations return to the original pixels");
			free(cur);
		}

		free(rot_encoded);
		free(upright);
		printf("\n");
	}

	/* Illegal angles must be rejected on both entry points. */
	printf("== invalid angle handling ==\n");
	{
	int bad_angles[4] = {45, 360, -90, 91};
	for (i = 0; i < 4; i++) {
		int bad_len;
		qoi_desc bad_desc = base_desc;
		CHECK(qoi_encode_rotated(original, &bad_desc, bad_angles[i],
			&bad_len) == NULL, "encode rejects illegal angle");
		CHECK(qoi_decode_rotated(base_encoded, base_len, &bad_desc,
			channels, bad_angles[i]) == NULL,
			"decode rejects illegal angle");
	}
		printf("  angles 45, 360, -90, 91 all rejected\n\n");
	}

	/* Angle 0 must keep the original code paths byte-for-byte identical. */
	printf("== unchanged original path ==\n");
	{
		int zero_len;
		qoi_desc zero_desc = base_desc;
		unsigned char *zero_enc = qoi_encode_rotated(original, &zero_desc,
			QOI_ROTATE_0, &zero_len);
		CHECK(zero_len == base_len &&
			memcmp(zero_enc, base_encoded, base_len) == 0,
			"qoi_encode_rotated(0) == qoi_encode output, byte for byte");

		{
			qoi_desc d1, d2;
			unsigned char *p1 = qoi_decode(base_encoded, base_len, &d1,
				channels);
			unsigned char *p2 = qoi_decode_rotated(base_encoded, base_len,
				&d2, channels, QOI_ROTATE_0);
			CHECK(memcmp(p1, p2, px_count) == 0 &&
				d1.width == d2.width && d1.height == d2.height,
				"qoi_decode_rotated(0) == qoi_decode output, byte for byte");
			free(p1);
			free(p2);
		}
		free(zero_enc);
	}

	/* RGB (3 channels) must be honored as well. */
	{
		const int rgb_count = width * height * 3;
		unsigned char *rgb = malloc(rgb_count);
		qoi_desc rgb_desc = {
			.width = width, .height = height,
			.channels = 3, .colorspace = QOI_SRGB
		};
		int rgb_len;
		qoi_desc rgb_out;
		unsigned char *rgb_enc, *rgb_back;
		make_pixels(rgb, width, height, 3);
		rgb_enc = qoi_encode_rotated(rgb, &rgb_desc, 90, &rgb_len);
		CHECK(rgb_enc != NULL, "RGB encode rotated");
		rgb_back = qoi_decode_rotated(rgb_enc, rgb_len, &rgb_out, 3, 90);
		CHECK(rgb_back != NULL && (int)rgb_out.width == width &&
			(int)rgb_out.height == height &&
			memcmp(rgb_back, rgb, rgb_count) == 0,
			"RGB round trip restores pixels and dimensions");
		free(rgb_enc);
		free(rgb_back);
		free(rgb);
	}

	/* Encode timing on 512x512 RGBA for each angle. */
	printf("\n== encode timing on 512x512 RGBA (500 iterations each) ==\n");
	{
		const int big = 512;
		const int iters = 500;
		unsigned char *big_px = malloc((size_t)big * big * 4);
		qoi_desc big_desc = {
			.width = big, .height = big,
			.channels = 4, .colorspace = QOI_SRGB
		};
		make_pixels(big_px, big, big, 4);
		for (a = 0; a < 3; a++) {
			int angle = angles[a], tmp_len;
			double t0, t1;
			for (iter = 0; iter < 20; iter++) {
				unsigned char *warm = qoi_encode_rotated(big_px, &big_desc,
					angle, &tmp_len);
				free(warm);
			}
			t0 = now_seconds();
			for (iter = 0; iter < iters; iter++) {
				unsigned char *enc = qoi_encode_rotated(big_px, &big_desc,
					angle, &tmp_len);
				free(enc);
			}
			t1 = now_seconds();
			printf("angle %3d: %.3f ms/call (%.0f calls/s)\n", angle,
				(t1 - t0) * 1000.0 / iters,
				iters / (t1 - t0));
		}
		free(big_px);
	}

	free(base_encoded);
	free(original);

	printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED");
	return failures ? 1 : 0;
}
