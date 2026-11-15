/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT


QOI - The "Quite OK Image" format for fast, lossless image compression

-- About

QOI encodes and decodes images in a lossless format. Compared to stb_image and
stb_image_write QOI offers 20x-50x faster encoding, 3x-4x faster decoding and
20% better compression.


-- Synopsis

// Define `QOI_IMPLEMENTATION` in *one* C/C++ file before including this
// library to create the implementation.

#define QOI_IMPLEMENTATION
#include "qoi.h"

// Encode and store an RGBA buffer to the file system. The qoi_desc describes
// the input pixel data.
qoi_write("image_new.qoi", rgba_pixels, &(qoi_desc){
	.width = 1920,
	.height = 1080,
	.channels = 4,
	.colorspace = QOI_SRGB
});

// Load and decode a QOI image from the file system into a 32bbp RGBA buffer.
// The qoi_desc struct will be filled with the width, height, number of channels
// and colorspace read from the file header.
qoi_desc desc;
void *rgba_pixels = qoi_read("image.qoi", &desc, 4);



-- Documentation

This library provides the following functions;
- qoi_read    -- read and decode a QOI file
- qoi_decode  -- decode the raw bytes of a QOI image from memory
- qoi_write   -- encode and write a QOI file
- qoi_encode  -- encode an rgba buffer into a QOI image in memory

See the function declaration below for the signature and more information.

If you don't want/need the qoi_read and qoi_write functions, you can define
QOI_NO_STDIO before including this library.

This library uses malloc() and free(). To supply your own malloc implementation
you can define QOI_MALLOC and QOI_FREE before including this library.

This library uses memset() to zero-initialize the index. To supply your own
implementation you can define QOI_ZEROARR before including this library.


-- Data Format

A QOI file has a 14 byte header, followed by any number of data "chunks" and an
8-byte end marker.

struct qoi_header_t {
	char     magic[4];   // magic bytes "qoif"
	uint32_t width;      // image width in pixels (BE)
	uint32_t height;     // image height in pixels (BE)
	uint8_t  channels;   // 3 = RGB, 4 = RGBA
	uint8_t  colorspace; // 0 = sRGB with linear alpha, 1 = all channels linear
};

Images are encoded row by row, left to right, top to bottom. The decoder and
encoder start with {r: 0, g: 0, b: 0, a: 255} as the previous pixel value. An
image is complete when all pixels specified by width * height have been covered.

Pixels are encoded as
 - a run of the previous pixel
 - an index into an array of previously seen pixels
 - a difference to the previous pixel value in r,g,b
 - full r,g,b or r,g,b,a values

The color channels are assumed to not be premultiplied with the alpha channel
("un-premultiplied alpha").

A running array[64] (zero-initialized) of previously seen pixel values is
maintained by the encoder and decoder. Each pixel that is seen by the encoder
and decoder is put into this array at the position formed by a hash function of
the color value. In the encoder, if the pixel value at the index matches the
current pixel, this index position is written to the stream as QOI_OP_INDEX.
The hash function for the index is:

	index_position = (r * 3 + g * 5 + b * 7 + a * 11) % 64

Each chunk starts with a 2- or 8-bit tag, followed by a number of data bits. The
bit length of chunks is divisible by 8 - i.e. all chunks are byte aligned. All
values encoded in these data bits have the most significant bit on the left.

The 8-bit tags have precedence over the 2-bit tags. A decoder must check for the
presence of an 8-bit tag first.

The byte stream's end is marked with 7 0x00 bytes followed a single 0x01 byte.


The possible chunks are:


.- QOI_OP_INDEX ----------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----------------|
|  0  0 |     index       |
`-------------------------`
2-bit tag b00
6-bit index into the color index array: 0..63

A valid encoder must not issue 2 or more consecutive QOI_OP_INDEX chunks to the
same index. QOI_OP_RUN should be used instead.


.- QOI_OP_DIFF -----------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----+-----+-----|
|  0  1 |  dr |  dg |  db |
`-------------------------`
2-bit tag b01
2-bit   red channel difference from the previous pixel between -2..1
2-bit green channel difference from the previous pixel between -2..1
2-bit  blue channel difference from the previous pixel between -2..1

