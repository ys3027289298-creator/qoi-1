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

The optional preprocessing pipeline is available through
- qoi_preprocess_encode -- palette quantization, dithering and index deltas
                           in front of the unchanged encoder
- qoi_preprocess_decode -- decode its containers (or plain QOI data)

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
Optional lossy preprocessing

qoi_preprocess_encode() runs an optional palette preprocessing pipeline in
front of the unchanged lossless encoder and stores the result in a small
container; qoi_preprocess_decode() reverses it. qoi_encode() and qoi_decode()
are not modified. With stages == 0 qoi_preprocess_encode() returns exactly the
bytes qoi_encode() would produce.

Three stages can be switched on independently through the QOI_PP_* bits:

	QOI_PP_QUANTIZE -- build an adaptive palette of num_colors (1..256) entries
	                   with median cut over a weighted color histogram and map
	                   every pixel to a palette entry.
	QOI_PP_BAYER    -- assign palette indices with an s x s ordered Bayer
	                   matrix (s = 2, 4, 8 or 16) instead of plain nearest
	                   color, so the error of an area averages out.
	QOI_PP_DELTA    -- difference the palette index sequence against a causal
	                   predictor before handing it to the lossless encoder. The
	                   encoder tries several predictors and several strides
	                   (including "no prediction") and keeps the smallest
	                   payload, so this stage never inflates the payload. A
	                   stride equal to the dither matrix size compares pixels
	                   in the same phase of the Bayer tile, which is where
	                   dithered areas are most predictable.

QOI_PP_BAYER and QOI_PP_DELTA operate on palette indices and therefore imply
QOI_PP_QUANTIZE; the effective stage mask is what gets stored in the container.
Unknown stage bits, a num_colors outside 1..256, or a bayer_size that is not
one of 0/2/4/8/16 (with 2/4/8/16 required when QOI_PP_BAYER is set) make the
encoder fail. The stages are lossy: decoded pixels are palette colors, not the
original pixels.

Container layout:

	char     magic[4];    // "qoix"
	uint32_t width;       // big endian
	uint32_t height;      // big endian
	uint8_t  channels;    // 3 or 4
	uint8_t  colorspace;  // QOI_SRGB or QOI_LINEAR
	uint8_t  stages;      // effective QOI_PP_* mask
	uint8_t  bayer_size;  // 2/4/8/16 with QOI_PP_BAYER, else 0
	uint16_t colors;      // palette entries stored (BE, 1..256)
	uint8_t  predictor;   // 0..5, used with QOI_PP_DELTA
	uint8_t  stride;      // prediction step (1/2/4/8/16), else 1
	uint32_t payload_len; // big endian
	uint8_t  palette[colors][4];   // r,g,b,a
	uint8_t  payload[payload_len]; // QOI file of the index image

The payload is produced by the unmodified qoi_encode() from a 3 channel image
whose r, g and b bytes all carry the (possibly differenced) palette index.
qoi_preprocess_decode() also accepts plain QOI data, which decodes exactly like
qoi_decode() does. */

#define QOI_PP_QUANTIZE 0x01
#define QOI_PP_BAYER    0x02
#define QOI_PP_DELTA    0x04

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned char channels;    /* 3 = RGB, 4 = RGBA */
	unsigned char colorspace;  /* QOI_SRGB or QOI_LINEAR */
	unsigned short num_colors; /* 1..256 */
	unsigned char stages;      /* QOI_PP_* mask */
	unsigned char bayer_size;  /* 0, 2, 4, 8 or 16 */
} qoi_preprocess_desc;

typedef struct {
	unsigned int palette_size; /* palette entries stored */
	unsigned int header_size;  /* container header plus palette bytes */
	unsigned int payload_size; /* QOI payload bytes */
} qoi_preprocess_stats;

/* Encode raw RGB or RGBA pixels with the preprocessing pipeline. Returns NULL
on invalid parameters (including an illegal num_colors or bayer_size) or when a
memory allocation fails. On success *out_len is set to the encoded size and
stats, when not NULL, receives the size breakdown. The returned data should be
free()d after use. */
void *qoi_preprocess_encode(const void *data, const qoi_preprocess_desc *desc,
	int *out_len, qoi_preprocess_stats *stats);


