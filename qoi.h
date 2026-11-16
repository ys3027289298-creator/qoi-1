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
Indexed color ("qoix") quantization

The functions below turn a true color (RGB/RGBA) image into an indexed color
image: a palette of at most 256 colors plus one byte per pixel selecting a
palette entry. Two palette quantization methods are provided and can be
switched between at run time:

	QOI_QUANT_UNIFORM -- uniform quantization on a regular RGB grid
	QOI_QUANT_MEDIAN  -- median cut quantization

Everything is independent of the lossless QOI codec above and does not change
its behavior or any of its signatures.

Two interchange formats share the same 20 byte header (all multi byte integers
are stored big endian); they only differ in their payload:

	struct qoi_x_header_t {
		char     magic[4];   // "qoix" (indexed) or "qoip" (passthrough)
		uint32_t width;
		uint32_t height;
		uint8_t  channels;   // 3 or 4
		uint8_t  colorspace;
		uint8_t  method;     // QOI_QUANT_* for "qoix", 0 for "qoip"
		uint16_t block_size; // block edge length in pixels, 0 = whole image
		uint16_t num_colors; // requested palette size, 0 for "qoip"
		uint8_t  reserved;   // always 0
	};

"qoip" (passthrough) stores the raw pixel bytes verbatim; encoding and
decoding is lossless and the encoded payload is byte for byte identical to
the input pixels.

"qoix" (indexed) stores the image as independently quantized, square blocks
of block_size x block_size pixels (the blocks at the right and bottom edge
may be smaller; block_size 0 means one block covering the whole image). Each
block holds its own palette and a run length encoded index array, so a
streaming encoder only has to keep a single block and a single palette in
memory at a time. Its working set therefore scales with the block size, not
with the image dimensions. */

#define QOI_PALETTE_MAX 256

typedef enum {
	QOI_QUANT_UNIFORM = 0,
	QOI_QUANT_MEDIAN  = 1
} qoi_quant_method_t;

typedef struct {
	unsigned char r, g, b, a;
} qoi_palette_color_t;

/* Parameters for an indexed color encoding. width, height, channels and
colorspace describe the source image exactly like in a qoi_desc. num_colors
is the requested palette size and must be in 1..QOI_PALETTE_MAX; values of
0 or greater than QOI_PALETTE_MAX are rejected. method selects the palette
quantization. block_size is the block edge length in pixels; 0 means one
block for the whole image and any positive value enables block wise
quantization. */

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned char channels;
	unsigned char colorspace;
	unsigned short num_colors;
	unsigned char method;
	unsigned int block_size;
} qoi_indexed_desc_t;


/* Encode raw RGB or RGBA pixels into an indexed color image in memory.

The function either returns NULL on failure (invalid parameters or malloc
failed) or a pointer to the encoded "qoix" data on success. On success
*out_len is set to the encoded size in bytes. The returned data should be
free()d after use. */

void *qoi_indexed_encode(const void *data, const qoi_indexed_desc_t *desc,
	int *out_len);


/* Decode an indexed color ("qoix") image back into raw pixels. If channels
is 0, the channel count from the file header is used; 3 and 4 force the
output format. The function either returns NULL on failure (invalid data or
malloc failed) or a pointer to the reconstructed pixels. On success the
qoi_desc struct is filled with the image description. The returned pixel
data should be free()d after use. */

void *qoi_indexed_decode(const void *data, int size, qoi_desc *desc,
	int channels);


/* Streaming, block wise encoder. Call qoi_indexed_stream_begin() once, then
qoi_indexed_stream_block() once per block in top to bottom, left to right
order and finally qoi_indexed_stream_end(). The blocks of every row must be
exactly desc->block_size pixels wide (except the last block in a row) and
every block except the ones in the last block row must be exactly
desc->block_size pixels high.

The encoder never allocates memory proportional to the whole image: its
working set is bounded by one block of pixels, one palette of at most
QOI_PALETTE_MAX colors and a worst case block output buffer. All three
functions return 0 on success and -1 on failure; after a failure the
encoder has to be released with qoi_indexed_stream_end(). */

typedef struct qoi_indexed_stream qoi_indexed_stream_t;

int qoi_indexed_stream_begin(qoi_indexed_stream_t **stream,
	const qoi_indexed_desc_t *desc,
	void (*write_fn)(const void *data, int size, void *user),
	void *user);

