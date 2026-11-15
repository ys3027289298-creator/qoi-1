/*

Test helper: encode the two synthetic sample images with qoi_encode and
write them to <outdir>/a.qoi and <outdir>/b.qoi. Compiled against the
original and the modified qoi.h to prove the original encode path is
byte-identical.

*/

#define QOI_IMPLEMENTATION
#include "qoi.h"
#include <stdio.h>

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

int main(int argc, char **argv) {
	static unsigned char img_a[64 * 64 * 3];
	static unsigned char img_b[48 * 48 * 4];
	char path[4096];

	if (argc < 2) {
		fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
		return 1;
	}

	gen_a(img_a, 64, 64);
	gen_b(img_b, 48, 48);

	snprintf(path, sizeof(path), "%s/a.qoi", argv[1]);
	if (!qoi_write(path, img_a, &(qoi_desc){64, 64, 3, QOI_SRGB})) {
		fprintf(stderr, "encode a failed\n");
		return 1;
	}

	snprintf(path, sizeof(path), "%s/b.qoi", argv[1]);
	if (!qoi_write(path, img_b, &(qoi_desc){48, 48, 4, QOI_LINEAR})) {
		fprintf(stderr, "encode b failed\n");
		return 1;
	}

	return 0;
}