The difference to the current channel values are using a wraparound operation,
so "1 - 2" will result in 255, while "255 + 1" will result in 0.

Values are stored as unsigned integers with a bias of 2. E.g. -2 is stored as
0 (b00). 1 is stored as 3 (b11).

The alpha value remains unchanged from the previous pixel.


.- QOI_OP_LUMA -------------------------------------.
|         Byte[0]         |         Byte[1]         |
|  7  6  5  4  3  2  1  0 |  7  6  5  4  3  2  1  0 |
|-------+-----------------+-------------+-----------|
|  1  0 |  green diff     |   dr - dg   |  db - dg  |
`---------------------------------------------------`
2-bit tag b10
6-bit green channel difference from the previous pixel -32..31
4-bit   red channel difference minus green channel difference -8..7
4-bit  blue channel difference minus green channel difference -8..7

The green channel is used to indicate the general direction of change and is
encoded in 6 bits. The red and blue channels (dr and db) base their diffs off
of the green channel difference and are encoded in 4 bits. I.e.:
	dr_dg = (cur_px.r - prev_px.r) - (cur_px.g - prev_px.g)
	db_dg = (cur_px.b - prev_px.b) - (cur_px.g - prev_px.g)

The difference to the current channel values are using a wraparound operation,
so "10 - 13" will result in 253, while "250 + 7" will result in 1.

Values are stored as unsigned integers with a bias of 32 for the green channel
and a bias of 8 for the red and blue channel.

The alpha value remains unchanged from the previous pixel.


.- QOI_OP_RUN ------------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----------------|
|  1  1 |       run       |
`-------------------------`
2-bit tag b11
6-bit run-length repeating the previous pixel: 1..62

The run-length is stored with a bias of -1. Note that the run-lengths 63 and 64
(b111110 and b111111) are illegal as they are occupied by the QOI_OP_RGB and
QOI_OP_RGBA tags.


.- QOI_OP_RGB ------------------------------------------.
|         Byte[0]         | Byte[1] | Byte[2] | Byte[3] |
|  7  6  5  4  3  2  1  0 | 7 .. 0  | 7 .. 0  | 7 .. 0  |
|-------------------------+---------+---------+---------|
|  1  1  1  1  1  1  1  0 |   red   |  green  |  blue   |
`-------------------------------------------------------`
8-bit tag b11111110
8-bit   red channel value
8-bit green channel value
8-bit  blue channel value

The alpha value remains unchanged from the previous pixel.


.- QOI_OP_RGBA ---------------------------------------------------.
|         Byte[0]         | Byte[1] | Byte[2] | Byte[3] | Byte[4] |
|  7  6  5  4  3  2  1  0 | 7 .. 0  | 7 .. 0  | 7 .. 0  | 7 .. 0  |
|-------------------------+---------+---------+---------+---------|
|  1  1  1  1  1  1  1  1 |   red   |  green  |  blue   |  alpha  |
`-----------------------------------------------------------------`
8-bit tag b11111111
8-bit   red channel value
8-bit green channel value
8-bit  blue channel value
8-bit alpha channel value

*/


/* -----------------------------------------------------------------------------
Header - Public functions */

#ifndef QOI_H
#define QOI_H

#ifdef __cplusplus
extern "C" {
#endif

/* A pointer to a qoi_desc struct has to be supplied to all of qoi's functions.
It describes either the input format (for qoi_write and qoi_encode), or is
filled with the description read from the file header (for qoi_read and
qoi_decode).

The colorspace in this qoi_desc is an enum where
	0 = sRGB, i.e. gamma scaled RGB channels and a linear alpha channel
	1 = all channels are linear
You may use the constants QOI_SRGB or QOI_LINEAR. The colorspace is purely
informative. It will be saved to the file header, but does not affect
how chunks are en-/decoded. */

#define QOI_SRGB   0
#define QOI_LINEAR 1

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned char channels;
	unsigned char colorspace;
} qoi_desc;

#ifndef QOI_NO_STDIO