int qoi_indexed_stream_block(qoi_indexed_stream_t *stream,
	const void *block_pixels, int block_w, int block_h);

int qoi_indexed_stream_end(qoi_indexed_stream_t *stream);


/* Encode raw RGB or RGBA pixels into a "qoip" passthrough image in memory.
The raw pixel bytes are copied verbatim into the payload, so the encoded
payload is byte for byte identical to the input.

The function either returns NULL on failure (invalid parameters or malloc
failed) or a pointer to the encoded data on success. On success *out_len is
set to the encoded size in bytes. The returned data should be free()d after
use. */

void *qoi_passthrough_encode(const void *data, const qoi_desc *desc,
	int *out_len);


/* Decode a "qoip" passthrough image. The reconstructed pixels are byte for
byte identical to the pixels passed to qoi_passthrough_encode(). If channels
is 0, the channel count from the file header is used; 3 and 4 force the
output format. The function either returns NULL on failure or a pointer to
the decoded pixels. The returned pixel data should be free()d after use. */

void *qoi_passthrough_decode(const void *data, int size, qoi_desc *desc,
	int channels);


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
#endif /* QOI_IMPLEMENTATION */


/* =============================================================================
Indexed color ("qoix" / "qoip") implementation
============================================================================= */

#ifdef QOI_IMPLEMENTATION

#define QOI_X_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'x'))
#define QOI_P_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'p'))
#define QOI_X_HEADER_SIZE 20

typedef struct {
	int start;
	int len;
	int channel;
	int range;
} qoi_mc_box_t;

struct qoi_indexed_stream {
	qoi_indexed_desc_t desc;
	void (*write_fn)(const void *data, int size, void *user);
	void *user;
	unsigned int blocks_x;
	unsigned int blocks_y;
	unsigned int cur_block;
	int error;

	unsigned char *work;        /* original block pixels, RGBA each */
	int work_cap;
	unsigned char *sort;        /* scratch copy for median cut */
	int sort_cap;
	unsigned char *rle;         /* worst case block index output */
	int rle_cap;
	unsigned char *indices;     /* one index per block pixel */
	int indices_cap;
	qoi_palette_color_t palette[QOI_PALETTE_MAX];
	qoi_mc_box_t boxes[QOI_PALETTE_MAX];
};

static void qoi_x_write_16(unsigned char *bytes, int *p, unsigned int v) {
	bytes[(*p)++] = (0x0000ff00 & v) >> 8;
	bytes[(*p)++] = (0x000000ff & v);
}

static unsigned int qoi_x_read_16(const unsigned char *bytes, int *p) {
	unsigned int a = bytes[(*p)++];
	unsigned int b = bytes[(*p)++];
	return (a << 8) | b;
}

static void qoi_x_sink(qoi_indexed_stream_t *st, const void *data, int size) {
	if (st->error == 0) {
		st->write_fn(data, size, st->user);
	}
}

static int qoi_x_desc_valid(const qoi_indexed_desc_t *desc) {
	if (
		desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		(desc->channels != 3 && desc->channels != 4) ||
		desc->colorspace > 1 ||
		desc->num_colors == 0 ||
		desc->num_colors > QOI_PALETTE_MAX ||
		(desc->method != QOI_QUANT_UNIFORM && desc->method != QOI_QUANT_MEDIAN) ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return 0;
	}
	return 1;
}

static void qoi_x_write_header(qoi_indexed_stream_t *st, unsigned int magic) {
	unsigned char hdr[QOI_X_HEADER_SIZE];
	int p = 0;

	qoi_write_32(hdr, &p, magic);
	qoi_write_32(hdr, &p, st->desc.width);
	qoi_write_32(hdr, &p, st->desc.height);
	hdr[p++] = st->desc.channels;
	hdr[p++] = st->desc.colorspace;
	hdr[p++] = (unsigned char)st->desc.method;
	qoi_x_write_16(hdr, &p, (unsigned int)st->desc.block_size);
	qoi_x_write_16(hdr, &p, (unsigned int)st->desc.num_colors);
	hdr[p++] = 0;
	qoi_x_sink(st, hdr, QOI_X_HEADER_SIZE);
}


/* -- Uniform quantization --------------------------------------------------*/

