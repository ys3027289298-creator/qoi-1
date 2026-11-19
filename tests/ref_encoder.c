/* Reference for the driver tests: encode a whole raw image with the original
qoi_encode() and write the result. This is what "byte-for-byte identical"
means in tests/qoi_stream_driver.sh. */

#include <stdio.h>
#include <stdlib.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

int main(int argc, char **argv) {
	FILE *f;
	long n;
	unsigned char *px;
	void *q;
	int qn = 0, w, h, ch;
	qoi_desc desc;

	if (argc != 6) {
		fprintf(stderr, "usage: ref_encoder <raw> <w> <h> <channels> <out.qoi>\n");
		return 1;
	}
	w = atoi(argv[2]);
	h = atoi(argv[3]);
	ch = atoi(argv[4]);

	f = fopen(argv[1], "rb");
	if (!f) {
		perror("open");
		return 1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	px = (unsigned char *)malloc((size_t)n);
	if (!px || fread(px, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	fclose(f);

	desc.width = (unsigned)w;
	desc.height = (unsigned)h;
	desc.channels = (unsigned char)ch;
	desc.colorspace = QOI_SRGB;
	q = qoi_encode(px, &desc, &qn);
	if (!q) {
		fprintf(stderr, "qoi_encode failed\n");
		return 1;
	}
	f = fopen(argv[5], "wb");
	if (!f || fwrite(q, 1, (size_t)qn, f) != (size_t)qn) {
		fprintf(stderr, "cannot write %s\n", argv[5]);
		return 1;
	}
	fclose(f);
	free(q);
	free(px);
	return 0;
}