/* Encode raw RGB or RGBA pixels into a QOI image and write it to the file
system. The qoi_desc struct must be filled with the image width, height,
number of channels (3 = RGB, 4 = RGBA) and the colorspace.

The function returns 0 on failure (invalid parameters, or fopen or malloc
failed) or the number of bytes written on success. */

int qoi_write(const char *filename, const void *data, const qoi_desc *desc);


/* Read and decode a QOI image from the file system. If channels is 0, the
number of channels from the file header is used. If channels is 3 or 4 the
output format will be forced into this number of channels.

The function either returns NULL on failure (invalid data, or malloc or fopen
failed) or a pointer to the decoded pixels. On success, the qoi_desc struct
will be filled with the description from the file header.

The returned pixel data should be free()d after use. */

void *qoi_read(const char *filename, qoi_desc *desc, int channels);

#endif /* QOI_NO_STDIO */


/* Encode raw RGB or RGBA pixels into a QOI image in memory.

The function either returns NULL on failure (invalid parameters or malloc
failed) or a pointer to the encoded data on success. On success the out_len
is set to the size in bytes of the encoded data.

The returned qoi data should be free()d after use. */

void *qoi_encode(const void *data, const qoi_desc *desc, int *out_len);


/* Decode a QOI image from memory.

The function either returns NULL on failure (invalid parameters or malloc
failed) or a pointer to the decoded pixels. On success, the qoi_desc struct
is filled with the description from the file header.

The returned pixel data should be free()d after use. */

void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels);


/* -----------------------------------------------------------------------------
Indexed color pipeline: median-cut palette quantization, index mapping and
two lossless encodings for the index array (run-length and optional delta).
These functions are independent of the QOI en-/decoder above and do not
change its behavior in any way. */

#define QOI_PALETTE_MAX 256

typedef struct {
	unsigned char r, g, b, a;
} qoi_palette_color_t;

/* Build a palette of num_colors colors from the given pixels using
median-cut quantization. channels must be 3 (RGB) or 4 (RGBA), num_colors
must be in the range 2..QOI_PALETTE_MAX.

The function returns the number of palette entries written to palette_out
(which must hold at least num_colors entries) or 0 on invalid parameters.
Fewer than num_colors entries may be returned if the image contains fewer
distinct colors. */

int qoi_palette_build(const void *pixels, int width, int height, int channels,
	int num_colors, qoi_palette_color_t *palette_out);

/* Map every pixel to the nearest palette entry (least squared error over
all channels). indices_out must hold width*height bytes.

The function returns 0 on success, -1 on invalid parameters. */

int qoi_palette_map(const void *pixels, int width, int height, int channels,
	const qoi_palette_color_t *palette, int num_colors,
	unsigned char *indices_out);

/* Reconstruct pixels from an index array and a palette. pixels_out must
hold count*channels bytes.

The function returns 0 on success, -1 on invalid parameters or if an index
is out of range. */

int qoi_palette_apply(const unsigned char *indices, int count,
	const qoi_palette_color_t *palette, int num_colors, int channels,
	void *pixels_out);

/* Encode an index array with run-length encoding. A header byte < 0x80
introduces a literal block of (header+1) indices, a header byte >= 0x80 a
run of (header-0x7f) copies of the single index that follows it.

The function returns the encoded size in bytes or -1 on invalid parameters
or if out_cap is too small. */

int qoi_index_encode_rle(const unsigned char *indices, int count,
	unsigned char *out, int out_cap);

/* Encode an index array, optionally with delta coding. With use_delta == 0
the indices are copied verbatim. With use_delta != 0 the first index is
stored verbatim and every following index as a zig-zag encoded varint of
its difference to the previous index.

The function returns the encoded size in bytes or -1 on invalid parameters
or if out_cap is too small. */

int qoi_index_encode_delta(const unsigned char *indices, int count,
	int use_delta, unsigned char *out, int out_cap);

/* Decode data produced by qoi_index_encode_rle() back into exactly count
indices. The function returns 0 on success, -1 on malformed input. */

int qoi_index_decode_rle(const unsigned char *in, int in_len,
	unsigned char *indices_out, int count);