/* Distribute k levels across the three RGB channels so that the product of
the level counts is as large as possible without exceeding k. The result is
fully deterministic, which keeps repeated encodings byte for byte identical. */
static void qoi_uniform_levels(int k, int *lr, int *lg, int *lb) {
	int best[3] = {1, 1, 1};
	int a, b, c;
	for (a = 1; a <= k; a++) {
		for (b = 1; a * b <= k; b++) {
			for (c = 1; a * b * c <= k; c++) {
				if (a * b * c > best[0] * best[1] * best[2]) {
					best[0] = a;
					best[1] = b;
					best[2] = c;
				}
			}
		}
	}
	*lr = best[0];
	*lg = best[1];
	*lb = best[2];
}

static int qoi_uniform_build(qoi_palette_color_t *palette, int num_colors) {
	int levels[3], c, count;

	qoi_uniform_levels(num_colors, &levels[0], &levels[1], &levels[2]);
	count = levels[0] * levels[1] * levels[2];

	for (c = 0; c < count; c++) {
		int ir = c / (levels[1] * levels[2]);
		int rem = c - ir * (levels[1] * levels[2]);
		int ig = rem / levels[2];
		int ib = rem - ig * levels[2];
		int ch;
		for (ch = 0; ch < 3; ch++) {
			int level = levels[ch];
			int index = (ch == 0) ? ir : (ch == 1) ? ig : ib;
			((unsigned char *)&palette[c])[ch] = (unsigned char)(
				(2 * index + 1) * 256 / (2 * level)
			);
		}
		palette[c].a = 255;
	}
	return count;
}

static int qoi_uniform_index(const unsigned char *px, const int *levels) {
	int idx[3], ch;
	for (ch = 0; ch < 3; ch++) {
		int cell = ((int)px[ch] * levels[ch]) >> 8;
		if (cell >= levels[ch]) {
			cell = levels[ch] - 1;
		}
		idx[ch] = cell;
	}
	return (idx[0] * levels[1] + idx[1]) * levels[2] + idx[2];
}


/* -- Median cut quantization -----------------------------------------------*/

static int qoi_mc_sort_channel;

static int qoi_mc_cmp(const void *pa, const void *pb) {
	const unsigned char *a = (const unsigned char *)pa;
	const unsigned char *b = (const unsigned char *)pb;
	int c, d;
	d = (int)a[qoi_mc_sort_channel] - (int)b[qoi_mc_sort_channel];
	if (d) {
		return d;
	}
	for (c = 0; c < 4; c++) {
		d = (int)a[c] - (int)b[c];
		if (d) {
			return d;
		}
	}
	return 0;
}

static void qoi_mc_measure(const qoi_palette_color_t *colors, int channels,
	qoi_mc_box_t *box) {
	unsigned char cmin[4] = {255, 255, 255, 255};
	unsigned char cmax[4] = {0, 0, 0, 0};
	int j, c, best_c = 0, best_range = -1;

	if (box->len < 2) {
		box->channel = 0;
		box->range = 0;
		return;
	}

	for (j = box->start; j < box->start + box->len; j++) {
		const unsigned char *px = (const unsigned char *)&colors[j];
		for (c = 0; c < channels; c++) {
			if (px[c] < cmin[c]) {
				cmin[c] = px[c];
			}
			if (px[c] > cmax[c]) {
				cmax[c] = px[c];
			}
		}
	}

	for (c = 0; c < channels; c++) {
		int range = (int)cmax[c] - (int)cmin[c];
		if (range > best_range) {
			best_range = range;
			best_c = c;
		}
	}
	box->channel = best_c;
	box->range = best_range;
}