/* Decode data produced by qoi_preprocess_encode(), or plain QOI data. If
channels is 0 the channel count from the header is used; 3 and 4 force the
output format. Returns NULL on invalid data or a failed allocation. The
returned pixel data should be free()d after use. */
void *qoi_preprocess_decode(const void *data, int size, qoi_desc *desc,
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


/* -----------------------------------------------------------------------------
Preprocessing pipeline implementation.

Self contained: it uses the public qoi_encode() and qoi_decode() unchanged.
With stages == 0, qoi_preprocess_encode() returns qoi_encode()'s buffer as is,
so the output is byte for byte identical to a plain qoi_encode() call. */

#ifdef QOI_IMPLEMENTATION

#define QOI_PP_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'x'))
#define QOI_PP_HEADER_SIZE 24

/* Delta predictors. NONE leaves the index sequence untouched and is always
tried, so the delta stage can never make the payload larger than the plain
index image. LL and UU refer to the index two pixels to the left and two rows
up; they fit the checkerboard pattern an ordered dither produces. */
#define QOI_PP_PRED_NONE  0
#define QOI_PP_PRED_LEFT  1
#define QOI_PP_PRED_UP    2
#define QOI_PP_PRED_PAETH 3
#define QOI_PP_PRED_LL    4
#define QOI_PP_PRED_UU    5
#define QOI_PP_PRED_COUNT 6

/* 16x16 ordered Bayer matrix with the thresholds 0..255. The 2x2, 4x4 and 8x8
matrices are its top left corners; each of them spans the full threshold
range, scaled to its own number of levels by the index arithmetic below. */
static const unsigned char qoi_pp_bayer16[16][16] = {
	{   0, 192,  48, 240,  12, 204,  60, 252,   3, 195,  51, 243,  15, 207,  63, 255 },
	{ 128,  64, 176, 112, 140,  76, 188, 124, 131,  67, 179, 115, 143,  79, 191, 127 },
	{  32, 224,  16, 208,  44, 236,  28, 220,  35, 227,  19, 211,  47, 239,  31, 223 },
	{ 160,  96, 144,  80, 172, 108, 156,  92, 163,  99, 147,  83, 175, 111, 159,  95 },
	{   8, 200,  56, 248,   4, 196,  52, 244,  11, 203,  59, 251,   7, 199,  55, 247 },
	{ 136,  72, 184, 120, 132,  68, 180, 116, 139,  75, 187, 123, 135,  71, 183, 119 },
	{  40, 232,  24, 216,  36, 228,  20, 212,  43, 235,  27, 219,  39, 231,  23, 215 },
	{ 168, 104, 152,  88, 164, 100, 148,  84, 171, 107, 155,  91, 167, 103, 151,  87 },
	{   2, 194,  50, 242,  14, 206,  62, 254,   1, 193,  49, 241,  13, 205,  61, 253 },
	{ 130,  66, 178, 114, 142,  78, 190, 126, 129,  65, 177, 113, 141,  77, 189, 125 },
	{  34, 226,  18, 210,  46, 238,  30, 222,  33, 225,  17, 209,  45, 237,  29, 221 },
	{ 162,  98, 146,  82, 174, 110, 158,  94, 161,  97, 145,  81, 173, 109, 157,  93 },
	{  10, 202,  58, 250,   6, 198,  54, 246,   9, 201,  57, 249,   5, 197,  53, 245 },
	{ 138,  74, 186, 122, 134,  70, 182, 118, 137,  73, 185, 121, 133,  69, 181, 117 },
	{  42, 234,  26, 218,  38, 230,  22, 214,  41, 233,  25, 217,  37, 229,  21, 213 },
	{ 170, 106, 154,  90, 166, 102, 150,  86, 169, 105, 153,  89, 165, 101, 149,  85 }
};

typedef struct {
	unsigned char r, g, b, a;
	unsigned int count;
} qoi_pp_color_t;

typedef struct {
	int start, end; /* [start, end) range in the color array */
} qoi_pp_box_t;

/* Perceptual channel weights; green matters most, blue least. */
#define QOI_PP_WR 3
#define QOI_PP_WG 6
#define QOI_PP_WB 1
#define QOI_PP_WA 3

static int qoi_pp_valid_bayer(int size) {
	return size == 2 || size == 4 || size == 8 || size == 16;
}

static int qoi_pp_channel_of(const qoi_pp_color_t *c, int channel) {
	if (channel == 0) { return c->r; }
	if (channel == 1) { return c->g; }
	if (channel == 2) { return c->b; }
	return c->a;
}

/* Integer square root; keeps the pipeline free of libm. */
static unsigned int qoi_pp_isqrt(unsigned int v) {
	unsigned int r = 0;
	unsigned int bit = 1u << 30;
	while (bit > v) {
		bit >>= 2;
	}
	while (bit != 0) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* Heapsort. Sorting by hand keeps the palette independent of the C library's
qsort() implementation, so the same input always yields the same bytes. */

static void qoi_pp_sift_keys(unsigned int *a, int start, int end) {
	int root = start;
	while (root * 2 + 1 <= end) {
		int child = root * 2 + 1;
		int swap = root;
		if (a[swap] < a[child]) {
			swap = child;
		}
		if (child + 1 <= end && a[swap] < a[child + 1]) {
			swap = child + 1;
		}
		if (swap == root) {
			return;
		}
		{
			unsigned int t = a[root];
			a[root] = a[swap];
			a[swap] = t;
		}
		root = swap;
	}
}

static void qoi_pp_sift_colors(qoi_pp_color_t *a, int start, int end, int ch) {
	int root = start;
	while (root * 2 + 1 <= end) {
		int child = root * 2 + 1;
		int swap = root;
		if (qoi_pp_channel_of(&a[swap], ch) < qoi_pp_channel_of(&a[child], ch)) {
			swap = child;
		}
		if (child + 1 <= end &&
			qoi_pp_channel_of(&a[swap], ch) < qoi_pp_channel_of(&a[child + 1], ch)) {
			swap = child + 1;
		}
		if (swap == root) {
			return;
		}
		{
			qoi_pp_color_t t = a[root];
			a[root] = a[swap];
			a[swap] = t;
		}
		root = swap;
	}
}

static void qoi_pp_sort_keys(unsigned int *a, int n) {
	int start, i;
	for (start = (n - 2) / 2; start >= 0; start--) {
		qoi_pp_sift_keys(a, start, n - 1);
	}
	for (i = n - 1; i > 0; i--) {
		unsigned int t = a[i];
		a[i] = a[0];
		a[0] = t;
		qoi_pp_sift_keys(a, 0, i - 1);
	}
}

static void qoi_pp_sort_colors(qoi_pp_color_t *a, int n, int ch) {
	int start, i;
	for (start = (n - 2) / 2; start >= 0; start--) {
		qoi_pp_sift_colors(a, start, n - 1, ch);
	}
	for (i = n - 1; i > 0; i--) {
		qoi_pp_color_t t = a[i];
		a[i] = a[0];
		a[0] = t;
		qoi_pp_sift_colors(a, 0, i - 1, ch);
	}
}

/* ---------------------------------------------------------------------------
Palette construction: median cut over a weighted histogram of the distinct
colors, splitting the box with the widest perceptually weighted channel range
at its weighted median. Each palette entry is the weighted mean of the colors
in its box. The result is ordered by luma so that neighbouring palette indices
are perceptually close, which gives the delta stage smaller residuals. */

static int qoi_pp_build_palette(
	const unsigned char *pixels, int px_count, int channels, int num_colors,
	unsigned char palette[256][4]
) {
	unsigned int *keys;
	qoi_pp_color_t *colors;
	qoi_pp_box_t boxes[256];
	int n_colors = 0;
	int n_boxes = 1;
	int i;

	keys = (unsigned int *)QOI_MALLOC((size_t)(unsigned int)px_count * sizeof(unsigned int));
	if (!keys) {
		return 0;
	}
	for (i = 0; i < px_count; i++) {
		const unsigned char *p = pixels + i * channels;
		keys[i] =
			(unsigned int)p[0] << 24 |
			(unsigned int)p[1] << 16 |
			(unsigned int)p[2] <<  8 |
			(unsigned int)((channels == 4) ? p[3] : 255);
	}
	qoi_pp_sort_keys(keys, px_count);

	colors = (qoi_pp_color_t *)QOI_MALLOC((size_t)(unsigned int)px_count * sizeof(qoi_pp_color_t));
	if (!colors) {
		QOI_FREE(keys);
		return 0;
	}
	for (i = 0; i < px_count; i++) {
		unsigned int key = keys[i];
		unsigned char r = (key >> 24) & 0xff;
		unsigned char g = (key >> 16) & 0xff;
		unsigned char b = (key >>  8) & 0xff;
		unsigned char a =  key        & 0xff;
		if (n_colors > 0 &&
			colors[n_colors - 1].r == r && colors[n_colors - 1].g == g &&
			colors[n_colors - 1].b == b && colors[n_colors - 1].a == a
		) {
			colors[n_colors - 1].count++;
		}
		else {
			colors[n_colors].r = r;
			colors[n_colors].g = g;
			colors[n_colors].b = b;
			colors[n_colors].a = a;
			colors[n_colors].count = 1;
			n_colors++;
		}
	}
	QOI_FREE(keys);

	boxes[0].start = 0;
	boxes[0].end = n_colors;

	while (n_boxes < num_colors) {
		int best = -1;
		unsigned int best_range = 0;
		int split_channel = 0;

		for (i = 0; i < n_boxes; i++) {
			unsigned int lo[4], hi[4], range;
			int j, k;
			if (boxes[i].end - boxes[i].start < 2) {
				continue;
			}
			for (k = 0; k < 4; k++) {
				lo[k] = 255;
				hi[k] = 0;
			}
			for (j = boxes[i].start; j < boxes[i].end; j++) {
				for (k = 0; k < 4; k++) {
					unsigned int v = qoi_pp_channel_of(&colors[j], k);
					if (v < lo[k]) { lo[k] = v; }
					if (v > hi[k]) { hi[k] = v; }
				}
			}
			{
				static const unsigned int w[4] = {
					QOI_PP_WR, QOI_PP_WG, QOI_PP_WB, QOI_PP_WA
				};
				range = 0;
				for (k = 0; k < 4; k++) {
					unsigned int r = (hi[k] - lo[k]) * w[k];
					if (r > range) {
						range = r;
						split_channel = k;
					}
				}
			}
			if (range > best_range) {
				best_range = range;
				best = i;
			}
		}
		if (best < 0 || best_range == 0) {
			break; /* every remaining box holds a single color */
		}

		/* Split at the weighted median, keeping at least one color on either
		side. */
		qoi_pp_sort_colors(colors + boxes[best].start,
			boxes[best].end - boxes[best].start, split_channel);
		{
			unsigned int total = 0, acc = 0;
			int split = boxes[best].start + 1;
			for (i = boxes[best].start; i < boxes[best].end; i++) {
				total += colors[i].count;
			}
			for (i = boxes[best].start; i < boxes[best].end - 1; i++) {
				acc += colors[i].count;
				if (acc * 2 >= total) {
					split = i + 1;
					break;
				}
			}
			boxes[n_boxes].start = split;
			boxes[n_boxes].end = boxes[best].end;
			boxes[best].end = split;
			n_boxes++;
		}
	}

	for (i = 0; i < n_boxes; i++) {
		unsigned int sum[4];
		unsigned int count = 0;
		int j;
		sum[0] = sum[1] = sum[2] = sum[3] = 0;
		for (j = boxes[i].start; j < boxes[i].end; j++) {
			sum[0] += (unsigned int)colors[j].r * colors[j].count;
			sum[1] += (unsigned int)colors[j].g * colors[j].count;
			sum[2] += (unsigned int)colors[j].b * colors[j].count;
			sum[3] += (unsigned int)colors[j].a * colors[j].count;
			count += colors[j].count;
		}
		if (count == 0) {
			count = 1;
		}
		palette[i][0] = (unsigned char)((sum[0] + count / 2) / count);
		palette[i][1] = (unsigned char)((sum[1] + count / 2) / count);
		palette[i][2] = (unsigned char)((sum[2] + count / 2) / count);
		palette[i][3] = (unsigned char)((sum[3] + count / 2) / count);
	}

	QOI_FREE(colors);

	/* Sort by luma (insertion sort; at most 256 entries). */
	{
		int a, b;
		for (a = 1; a < n_boxes; a++) {
			unsigned char cur[4];
			unsigned int cur_key;
			memcpy(cur, palette[a], 4);
			cur_key = cur[0] * QOI_PP_WR + cur[1] * QOI_PP_WG +
				cur[2] * QOI_PP_WB + cur[3] * QOI_PP_WA;
			b = a - 1;
			while (b >= 0) {
				unsigned int key = palette[b][0] * QOI_PP_WR +
					palette[b][1] * QOI_PP_WG +
					palette[b][2] * QOI_PP_WB +
					palette[b][3] * QOI_PP_WA;
				if (key <= cur_key) {
					break;
				}
				memcpy(palette[b + 1], palette[b], 4);
				b--;
			}
			memcpy(palette[b + 1], cur, 4);
		}
	}

	return n_boxes;
}

/* Weighted squared distance between an RGBA pixel and a palette entry. */
static unsigned int qoi_pp_distance(
	const unsigned char rgba[4], const unsigned char *pal
) {
	int dr = (int)rgba[0] - pal[0];
	int dg = (int)rgba[1] - pal[1];
	int db = (int)rgba[2] - pal[2];
	int da = (int)rgba[3] - pal[3];
	return (unsigned int)(
		dr * dr * QOI_PP_WR + dg * dg * QOI_PP_WG +
		db * db * QOI_PP_WB + da * da * QOI_PP_WA
	);
}

/* Nearest and second nearest palette entry; the dither stage needs both. */
static void qoi_pp_nearest2(
	const unsigned char rgba[4], unsigned char palette[256][4],
	int n_colors, int *best, int *second,
	unsigned int *d_best, unsigned int *d_second
) {
	int i;
	*best = 0;
	*second = 0;
	*d_best = 0xffffffffu;
	*d_second = 0xffffffffu;
	for (i = 0; i < n_colors; i++) {
		unsigned int d = qoi_pp_distance(rgba, palette[i]);
		if (d < *d_best) {
			*d_second = *d_best;
			*second = *best;
			*d_best = d;
			*best = i;
		}
		else if (d < *d_second) {
			*d_second = d;
			*second = i;
		}
	}
}

/* Causal predictor: the index a delta residual refers to. left/up/up_left
are the direct neighbours, left2/up2 the pixels two positions away. */
static int qoi_pp_predict(
	int predictor, int left, int up, int up_left, int left2, int up2
) {
	if (predictor == QOI_PP_PRED_LEFT) {
		return left;
	}
	if (predictor == QOI_PP_PRED_UP) {
		return up;
	}
	if (predictor == QOI_PP_PRED_PAETH) {
		int p = left + up - up_left;
		int pa = p > left ? p - left : left - p;
		int pb = p > up ? p - up : up - p;
		int pc = p > up_left ? p - up_left : up_left - p;
		if (pa <= pb && pa <= pc) {
			return left;
		}
		if (pb <= pc) {
			return up;
		}
		return up_left;
	}
	if (predictor == QOI_PP_PRED_LL) {
		return left2;
	}
	if (predictor == QOI_PP_PRED_UU) {
		return up2;
	}
	return 0;
}

/* Map a signed difference into 0..255 (zigzag), so small changes stay small. */
static unsigned char qoi_pp_zigzag(int diff) {
	unsigned int d = (unsigned int)diff & 0xff;
	if (d < 128) {
		return (unsigned char)(d * 2);
	}
	return (unsigned char)((256 - d) * 2 - 1);
}

static int qoi_pp_unzigzag(unsigned int raw) {
	return (raw & 1) ? -(int)((raw + 1) / 2) : (int)(raw / 2);
}

/* Neighbouring palette indices for the delta stage. Outside the image a
neighbour falls back to the closest available one (left borrows from above,
above borrows from the left, the far reaches borrow the direct neighbour and
the top left corner predicts 0). Encoder and decoder must agree, so these
rules live here only. */
static void qoi_pp_neighbors(
	const unsigned char *indexes, int width, int i, int stride,
	int *left, int *up, int *up_left, int *left2, int *up2
) {
	int x = i % width;
	int l = (x >= stride) ? indexes[i - stride] :
		((i >= stride * width) ? indexes[i - stride * width] : 0);
	int u = (i >= stride * width) ? indexes[i - stride * width] : l;
	*left = l;
	*up = u;
	*up_left = (x >= stride && i >= stride * width) ?
		indexes[i - stride * width - stride] : l;
	*left2 = (x >= 2 * stride) ? indexes[i - 2 * stride] : l;
	*up2 = (i >= 2 * stride * width) ? indexes[i - 2 * stride * width] : u;
}

/* Build the 3 channel index image. With delta the index bytes become zigzag
residuals against the selected predictor. */
static void qoi_pp_build_index_image(
	const unsigned char *indexes, int width, int height, int delta,
	int predictor, int stride, unsigned char *gray
) {
	int px_count = width * height;
	int i;
	for (i = 0; i < px_count; i++) {
		int left, up, up_left, left2, up2;
		int value = indexes[i];
		unsigned char v = (unsigned char)value;
		if (delta && predictor != QOI_PP_PRED_NONE) {
			int predicted;
			qoi_pp_neighbors(indexes, width, i, stride,
				&left, &up, &up_left, &left2, &up2);
			predicted = qoi_pp_predict(predictor, left, up, up_left,
				left2, up2);
			v = qoi_pp_zigzag(value - predicted);
		}
		gray[i * 3 + 0] = v;
		gray[i * 3 + 1] = v;
		gray[i * 3 + 2] = v;
	}
}

void *qoi_preprocess_encode(const void *data, const qoi_preprocess_desc *desc,
	int *out_len, qoi_preprocess_stats *stats
) {
	unsigned char *indexes = NULL;
	unsigned char *gray = NULL;
	unsigned char *payload = NULL;
	unsigned char *best_payload = NULL;
	unsigned char *out = NULL;
	unsigned char palette[256][4];
	qoi_desc qdesc;
	qoi_desc pdesc;
	int px_count, n_colors, payload_len = 0, best_len = 0, total, p;
	int width, height, channels, stages, predictor, best_predictor;
	int best_stride;
	int i;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}
	if ((desc->stages & ~(QOI_PP_QUANTIZE | QOI_PP_BAYER | QOI_PP_DELTA)) != 0) {
		return NULL;
	}
	if ((desc->stages & QOI_PP_BAYER) && !qoi_pp_valid_bayer(desc->bayer_size)) {
		return NULL;
	}
	if (!(desc->stages & QOI_PP_BAYER) &&
		desc->bayer_size != 0 && !qoi_pp_valid_bayer(desc->bayer_size)) {
		return NULL;
	}
	if (desc->num_colors < 1 || desc->num_colors > 256) {
		return NULL;
	}

	width = (int)desc->width;
	height = (int)desc->height;
	channels = desc->channels;
	px_count = width * height;

	qdesc.width = desc->width;
	qdesc.height = desc->height;
	qdesc.channels = desc->channels;
	qdesc.colorspace = desc->colorspace;

	/* No preprocessing: the original encoder produces the whole output. */
	if (desc->stages == 0) {
		void *encoded = qoi_encode(data, &qdesc, out_len);
		if (encoded && stats) {
			stats->palette_size = 0;
			stats->header_size = 0;
			stats->payload_size = (unsigned int)*out_len;
		}
		return encoded;
	}

	/* Dithering and delta coding work on palette indices, so either of them
	implies the quantization stage. The effective mask is what gets stored. */
	stages = desc->stages;
	if (stages & (QOI_PP_BAYER | QOI_PP_DELTA)) {
		stages |= QOI_PP_QUANTIZE;
	}
	best_predictor = QOI_PP_PRED_NONE;
	best_stride = 1;

	n_colors = qoi_pp_build_palette(
		(const unsigned char *)data, px_count, channels,
		desc->num_colors, palette);
	if (n_colors == 0) {
		return NULL;
	}

	indexes = (unsigned char *)QOI_MALLOC((size_t)(unsigned int)px_count);
	gray = (unsigned char *)QOI_MALLOC((size_t)(unsigned int)px_count * 3);
	if (!indexes || !gray) {
		QOI_FREE(indexes);
		QOI_FREE(gray);
		return NULL;
	}

	for (i = 0; i < px_count; i++) {
		const unsigned char *pixel = (const unsigned char *)data + i * channels;
		unsigned char rgba[4];
		int best, second;
		unsigned int d_best, d_second;

		rgba[0] = pixel[0];
		rgba[1] = pixel[1];
		rgba[2] = pixel[2];
		rgba[3] = (channels == 4) ? pixel[3] : 255;

		qoi_pp_nearest2(rgba, palette, n_colors, &best, &second,
			&d_best, &d_second);

		if ((stages & QOI_PP_BAYER) && d_second != 0xffffffffu && d_second != 0) {
			/* Switch to the second nearest entry where the matrix threshold
			says so. The threshold sits at the point where the two colors would
			balance, so the average of an area reproduces the exact color. */
			unsigned int s1 = qoi_pp_isqrt(d_best);
			unsigned int s2 = qoi_pp_isqrt(d_second);
			/* Threshold in 0..255 at which the second nearest entry is
			picked. The 16x16 matrix stores all of them, so its top left
			s x s corner yields exactly the s*s levels this comparison
			needs. */
			unsigned int threshold = (s1 + s2) ? 255u * s1 / (s1 + s2) : 0;
			unsigned int bayer = qoi_pp_bayer16[
				(unsigned int)(i / width) % desc->bayer_size
			][
				(unsigned int)(i % width) % desc->bayer_size
			];
			if (bayer < threshold) {
				best = second;
			}
		}
		indexes[i] = (unsigned char)best;
	}

	pdesc.width = desc->width;
	pdesc.height = desc->height;
	pdesc.channels = 3;
	pdesc.colorspace = desc->colorspace;

	if (stages & QOI_PP_DELTA) {
		/* Try every predictor on every step that can line up with the
		dither tile (plus step 1), and keep the smallest payload. The
		"no prediction" payload is already in hand, so this can only win.
		The first candidate is always the plain index image, which is why
		the predictor search starts with the untransformed data. */
		int stride_max = (stages & QOI_PP_BAYER) ? desc->bayer_size : 16;
		int stride;

		qoi_pp_build_index_image(indexes, width, height, 0, 0, 0, gray);
		payload = (unsigned char *)qoi_encode(gray, &pdesc, &payload_len);
		if (!payload) {
			QOI_FREE(indexes);
			QOI_FREE(gray);
			return NULL;
		}
		best_payload = payload;
		best_len = payload_len;

		for (stride = 1; stride <= stride_max; stride *= 2) {
			for (predictor = 1; predictor < QOI_PP_PRED_COUNT; predictor++) {
				qoi_pp_build_index_image(indexes, width, height, 1,
					predictor, stride, gray);
				payload = (unsigned char *)qoi_encode(gray, &pdesc,
					&payload_len);
				if (!payload) {
					QOI_FREE(best_payload);
					QOI_FREE(indexes);
					QOI_FREE(gray);
					return NULL;
				}
				if (payload_len < best_len) {
					QOI_FREE(best_payload);
					best_payload = payload;
					best_len = payload_len;
					best_predictor = predictor;
					best_stride = stride;
				}
				else {
					QOI_FREE(payload);
				}
			}
		}
		payload = best_payload;
		payload_len = best_len;
	}
	else {
		qoi_pp_build_index_image(indexes, width, height, 0, 0, 0, gray);
		payload = (unsigned char *)qoi_encode(gray, &pdesc, &payload_len);
		if (!payload) {
			QOI_FREE(indexes);
			QOI_FREE(gray);
			return NULL;
		}
	}

	total = QOI_PP_HEADER_SIZE + n_colors * 4 + payload_len;
	out = (unsigned char *)QOI_MALLOC(total);
	if (!out) {
		QOI_FREE(indexes);
		QOI_FREE(gray);
		QOI_FREE(payload);
		return NULL;
	}

	p = 0;
	qoi_write_32(out, &p, QOI_PP_MAGIC);
	qoi_write_32(out, &p, desc->width);
	qoi_write_32(out, &p, desc->height);
	out[p++] = desc->channels;
	out[p++] = desc->colorspace;
	out[p++] = (unsigned char)stages;
	out[p++] = (stages & QOI_PP_BAYER) ? (unsigned char)desc->bayer_size : 0;
	out[p++] = (unsigned char)((n_colors >> 8) & 0xff);
	out[p++] = (unsigned char)(n_colors & 0xff);
	out[p++] = (unsigned char)best_predictor;
	out[p++] = (unsigned char)best_stride;
	qoi_write_32(out, &p, (unsigned int)payload_len);
	for (i = 0; i < n_colors; i++) {
		out[p++] = palette[i][0];
		out[p++] = palette[i][1];
		out[p++] = palette[i][2];
		out[p++] = palette[i][3];
	}
	memcpy(out + p, payload, payload_len);
	p += payload_len;

	QOI_FREE(indexes);
	QOI_FREE(gray);
	QOI_FREE(payload);

	*out_len = p;
	if (stats) {
		stats->palette_size = (unsigned int)n_colors;
		stats->header_size = (unsigned int)(QOI_PP_HEADER_SIZE + n_colors * 4);
		stats->payload_size = (unsigned int)payload_len;
	}
	return out;
}

void *qoi_preprocess_decode(const void *data, int size, qoi_desc *desc,
	int channels
) {
	const unsigned char *bytes;
	const unsigned char *palette;
	unsigned char *gray;
	unsigned char *indexes;
	unsigned char *pixels;
	qoi_desc pdesc;
	int width, height, file_channels, colorspace, stages, bayer_size;
	int n_colors, payload_len, predictor, stride, p, i, px_count, out_channels;
	int probe;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < 4
	) {
		return NULL;
	}

	bytes = (const unsigned char *)data;

	/* Plain QOI data decodes exactly like qoi_decode() does. */
	probe = 0;
	if (qoi_read_32(bytes, &probe) == QOI_MAGIC) {
		return qoi_decode(data, size, desc, channels);
	}

	if (size < QOI_PP_HEADER_SIZE) {
		return NULL;
	}

	p = 0;
	if (qoi_read_32(bytes, &p) != QOI_PP_MAGIC) {
		return NULL;
	}
	width = (int)qoi_read_32(bytes, &p);
	height = (int)qoi_read_32(bytes, &p);
	file_channels = bytes[p++];
	colorspace = bytes[p++];
	stages = bytes[p++];
	bayer_size = bytes[p++];
	n_colors = (bytes[p] << 8) | bytes[p + 1];
	p += 2;
	predictor = bytes[p++];
	stride = bytes[p++];
	payload_len = (int)qoi_read_32(bytes, &p);

	if (
		width <= 0 || height <= 0 ||
		file_channels < 3 || file_channels > 4 ||
		colorspace > 1 ||
		(stages & ~(QOI_PP_QUANTIZE | QOI_PP_BAYER | QOI_PP_DELTA)) != 0 ||
		((stages & (QOI_PP_BAYER | QOI_PP_DELTA)) && !(stages & QOI_PP_QUANTIZE)) ||
		n_colors < 1 || n_colors > 256 ||
		predictor >= QOI_PP_PRED_COUNT ||
		((stages & QOI_PP_DELTA) == 0 && predictor != 0) ||
		(stride != 1 && stride != 2 && stride != 4 && stride != 8 &&
			stride != 16) ||
		((stages & QOI_PP_DELTA) == 0 && stride != 1) ||
		payload_len < (int)(QOI_HEADER_SIZE + sizeof(qoi_padding)) ||
		payload_len > size ||
		(unsigned int)height >= QOI_PIXELS_MAX / (unsigned int)width ||
		(size_t)QOI_PP_HEADER_SIZE + (size_t)n_colors * 4 +
			(size_t)payload_len != (size_t)size
	) {
		return NULL;
	}
	if (stages & QOI_PP_BAYER) {
		if (!qoi_pp_valid_bayer(bayer_size)) {
			return NULL;
		}
	}
	else if (bayer_size != 0) {
		return NULL;
	}

	/* A QOI chunk covers at most 62 pixels, so the payload must be able to
	hold ceil(px_count / 62) chunk bytes. This rejects containers whose
	header promises a huge image with a tiny payload before qoi_decode()
	allocates the pixel buffer. */
	if ((long long)(payload_len - QOI_HEADER_SIZE - (int)sizeof(qoi_padding)) *
		62 < (long long)width * height
	) {
		return NULL;
	}

	palette = bytes + QOI_PP_HEADER_SIZE;

	gray = (unsigned char *)qoi_decode(
		bytes + QOI_PP_HEADER_SIZE + n_colors * 4, payload_len, &pdesc, 3);
	if (!gray) {
		return NULL;
	}
	if (
		(int)pdesc.width != width || (int)pdesc.height != height ||
		pdesc.channels != 3
	) {
		QOI_FREE(gray);
		return NULL;
	}

	px_count = width * height;
	indexes = (unsigned char *)QOI_MALLOC((size_t)(unsigned int)px_count);
	out_channels = (channels == 0) ? file_channels : channels;
	pixels = (unsigned char *)QOI_MALLOC((size_t)(unsigned int)px_count * (unsigned int)out_channels);
	if (!indexes || !pixels) {
		QOI_FREE(gray);
		QOI_FREE(indexes);
		QOI_FREE(pixels);
		return NULL;
	}

	/* Undo the delta prediction first; the predictors need the neighbouring
	indices in their final form, so this is a separate forward pass. */
	for (i = 0; i < px_count; i++) {
		int value = gray[i * 3];
		if (stages & QOI_PP_DELTA) {
			int left, up, up_left, left2, up2;
			if (predictor == QOI_PP_PRED_NONE) {
				value = (int)gray[i * 3];
			}
			else {
				int predicted;
				qoi_pp_neighbors(indexes, width, i, stride,
					&left, &up, &up_left, &left2, &up2);
				predicted = qoi_pp_predict(predictor, left, up, up_left,
					left2, up2);
				value = (predicted +
					qoi_pp_unzigzag((unsigned int)gray[i * 3])) & 0xff;
			}
		}
		indexes[i] = (unsigned char)value;
	}

	for (i = 0; i < px_count; i++) {
		int index = indexes[i];
		if (index >= n_colors) {
			QOI_FREE(gray);
			QOI_FREE(indexes);
			QOI_FREE(pixels);
			return NULL;
		}
		pixels[i * out_channels + 0] = palette[index * 4 + 0];
		pixels[i * out_channels + 1] = palette[index * 4 + 1];
		pixels[i * out_channels + 2] = palette[index * 4 + 2];
		if (out_channels == 4) {
			if (file_channels == 4) {
				pixels[i * out_channels + 3] = palette[index * 4 + 3];
			}
			else {
				pixels[i * out_channels + 3] = 255;
			}
		}
	}

	QOI_FREE(gray);
	QOI_FREE(indexes);

	desc->width = (unsigned int)width;
	desc->height = (unsigned int)height;
	desc->channels = (unsigned char)file_channels;
	desc->colorspace = (unsigned char)colorspace;
	return pixels;
}

#endif /* QOI_IMPLEMENTATION (preprocessing pipeline) */