/* Decode data produced by qoi_index_encode_delta() back into exactly count
indices. use_delta must match the value used for encoding. The function
returns 0 on success, -1 on malformed input. */

int qoi_index_decode_delta(const unsigned char *in, int in_len,
	int use_delta, unsigned char *indices_out, int count);


#ifdef __cplusplus
}
#endif
#endif /* QOI_H */


/* -----------------------------------------------------------------------------
Implementation */

#ifdef QOI_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>

#ifndef QOI_MALLOC
	#define QOI_MALLOC(sz) malloc(sz)
	#define QOI_FREE(p)    free(p)
#endif
#ifndef QOI_ZEROARR
	#define QOI_ZEROARR(a) memset((a),0,sizeof(a))
#endif

#define QOI_OP_INDEX  0x00 /* 00xxxxxx */
#define QOI_OP_DIFF   0x40 /* 01xxxxxx */
#define QOI_OP_LUMA   0x80 /* 10xxxxxx */
#define QOI_OP_RUN    0xc0 /* 11xxxxxx */
#define QOI_OP_RGB    0xfe /* 11111110 */
#define QOI_OP_RGBA   0xff /* 11111111 */

#define QOI_MASK_2    0xc0 /* 11000000 */

#define QOI_COLOR_HASH(C) (C.rgba.r*3 + C.rgba.g*5 + C.rgba.b*7 + C.rgba.a*11)
#define QOI_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'f'))
#define QOI_HEADER_SIZE 14

/* 2GB is the max file size that this implementation can safely handle. We guard
against anything larger than that, assuming the worst case with 5 bytes per
pixel, rounded down to a nice clean value. 400 million pixels ought to be
enough for anybody. */
#define QOI_PIXELS_MAX ((unsigned int)400000000)

typedef union {
	struct { unsigned char r, g, b, a; } rgba;
	unsigned int v;
} qoi_rgba_t;

static const unsigned char qoi_padding[8] = {0,0,0,0,0,0,0,1};

static void qoi_write_32(unsigned char *bytes, int *p, unsigned int v) {
	bytes[(*p)++] = (0xff000000 & v) >> 24;
	bytes[(*p)++] = (0x00ff0000 & v) >> 16;
	bytes[(*p)++] = (0x0000ff00 & v) >> 8;
	bytes[(*p)++] = (0x000000ff & v);
}

static unsigned int qoi_read_32(const unsigned char *bytes, int *p) {
	unsigned int a = bytes[(*p)++];
	unsigned int b = bytes[(*p)++];
	unsigned int c = bytes[(*p)++];
	unsigned int d = bytes[(*p)++];
	return a << 24 | b << 16 | c << 8 | d;
}