static int qoi_median_build(qoi_palette_color_t *colors, int count,
	int channels, int num_colors, qoi_mc_box_t *boxes,
	qoi_palette_color_t *palette_out) {
	int num_boxes = 1, i, c;

	boxes[0].start = 0;
	boxes[0].len = count;
	qoi_mc_measure(colors, channels, &boxes[0]);

	while (num_boxes < num_colors) {
		int split = -1, split_range = -1, half;
		qoi_mc_box_t *box;

		for (i = 0; i < num_boxes; i++) {
			if (boxes[i].len > 1 && boxes[i].range > split_range) {
				split_range = boxes[i].range;
				split = i;
			}
		}
		if (split < 0) {
			break;
		}

		box = &boxes[split];
		qoi_mc_sort_channel = box->channel;
		qsort(colors + box->start, box->len, sizeof(qoi_palette_color_t),
			qoi_mc_cmp);

		half = box->len / 2;
		boxes[num_boxes].start = box->start + half;
		boxes[num_boxes].len = box->len - half;
		box->len = half;

		qoi_mc_measure(colors, channels, box);
		qoi_mc_measure(colors, channels, &boxes[num_boxes]);
		num_boxes++;
	}

	for (i = 0; i < num_boxes; i++) {
		unsigned int sum[4] = {0, 0, 0, 0};
		int j;
		for (j = boxes[i].start; j < boxes[i].start + boxes[i].len; j++) {
			const unsigned char *px = (const unsigned char *)&colors[j];
			for (c = 0; c < channels; c++) {
				sum[c] += px[c];
			}
		}
		for (c = 0; c < 4; c++) {
			if (c < channels) {
				((unsigned char *)&palette_out[i])[c] = (unsigned char)(
					(sum[c] + (unsigned int)boxes[i].len / 2) /
					(unsigned int)boxes[i].len
				);
			}
			else {
				((unsigned char *)&palette_out[i])[c] =
					(c == 3) ? 255 : 0;
			}
		}
	}
	return num_boxes;
}


/* Map one pixel to the nearest palette entry. Ties resolve to the lowest
index, which keeps mapping deterministic. */
static int qoi_nearest_index(const unsigned char *px, int channels,
	const qoi_palette_color_t *palette, int num_colors) {
	int k, best = 0;
	unsigned int best_err = 0xffffffff;
	unsigned char pa = (channels == 4) ? px[3] : 255;

	for (k = 0; k < num_colors; k++) {
		int dr = (int)px[0] - palette[k].r;
		int dg = (int)px[1] - palette[k].g;
		int db = (int)px[2] - palette[k].b;
		int da = (int)pa - palette[k].a;
		unsigned int err = (unsigned int)(
			dr * dr + dg * dg + db * db + da * da
		);
		if (err < best_err) {
			best_err = err;
			best = k;
		}
	}
	return best;
}


/* Run length encoding of an index array. A header byte < 0x80 starts a
literal run of (header + 1) raw indices; a header byte >= 0x80 starts a
repeated run of (header & 0x7f) + 1 copies of the single following index.
Worst case output size is 2 * count + 1 bytes. Returns the bytes written or
-1 if the output buffer is too small. */
static int qoi_rle_encode(const unsigned char *indices, int count,
	unsigned char *out, int out_cap) {
	int i = 0, p = 0;

	while (i < count) {
		int run = 1;
		while (
			i + run < count &&
			indices[i + run] == indices[i] &&
			run < 128
		) {
			run++;
		}

		if (run >= 2) {
			if (p + 2 > out_cap) {
				return -1;
			}
			out[p++] = (unsigned char)(0x80 | (run - 1));
			out[p++] = indices[i];
			i += run;
		}
		else {
			int lit_start = i, lit_len = 0;
			while (i < count && lit_len < 128) {
				int next_run = 1;
				while (
					i + next_run < count &&
					indices[i + next_run] == indices[i] &&
					next_run < 128
				) {
					next_run++;
				}
				if (next_run >= 2) {
					break;
				}
				i += next_run;
				lit_len += next_run;
			}
			if (p + 1 + lit_len > out_cap) {
				return -1;
			}
			out[p++] = (unsigned char)(lit_len - 1);
			memcpy(out + p, indices + lit_start, lit_len);
			p += lit_len;
		}
	}
	return p;
}

/* Inverse of qoi_rle_encode(). Produces exactly count indices; returns 0 on
success or -1 on malformed input. */
static int qoi_rle_decode(const unsigned char *in, int in_len,
	unsigned char *indices_out, int count) {
	int p = 0, o = 0;

	while (p < in_len) {
		int header = in[p++];
		if (header < 0x80) {
			int lit_len = header + 1;
			if (p + lit_len > in_len || o + lit_len > count) {
				return -1;
			}
			memcpy(indices_out + o, in + p, lit_len);
			p += lit_len;
			o += lit_len;
		}
		else {
			int run = (header & 0x7f) + 1;
			if (p + 1 > in_len || o + run > count) {
				return -1;
			}
			memset(indices_out + o, in[p++], run);
			o += run;
		}
	}
	return (o == count) ? 0 : -1;
}


/* -- Streaming block encoder -----------------------------------------------*/

int qoi_indexed_stream_begin(qoi_indexed_stream_t **stream,
	const qoi_indexed_desc_t *desc,
	void (*write_fn)(const void *data, int size, void *user),
	void *user) {

	qoi_indexed_stream_t *st;
	unsigned int block;

	if (
		stream == NULL || write_fn == NULL ||
		!qoi_x_desc_valid(desc)
	) {
		return -1;
	}

	st = (qoi_indexed_stream_t *)QOI_MALLOC(sizeof(qoi_indexed_stream_t));
	if (!st) {
		return -1;
	}
	memset(st, 0, sizeof(qoi_indexed_stream_t));
	st->desc = *desc;
	st->write_fn = write_fn;
	st->user = user;

	block = desc->block_size ? desc->block_size : desc->width;
	st->blocks_x = (desc->width + block - 1) / block;
	st->blocks_y = (desc->height + block - 1) / block;

	qoi_x_write_header(st, QOI_X_MAGIC);
	if (st->error) {
		QOI_FREE(st);
		return -1;
	}

	*stream = st;
	return 0;
}

int qoi_indexed_stream_block(qoi_indexed_stream_t *stream,
	const void *block_pixels, int block_w, int block_h) {

	qoi_indexed_stream_t *st = stream;
	const unsigned char *src;
	unsigned char rec[6];
	int levels[3];
	int num_palette, n, i;
	int need_work, need_rle;

	if (
		st == NULL || block_pixels == NULL ||
		block_w <= 0 || block_h <= 0
	) {
		return -1;
	}

	n = block_w * block_h;
	need_work = n * (int)sizeof(qoi_palette_color_t);
	need_rle = 2 * n + 1;

	if (need_work > st->work_cap) {
		unsigned char *tmp = (unsigned char *)QOI_MALLOC(need_work);
		if (!tmp) {
			st->error = 1;
			return -1;
		}
		QOI_FREE(st->work);
		st->work = tmp;
		st->work_cap = need_work;
	}
	if (need_work > st->sort_cap) {
		unsigned char *tmp = (unsigned char *)QOI_MALLOC(need_work);
		if (!tmp) {
			st->error = 1;
			return -1;
		}
		QOI_FREE(st->sort);
		st->sort = tmp;
		st->sort_cap = need_work;
	}
	if (n > st->indices_cap) {
		unsigned char *tmp = (unsigned char *)QOI_MALLOC(n);
		if (!tmp) {
			st->error = 1;
			return -1;
		}
		QOI_FREE(st->indices);
		st->indices = tmp;
		st->indices_cap = n;
	}
	if (need_rle > st->rle_cap) {
		unsigned char *tmp = (unsigned char *)QOI_MALLOC(need_rle);
		if (!tmp) {
			st->error = 1;
			return -1;
		}
		QOI_FREE(st->rle);
		st->rle = tmp;
		st->rle_cap = need_rle;
	}

	src = (const unsigned char *)block_pixels;
	for (i = 0; i < n; i++, src += st->desc.channels) {
		st->work[i * 4 + 0] = src[0];
		st->work[i * 4 + 1] = src[1];
		st->work[i * 4 + 2] = src[2];
		st->work[i * 4 + 3] =
			(st->desc.channels == 4) ? src[3] : 255;
	}

	if (st->desc.method == QOI_QUANT_UNIFORM) {
		num_palette = qoi_uniform_build(
			st->palette, st->desc.num_colors
		);
		qoi_uniform_levels(
			st->desc.num_colors, &levels[0], &levels[1], &levels[2]
		);
		for (i = 0; i < n; i++) {
			st->indices[i] = (unsigned char)qoi_uniform_index(
				st->work + i * 4, levels
			);
		}
	}
	else {
		qoi_palette_color_t *mc_colors =
			(qoi_palette_color_t *)st->sort;
		memcpy(st->sort, st->work,
			n * sizeof(qoi_palette_color_t));
		num_palette = qoi_median_build(
			mc_colors, n, st->desc.channels, st->desc.num_colors,
			st->boxes, st->palette
		);
		for (i = 0; i < n; i++) {
			st->indices[i] = (unsigned char)qoi_nearest_index(
				st->work + i * 4, st->desc.channels,
				st->palette, num_palette
			);
		}
	}

	{
		int rle_len = qoi_rle_encode(
			st->indices, n, st->rle, st->rle_cap
		);
		if (rle_len < 0) {
			st->error = 1;
			return -1;
		}

		/* Block record: uint16 width, uint16 height, uint16 palette entries,
		followed by the palette (4 bytes each) and the RLE index stream. */
		rec[0] = (block_w >> 8) & 0xff;
		rec[1] = block_w & 0xff;
		rec[2] = (block_h >> 8) & 0xff;
		rec[3] = block_h & 0xff;
		rec[4] = (num_palette >> 8) & 0xff;
		rec[5] = num_palette & 0xff;

		qoi_x_sink(st, rec, 6);
		qoi_x_sink(st, st->palette, num_palette * (int)sizeof(qoi_palette_color_t));
		qoi_x_sink(st, st->rle, rle_len);

		st->cur_block++;
		if (st->cur_block > st->blocks_x * st->blocks_y) {
			st->error = 1;
			return -1;
		}
	}

	return st->error ? -1 : 0;
}