void *qoi_encode(const void *data, const qoi_desc *desc, int *out_len) {
	int i, max_size, p, run;
	int px_len, px_end, px_pos, channels;
	unsigned char *bytes;
	const unsigned char *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px, px_prev;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	max_size =
		desc->width * desc->height * (desc->channels + 1) +
		QOI_HEADER_SIZE + sizeof(qoi_padding);

	p = 0;
	bytes = (unsigned char *) QOI_MALLOC(max_size);
	if (!bytes) {
		return NULL;
	}

	qoi_write_32(bytes, &p, QOI_MAGIC);
	qoi_write_32(bytes, &p, desc->width);
	qoi_write_32(bytes, &p, desc->height);
	bytes[p++] = desc->channels;
	bytes[p++] = desc->colorspace;


	pixels = (const unsigned char *)data;

	QOI_ZEROARR(index);

	run = 0;
	px_prev.rgba.r = 0;
	px_prev.rgba.g = 0;
	px_prev.rgba.b = 0;
	px_prev.rgba.a = 255;
	px = px_prev;

	px_len = desc->width * desc->height * desc->channels;
	px_end = px_len - desc->channels;
	channels = desc->channels;

	for (px_pos = 0; px_pos < px_len; px_pos += channels) {
		px.rgba.r = pixels[px_pos + 0];
		px.rgba.g = pixels[px_pos + 1];
		px.rgba.b = pixels[px_pos + 2];

		if (channels == 4) {
			px.rgba.a = pixels[px_pos + 3];
		}

		if (px.v == px_prev.v) {
			run++;
			if (run == 62 || px_pos == px_end) {
				bytes[p++] = QOI_OP_RUN | (run - 1);
				run = 0;
			}
		}
		else {
			int index_pos;

			if (run > 0) {
				bytes[p++] = QOI_OP_RUN | (run - 1);
				run = 0;
			}

			index_pos = QOI_COLOR_HASH(px) & (64 - 1);

			if (index[index_pos].v == px.v) {
				bytes[p++] = QOI_OP_INDEX | index_pos;
			}
			else {
				index[index_pos] = px;

				if (px.rgba.a == px_prev.rgba.a) {
					signed char vr = px.rgba.r - px_prev.rgba.r;
					signed char vg = px.rgba.g - px_prev.rgba.g;
					signed char vb = px.rgba.b - px_prev.rgba.b;

					signed char vg_r = vr - vg;
					signed char vg_b = vb - vg;

					if (
						vr > -3 && vr < 2 &&
						vg > -3 && vg < 2 &&
						vb > -3 && vb < 2
					) {
						bytes[p++] = QOI_OP_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2);
					}
					else if (
						vg_r >  -9 && vg_r <  8 &&
						vg   > -33 && vg   < 32 &&
						vg_b >  -9 && vg_b <  8
					) {
						bytes[p++] = QOI_OP_LUMA     | (vg   + 32);
						bytes[p++] = (vg_r + 8) << 4 | (vg_b +  8);
					}
					else {
						bytes[p++] = QOI_OP_RGB;
						bytes[p++] = px.rgba.r;
						bytes[p++] = px.rgba.g;
						bytes[p++] = px.rgba.b;
					}
				}
				else {
					bytes[p++] = QOI_OP_RGBA;
					bytes[p++] = px.rgba.r;
					bytes[p++] = px.rgba.g;
					bytes[p++] = px.rgba.b;
					bytes[p++] = px.rgba.a;
				}
			}
		}
		px_prev = px;
	}

	for (i = 0; i < (int)sizeof(qoi_padding); i++) {
		bytes[p++] = qoi_padding[i];
	}

	*out_len = p;
	return bytes;
}

void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels) {
	const unsigned char *bytes;
	unsigned int header_magic;
	unsigned char *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px;
	int px_len, chunks_len, px_pos;
	int p = 0, run = 0;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QOI_HEADER_SIZE + (int)sizeof(qoi_padding)
	) {
		return NULL;
	}

	bytes = (const unsigned char *)data;

	header_magic = qoi_read_32(bytes, &p);
	desc->width = qoi_read_32(bytes, &p);
	desc->height = qoi_read_32(bytes, &p);
	desc->channels = bytes[p++];
	desc->colorspace = bytes[p++];

	if (
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		header_magic != QOI_MAGIC ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	if (channels == 0) {
		channels = desc->channels;
	}

	px_len = desc->width * desc->height * channels;
	pixels = (unsigned char *) QOI_MALLOC(px_len);
	if (!pixels) {
		return NULL;
	}

	QOI_ZEROARR(index);
	px.rgba.r = 0;
	px.rgba.g = 0;
	px.rgba.b = 0;
	px.rgba.a = 255;

	chunks_len = size - (int)sizeof(qoi_padding);
	for (px_pos = 0; px_pos < px_len; px_pos += channels) {
		if (run > 0) {
			run--;
		}
		else if (p < chunks_len) {
			int b1 = bytes[p++];

			if (b1 == QOI_OP_RGB) {
				px.rgba.r = bytes[p++];
				px.rgba.g = bytes[p++];
				px.rgba.b = bytes[p++];
			}
			else if (b1 == QOI_OP_RGBA) {
				px.rgba.r = bytes[p++];
				px.rgba.g = bytes[p++];
				px.rgba.b = bytes[p++];
				px.rgba.a = bytes[p++];
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
				px = index[b1];
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
				px.rgba.r += ((b1 >> 4) & 0x03) - 2;
				px.rgba.g += ((b1 >> 2) & 0x03) - 2;
				px.rgba.b += ( b1       & 0x03) - 2;
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
				int b2 = bytes[p++];
				int vg = (b1 & 0x3f) - 32;
				px.rgba.r += vg - 8 + ((b2 >> 4) & 0x0f);
				px.rgba.g += vg;
				px.rgba.b += vg - 8 +  (b2       & 0x0f);
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_RUN) {
				run = (b1 & 0x3f);
			}

			index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
		}

		pixels[px_pos + 0] = px.rgba.r;
		pixels[px_pos + 1] = px.rgba.g;
		pixels[px_pos + 2] = px.rgba.b;
		
		if (channels == 4) {
			pixels[px_pos + 3] = px.rgba.a;
		}
	}

	return pixels;
}