int qoi_indexed_stream_end(qoi_indexed_stream_t *stream) {
	qoi_indexed_stream_t *st = stream;
	int ok;

	if (st == NULL) {
		return -1;
	}
	ok = (
		st->error == 0 &&
		st->cur_block == st->blocks_x * st->blocks_y
	);

	QOI_FREE(st->work);
	QOI_FREE(st->sort);
	QOI_FREE(st->rle);
	QOI_FREE(st->indices);
	QOI_FREE(st);
	return ok ? 0 : -1;
}


/* -- Growable in-memory sink for the one shot encoder ----------------------*/

typedef struct {
	unsigned char *data;
	int len;
	int cap;
	int error;
} qoi_mem_sink_t;

static void qoi_mem_sink_write(const void *data, int size, void *user) {
	qoi_mem_sink_t *sink = (qoi_mem_sink_t *)user;
	if (sink->error) {
		return;
	}
	if (size > sink->cap - sink->len) {
		int new_cap = sink->cap ? sink->cap : 64;
		unsigned char *tmp;
		while (new_cap < sink->len + size) {
			new_cap *= 2;
			if (new_cap <= 0) {
				sink->error = 1;
				return;
			}
		}
		tmp = (unsigned char *)QOI_MALLOC(new_cap);
		if (!tmp) {
			sink->error = 1;
			return;
		}
		if (sink->data) {
			memcpy(tmp, sink->data, sink->len);
			QOI_FREE(sink->data);
		}
		sink->data = tmp;
		sink->cap = new_cap;
	}
	memcpy(sink->data + sink->len, data, size);
	sink->len += size;
}

void *qoi_indexed_encode(const void *data, const qoi_indexed_desc_t *desc,
	int *out_len) {

	qoi_mem_sink_t sink;
	qoi_indexed_stream_t *st;
	const unsigned char *pixels;
	unsigned int block, blocks_x, blocks_y, by, bx;

	if (data == NULL || out_len == NULL || !qoi_x_desc_valid(desc)) {
		return NULL;
	}

	sink.data = NULL;
	sink.len = 0;
	sink.cap = 0;
	sink.error = 0;

	if (qoi_indexed_stream_begin(&st, desc, qoi_mem_sink_write, &sink)) {
		return NULL;
	}

	block = desc->block_size ? desc->block_size : desc->width;
	blocks_x = (desc->width + block - 1) / block;
	blocks_y = (desc->height + block - 1) / block;
	pixels = (const unsigned char *)data;

	for (by = 0; by < blocks_y; by++) {
		for (bx = 0; bx < blocks_x; bx++) {
			unsigned int x0 = bx * block;
			unsigned int y0 = by * block;
			int bw = (x0 + block <= desc->width) ?
				(int)block : (int)(desc->width - x0);
			int bh = (y0 + block <= desc->height) ?
				(int)block : (int)(desc->height - y0);
			int row, step = bw * desc->channels;
			unsigned char *block_pixels = (unsigned char *)QOI_MALLOC(
				(size_t)bw * bh * desc->channels
			);
			if (!block_pixels) {
				qoi_indexed_stream_end(st);
				QOI_FREE(sink.data);
				return NULL;
			}
			for (row = 0; row < bh; row++) {
				const unsigned char *src = pixels +
					((y0 + row) * desc->width + x0) * desc->channels;
				memcpy(block_pixels + row * step, src, step);
			}
			if (qoi_indexed_stream_block(st, block_pixels, bw, bh)) {
				QOI_FREE(block_pixels);
				qoi_indexed_stream_end(st);
				QOI_FREE(sink.data);
				return NULL;
			}
			QOI_FREE(block_pixels);
		}
	}

	if (qoi_indexed_stream_end(st) || sink.error) {
		QOI_FREE(sink.data);
		return NULL;
	}

	*out_len = sink.len;
	return sink.data;
}


/* -- Indexed decoder -------------------------------------------------------*/

void *qoi_indexed_decode(const void *data, int size, qoi_desc *desc,
	int channels) {

	const unsigned char *bytes;
	unsigned int magic, blocks_x, blocks_y, block, b;
	int p = 0, out_channels;
	qoi_indexed_desc_t hdr;
	unsigned char *pixels;
	qoi_palette_color_t *palette;
	unsigned char *indices;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QOI_X_HEADER_SIZE
	) {
		return NULL;
	}
	bytes = (const unsigned char *)data;

	magic = qoi_read_32(bytes, &p);
	hdr.width = qoi_read_32(bytes, &p);
	hdr.height = qoi_read_32(bytes, &p);
	hdr.channels = bytes[p++];
	hdr.colorspace = bytes[p++];
	hdr.method = bytes[p++];
	hdr.block_size = (unsigned short)qoi_x_read_16(bytes, &p);
	hdr.num_colors = (unsigned short)qoi_x_read_16(bytes, &p);
	if (bytes[p++] != 0) {
		return NULL;
	}

	if (
		magic != QOI_X_MAGIC ||
		!qoi_x_desc_valid(&hdr)
	) {
		return NULL;
	}

	out_channels = channels ? channels : hdr.channels;
	desc->width = hdr.width;
	desc->height = hdr.height;
	desc->channels = (unsigned char)out_channels;
	desc->colorspace = hdr.colorspace;

	block = hdr.block_size ? hdr.block_size : hdr.width;
	blocks_x = (hdr.width + block - 1) / block;
	blocks_y = (hdr.height + block - 1) / block;

	pixels = (unsigned char *)QOI_MALLOC(
		(size_t)hdr.width * hdr.height * out_channels
	);
	if (!pixels) {
		return NULL;
	}
	/* Working buffers are sized to one block, never to the whole image. */
	palette = (qoi_palette_color_t *)QOI_MALLOC(
		QOI_PALETTE_MAX * sizeof(qoi_palette_color_t)
	);
	indices = (unsigned char *)QOI_MALLOC((size_t)block * block);
	if (!palette || !indices) {
		QOI_FREE(pixels);
		QOI_FREE(palette);
		QOI_FREE(indices);
		return NULL;
	}

	for (b = 0; b < blocks_x * blocks_y; b++) {
		unsigned int bw, bh, num_palette, bx, by, x0, y0;
		int rle_len, count, i, rp, produced, hb;

		if (p + 6 > size) {
			goto fail;
		}
		bw = qoi_x_read_16(bytes, &p);
		bh = qoi_x_read_16(bytes, &p);
		num_palette = qoi_x_read_16(bytes, &p);

		if (
			bw == 0 || bh == 0 ||
			num_palette == 0 || num_palette > QOI_PALETTE_MAX ||
			bw > block || bh > block ||
			p + (int)num_palette * 4 > size
		) {
			goto fail;
		}
		memcpy(palette, bytes + p, num_palette * 4);
		p += num_palette * 4;

		count = (int)(bw * bh);
		if (count > (int)(block * block)) {
			goto fail;
		}
		/* Walk the self delimiting RLE stream to find its exact length,
		then decode it. This stays independent of the following block. */
		rp = 0;
		produced = 0;
		while (produced < count) {
			if (p + rp >= size) {
				goto fail;
			}
			hb = bytes[p + rp++];
			if (hb < 0x80) {
				int lit = hb + 1;
				if (p + rp + lit > size || produced + lit > count) {
					goto fail;
				}
				rp += lit;
				produced += lit;
			}
			else {
				int run = (hb & 0x7f) + 1;
				if (p + rp >= size || produced + run > count) {
					goto fail;
				}
				rp += 1;
				produced += run;
			}
		}
		rle_len = rp;
		if (qoi_rle_decode(bytes + p, rle_len, indices, count)) {
			goto fail;
		}
		p += rle_len;

		bx = b % blocks_x;
		by = b / blocks_x;
		x0 = bx * block;
		y0 = by * block;
		if (x0 + bw > hdr.width || y0 + bh > hdr.height) {
			goto fail;
		}

		for (i = 0; i < count; i++) {
			unsigned int px_x = x0 + (i % bw);
			unsigned int px_y = y0 + (i / bw);
			unsigned char *dst = pixels +
				(px_y * hdr.width + px_x) * out_channels;
			qoi_palette_color_t c;
			if (indices[i] >= num_palette) {
				goto fail;
			}
			c = palette[indices[i]];
			dst[0] = c.r;
			dst[1] = c.g;
			dst[2] = c.b;
			if (out_channels == 4) {
				dst[3] = (hdr.channels == 4) ? c.a : 255;
			}
		}
	}

	if (p != size) {
		goto fail;
	}
	QOI_FREE(palette);
	QOI_FREE(indices);
	return pixels;