#ifndef QOI_NO_STDIO
#include <stdio.h>

int qoi_write(const char *filename, const void *data, const qoi_desc *desc) {
	FILE *f = fopen(filename, "wb");
	int size, err;
	void *encoded;

	if (!f) {
		return 0;
	}

	encoded = qoi_encode(data, desc, &size);
	if (!encoded) {
		fclose(f);
		return 0;
	}

	fwrite(encoded, 1, size, f);
	fflush(f);
	err = ferror(f);
	fclose(f);

	QOI_FREE(encoded);
	return err ? 0 : size;
}

void *qoi_read(const char *filename, qoi_desc *desc, int channels) {
	FILE *f = fopen(filename, "rb");
	int size, bytes_read;
	void *pixels, *data;

	if (!f) {
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}

	data = QOI_MALLOC(size);
	if (!data) {
		fclose(f);
		return NULL;
	}

	bytes_read = fread(data, 1, size, f);
	fclose(f);
	pixels = (bytes_read != size) ? NULL : qoi_decode(data, bytes_read, desc, channels);
	QOI_FREE(data);
	return pixels;
}

#endif /* QOI_NO_STDIO */


/* -----------------------------------------------------------------------------
Indexed color pipeline implementation */

typedef struct {
	int start;
	int len;
	int channel;
	int range;
} qoi_mc_box_t;

static int qoi_mc_sort_channel;

static int qoi_mc_cmp(const void *pa, const void *pb) {
	const unsigned char *a = (const unsigned char *)pa;
	const unsigned char *b = (const unsigned char *)pb;
	int c, d;
	d = (int)a[qoi_mc_sort_channel] - (int)b[qoi_mc_sort_channel];
	if (d) return d;
	for (c = 0; c < 4; c++) {
		d = (int)a[c] - (int)b[c];
		if (d) return d;
	}
	return 0;
}

static void qoi_mc_measure(const qoi_palette_color_t *colors, qoi_mc_box_t *box) {
	unsigned char cmin[4] = {255, 255, 255, 255};
	unsigned char cmax[4] = {0, 0, 0, 0};
	int j, c, best_c = 0, best_range = 0;
	if (box->len < 2) {
		box->channel = 0;
		box->range = 0;
		return;
	}
	for (j = box->start; j < box->start + box->len; j++) {
		const unsigned char *px = (const unsigned char *)&colors[j];
		for (c = 0; c < 4; c++) {
			if (px[c] < cmin[c]) cmin[c] = px[c];
			if (px[c] > cmax[c]) cmax[c] = px[c];
		}
	}
	for (c = 0; c < 4; c++) {
		int range = (int)cmax[c] - (int)cmin[c];
		if (range > best_range) {
			best_range = range;
			best_c = c;
		}
	}
	box->channel = best_c;
	box->range = best_range;
}