fail:
	QOI_FREE(pixels);
	QOI_FREE(palette);
	QOI_FREE(indices);
	return NULL;
}


/* -- Passthrough ("qoip") --------------------------------------------------*/

void *qoi_passthrough_encode(const void *data, const qoi_desc *desc,
	int *out_len) {

	unsigned char *out;
	int px_len, total, p = 0;

	if (
		data == NULL || desc == NULL || out_len == NULL ||
		desc->width == 0 || desc->height == 0 ||
		(desc->channels != 3 && desc->channels != 4) ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	px_len = (int)((unsigned int)desc->width * desc->height * desc->channels);
	total = QOI_X_HEADER_SIZE + px_len;
	out = (unsigned char *)QOI_MALLOC(total);
	if (!out) {
		return NULL;
	}

	qoi_write_32(out, &p, QOI_P_MAGIC);
	qoi_write_32(out, &p, desc->width);
	qoi_write_32(out, &p, desc->height);
	out[p++] = desc->channels;
	out[p++] = desc->colorspace;
	out[p++] = 0;
	qoi_x_write_16(out, &p, 0);
	qoi_x_write_16(out, &p, 0);
	out[p++] = 0;

	memcpy(out + p, data, px_len);
	*out_len = total;
	return out;
}

void *qoi_passthrough_decode(const void *data, int size, qoi_desc *desc,
	int channels) {

	const unsigned char *bytes;
	unsigned int magic, width, height, block_size, num_colors;
	unsigned char src_channels, colorspace, method, reserved;
	int p = 0, out_channels, px_len;
	unsigned char *pixels;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QOI_X_HEADER_SIZE
	) {
		return NULL;
	}
	bytes = (const unsigned char *)data;

	magic = qoi_read_32(bytes, &p);
	width = qoi_read_32(bytes, &p);
	height = qoi_read_32(bytes, &p);
	src_channels = bytes[p++];
	colorspace = bytes[p++];
	method = bytes[p++];
	block_size = qoi_x_read_16(bytes, &p);
	num_colors = qoi_x_read_16(bytes, &p);
	reserved = bytes[p++];

	if (
		magic != QOI_P_MAGIC ||
		width == 0 || height == 0 ||
		(src_channels != 3 && src_channels != 4) ||
		colorspace > 1 || method != 0 ||
		block_size != 0 || num_colors != 0 || reserved != 0 ||
		height >= QOI_PIXELS_MAX / width
	) {
		return NULL;
	}

	out_channels = channels ? channels : src_channels;
	px_len = (int)(width * height * src_channels);
	if (size != QOI_X_HEADER_SIZE + px_len) {
		return NULL;
	}

	pixels = (unsigned char *)QOI_MALLOC(
		(size_t)width * height * out_channels
	);
	if (!pixels) {
		return NULL;
	}

	if (out_channels == src_channels) {
		memcpy(pixels, bytes + p, px_len);
	}
	else {
		int i;
		for (i = 0; i < (int)(width * height); i++) {
			pixels[i * out_channels + 0] = bytes[p + i * src_channels + 0];
			pixels[i * out_channels + 1] = bytes[p + i * src_channels + 1];
			pixels[i * out_channels + 2] = bytes[p + i * src_channels + 2];
			if (out_channels == 4) {
				pixels[i * out_channels + 3] =
					(src_channels == 4) ? bytes[p + i * src_channels + 3] : 255;
			}
		}
	}

	desc->width = width;
	desc->height = height;
	desc->channels = (unsigned char)out_channels;
	desc->colorspace = colorspace;
	return pixels;
}

#endif /* QOI_IMPLEMENTATION */