int qoi_palette_build(const void *pixels, int width, int height, int channels,
	int num_colors, qoi_palette_color_t *palette_out) {

	qoi_palette_color_t *colors;
	qoi_mc_box_t boxes[QOI_PALETTE_MAX];
	int px_count, num_boxes, i, c;

	if (
		!pixels || !palette_out ||
		width <= 0 || height <= 0 ||
		(channels != 3 && channels != 4) ||
		num_colors < 2 || num_colors > QOI_PALETTE_MAX ||
		(unsigned int)width * (unsigned int)height > QOI_PIXELS_MAX
	) {
		return 0;
	}

	px_count = width * height;
	colors = (qoi_palette_color_t *)QOI_MALLOC(px_count * sizeof(qoi_palette_color_t));
	if (!colors) {
		return 0;
	}

	{
		const unsigned char *src = (const unsigned char *)pixels;
		for (i = 0; i < px_count; i++, src += channels) {
			colors[i].r = src[0];
			colors[i].g = src[1];
			colors[i].b = src[2];
			colors[i].a = (channels == 4) ? src[3] : 255;
		}
	}

	boxes[0].start = 0;
	boxes[0].len = px_count;
	num_boxes = 1;
	qoi_mc_measure(colors, &boxes[0]);

	while (num_boxes < num_colors) {
		int split = -1, split_range = 0;
		for (i = 0; i < num_boxes; i++) {
			if (boxes[i].range > split_range) {
				split_range = boxes[i].range;
				split = i;
			}
		}
		if (split < 0) {
			break; /* no box can be split any further */
		}
		{
			qoi_mc_box_t *box = &boxes[split];
			qoi_mc_sort_channel = box->channel;
			qsort(colors + box->start, box->len, sizeof(qoi_palette_color_t), qoi_mc_cmp);
			boxes[num_boxes].start = box->start + box->len / 2;
			boxes[num_boxes].len = box->len - box->len / 2;
			box->len = box->len / 2;
			qoi_mc_measure(colors, box);
			qoi_mc_measure(colors, &boxes[num_boxes]);
			num_boxes++;
		}
	}

	for (i = 0; i < num_boxes; i++) {
		unsigned int sum[4] = {0, 0, 0, 0};
		int j;
		for (j = boxes[i].start; j < boxes[i].start + boxes[i].len; j++) {
			const unsigned char *px = (const unsigned char *)&colors[j];
			for (c = 0; c < 4; c++) {
				sum[c] += px[c];
			}
		}
		for (c = 0; c < 4; c++) {
			((unsigned char *)&palette_out[i])[c] =
				(unsigned char)((sum[c] + boxes[i].len / 2) / boxes[i].len);
		}
	}

	QOI_FREE(colors);
	return num_boxes;
}

int qoi_palette_map(const void *pixels, int width, int height, int channels,
	const qoi_palette_color_t *palette, int num_colors,
	unsigned char *indices_out) {

	int px_count, i, k;
	const unsigned char *src;

	if (
		!pixels || !palette || !indices_out ||
		width <= 0 || height <= 0 ||
		(channels != 3 && channels != 4) ||
		num_colors < 1 || num_colors > QOI_PALETTE_MAX ||
		(unsigned int)width * (unsigned int)height > QOI_PIXELS_MAX
	) {
		return -1;
	}

	px_count = width * height;
	src = (const unsigned char *)pixels;
	for (i = 0; i < px_count; i++, src += channels) {
		int best = 0;
		unsigned int best_err = 0xffffffff;
		unsigned char pa = (channels == 4) ? src[3] : 255;
		for (k = 0; k < num_colors; k++) {
			int dr = (int)src[0] - palette[k].r;
			int dg = (int)src[1] - palette[k].g;
			int db = (int)src[2] - palette[k].b;
			int da = (int)pa - palette[k].a;
			unsigned int err = (unsigned int)(dr*dr + dg*dg + db*db + da*da);
			if (err < best_err) {
				best_err = err;
				best = k;
			}
		}
		indices_out[i] = (unsigned char)best;
	}
	return 0;
}

int qoi_palette_apply(const unsigned char *indices, int count,
	const qoi_palette_color_t *palette, int num_colors, int channels,
	void *pixels_out) {

	int i;
	unsigned char *dst;

	if (
		!indices || !palette || !pixels_out || count < 0 ||
		(channels != 3 && channels != 4) ||
		num_colors < 1 || num_colors > QOI_PALETTE_MAX
	) {
		return -1;
	}

	dst = (unsigned char *)pixels_out;
	for (i = 0; i < count; i++) {
		const qoi_palette_color_t *c;
		if (indices[i] >= num_colors) {
			return -1;
		}
		c = &palette[indices[i]];
		dst[0] = c->r;
		dst[1] = c->g;
		dst[2] = c->b;
		if (channels == 4) {
			dst[3] = c->a;
		}
		dst += channels;
	}
	return 0;
}

int qoi_index_encode_rle(const unsigned char *indices, int count,
	unsigned char *out, int out_cap) {

	int i = 0, p = 0;

	if (!indices || !out || count < 0 || out_cap < 0) {
		return -1;
	}

	while (i < count) {
		int run = 1;
		while (i + run < count && indices[i + run] == indices[i] && run < 128) {
			run++;
		}
		if (run >= 2) {
			if (p + 2 > out_cap) return -1;
			out[p++] = (unsigned char)(0x80 | (run - 1));
			out[p++] = indices[i];
			i += run;
		}
		else {
			int lit_start = i, lit_len = 0;
			while (i < count && lit_len < 128) {
				run = 1;
				while (i + run < count && indices[i + run] == indices[i] && run < 128) {
					run++;
				}
				if (run >= 2) break;
				i += run;
				lit_len += run;
			}
			if (p + 1 + lit_len > out_cap) return -1;
			out[p++] = (unsigned char)(lit_len - 1);
			memcpy(out + p, indices + lit_start, lit_len);
			p += lit_len;
		}
	}
	return p;
}

int qoi_index_decode_rle(const unsigned char *in, int in_len,
	unsigned char *indices_out, int count) {

	int p = 0, o = 0;

	if (!in || !indices_out || in_len < 0 || count < 0) {
		return -1;
	}

	while (p < in_len) {
		int header = in[p++];
		if (header < 0x80) {
			int lit_len = header + 1;
			if (p + lit_len > in_len || o + lit_len > count) return -1;
			memcpy(indices_out + o, in + p, lit_len);
			p += lit_len;
			o += lit_len;
		}
		else {
			int run = (header & 0x7f) + 1;
			if (p + 1 > in_len || o + run > count) return -1;
			memset(indices_out + o, in[p++], run);
			o += run;
		}
	}
	return (o == count) ? 0 : -1;
}

int qoi_index_encode_delta(const unsigned char *indices, int count,
	int use_delta, unsigned char *out, int out_cap) {

	int i, p = 0;

	if (!indices || !out || count < 0 || out_cap < 0) {
		return -1;
	}

	if (!use_delta) {
		if (count > out_cap) return -1;
		memcpy(out, indices, count);
		return count;
	}

	for (i = 0; i < count; i++) {
		if (i == 0) {
			if (p + 1 > out_cap) return -1;
			out[p++] = indices[0];
		}
		else {
			int d = (int)indices[i] - (int)indices[i - 1];
			unsigned int zz = (unsigned int)((d << 1) ^ (d >> 31));
			do {
				unsigned char byte = (unsigned char)(zz & 0x7f);
				zz >>= 7;
				if (p + 1 > out_cap) return -1;
				out[p++] = byte | (zz ? 0x80 : 0);
			} while (zz);
		}
	}
	return p;
}

int qoi_index_decode_delta(const unsigned char *in, int in_len,
	int use_delta, unsigned char *indices_out, int count) {

	int i, p = 0;

	if (!in || !indices_out || in_len < 0 || count < 0) {
		return -1;
	}

	if (!use_delta) {
		if (in_len != count) return -1;
		memcpy(indices_out, in, count);
		return 0;
	}

	if (count == 0) {
		return (in_len == 0) ? 0 : -1;
	}

	if (p >= in_len) return -1;
	indices_out[0] = in[p++];
	for (i = 1; i < count; i++) {
		unsigned int zz = 0;
		int shift = 0, byte, d, idx;
		do {
			if (p >= in_len || shift > 28) return -1;
			byte = in[p++];
			zz |= (unsigned int)(byte & 0x7f) << shift;
			shift += 7;
		} while (byte & 0x80);
		d = (int)(zz >> 1) ^ -(int)(zz & 1);
		idx = (int)indices_out[i - 1] + d;
		if (idx < 0 || idx > 255) return -1;
		indices_out[i] = (unsigned char)idx;
	}
	return (p == in_len) ? 0 : -1;
}

#endif /* QOI_IMPLEMENTATION */
