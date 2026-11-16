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
- qoi_encode_rle -- run-length preprocess then encode (large flat areas)
- qoi_decode_rle -- decode a run-length preprocessed stream

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


/* --- Run-length preprocessed streams ("qoir") ---

QOI already has a run-length opcode, but it is limited to 62 pixels per
chunk. Images with very large flat areas (pixel art, UI mockups, flat
backgrounds) therefore spend one chunk on every 62 pixels of such an area.
The run-length layer below replaces such an area with two token pixels
before the regular QOI encoder is called.

The preprocessor scans the input pixels in row-major order and finds every
maximal run of identical pixels. A run is preprocessed when its length is
strictly greater than `threshold`; it is then replaced by a run token.
Every other pixel is copied as a literal token. The tokens are encoded with
the unchanged qoi_encode() as an ordinary QOI image of four channels and
height 1, so the original QOI bitstream format, the original encoder and
the original decoder are not modified in any way. The regular QOI image is
wrapped in a small header describing the original image and the threshold.

`threshold` is the largest run length that is left for the regular QOI
encoder. It must be at least 1 and at most QOI_RLE_MAX_THRESHOLD; any other
value, including 0 and negative values, is invalid and makes
qoi_encode_rle() fail.

Stream layout (all multi-byte integers are big endian):

	qoir header (20 bytes)
		char     magic[4];    // "qoir"
		uint32_t width;       // original image width in pixels
		uint32_t height;      // original image height in pixels
		uint8_t  channels;    // 3 = RGB, 4 = RGBA
		uint8_t  colorspace;  // 0 = sRGB, 1 = linear (informative only)
		uint16_t reserved;    // must be 0
		uint32_t threshold;   // largest run length left to the QOI encoder

	an ordinary QOI image ("qoif", 4 channels, "width" = token count,
	height 1, same colorspace as the original image)

Token pixels (r, g, b, a), read in this order:

	(r, g, b, a) with a != 0    literal pixel, emitted once
	(0, 0, 0, 0), (r, g, b, a)  escaped literal, emitted once
	(L>>16, L>>8, L, 0), V      run token: V emitted L times

A run token is a length pixel whose alpha byte is 0 and whose rgb bytes hold
the 24 bit count L, immediately followed by the pixel value V to repeat. It
costs two token pixels, i.e. a few bytes, no matter how long the run is. L
is 1..0xFFFFFF; a longer run is split into consecutive run tokens with the
same V.

The encoding is unambiguous because of these rules, all of which the encoder
applies:

 - A literal pixel whose alpha byte is 0 can be confused with a length
   pixel, so every such literal is escaped as (0,0,0,0) followed by the
   literal itself. A literal (0,0,0,0) is therefore written as (0,0,0,0),
   (0,0,0,0).
 - A length pixel always has a non-zero rgb value, because L >= 1, so
   (0,0,0,0) is never a length pixel. The decoder reads (0,0,0,0) as the
   escape marker and the next token as a literal.
 - A length pixel with non-zero rgb is always followed by a value pixel.
   The value pixel is consumed by the run, even when it is (0,0,0,0), so it
   can never be mistaken for an escape marker.

For 3 channel input the alpha byte of every literal is forced to 255, so no
escape is ever written and without long runs the token stream is exactly
the pixel stream.

qoi_decode_rle() reconstructs the exact original pixels. It returns NULL
for malformed headers, invalid parameters, truncated or inconsistent token
streams, and for streams whose expansion does not produce exactly
width * height pixels. */

#define QOI_RLE_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'r'))
#define QOI_RLE_HEADER_SIZE 20
#define QOI_RLE_MAX_RUN 0xFFFFFF
#define QOI_RLE_MAX_THRESHOLD 400000000

/* Encode raw RGB or RGBA pixels into a run-length preprocessed stream in
memory. Runs longer than `threshold` pixels are replaced by a run token and
the token stream is encoded with the regular QOI encoder.

The function either returns NULL on failure (invalid parameters, an
invalid threshold, or malloc failure) or a pointer to the encoded data on
success. On success the out_len is set to the size in bytes of the encoded
data.

The returned data should be free()d after use. */

void *qoi_encode_rle(const void *data, const qoi_desc *desc, int threshold,
	int *out_len);


/* Decode a run-length preprocessed stream produced by qoi_encode_rle()
from memory. If channels is 0, the number of channels from the stream
header is used. If channels is 3 or 4 the output format will be forced
into this number of channels.

The function either returns NULL on failure (invalid data or malloc
failed) or a pointer to the decoded pixels. On success, the qoi_desc struct
is filled with the description from the stream header.

The returned pixel data should be free()d after use. */

void *qoi_decode_rle(const void *data, int size, qoi_desc *desc, int channels);


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

/* -----------------------------------------------------------------------------
Run-length preprocessing layer ("qoir") */

/* Return 1 if the pixel at px_pos equals the pixel at other_pos. */
static int qoi_rle_pixel_eq(
	const unsigned char *pixels, int px_pos, int other_pos, int channels
) {
	int c;
	for (c = 0; c < channels; c++) {
		if (pixels[px_pos + c] != pixels[other_pos + c]) {
			return 0;
		}
	}
	return 1;
}

/* Append one token pixel and advance the token position. */
static void qoi_rle_put(unsigned char *tokens, int *t,
	unsigned char r, unsigned char g, unsigned char b, unsigned char a
) {
	tokens[(*t)++] = r;
	tokens[(*t)++] = g;
	tokens[(*t)++] = b;
	tokens[(*t)++] = a;
}

/* Return 1 if a literal pixel has to be escaped, i.e. its alpha byte is 0
and would otherwise be read as a run length pixel. */
static int qoi_rle_needs_escape(const unsigned char *p, int channels) {
	return channels == 4 && p[3] == 0;
}

/* Append one literal pixel, escaping it when its alpha byte is 0. */
static void qoi_rle_put_literal(unsigned char *tokens, int *t,
	const unsigned char *p, int channels
) {
	unsigned char a = channels == 4 ? p[3] : 255;

	if (channels == 4 && p[3] == 0) {
		qoi_rle_put(tokens, t, 0, 0, 0, 0);
	}
	qoi_rle_put(tokens, t, p[0], p[1], p[2], a);
}

void *qoi_encode_rle(const void *data, const qoi_desc *desc, int threshold,
	int *out_len
) {
	const unsigned char *pixels;
	unsigned char *tokens, *bytes;
	qoi_desc token_desc;
	int width, height, channels, px_count, px_pos, run_len;
	int token_count, t, segments, seg, seg_len, remaining;
	int inner_len = 0;
	void *inner;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width ||
		threshold < 1 || threshold > QOI_RLE_MAX_THRESHOLD
	) {
		return NULL;
	}

	pixels = (const unsigned char *)data;
	width = (int)desc->width;
	height = (int)desc->height;
	channels = desc->channels;
	px_count = width * height;

	/* First pass: count the tokens. A preprocessed run costs two tokens
	per QOI_RLE_MAX_RUN segment (length and value); every other pixel costs
	one token, or two when it has to be escaped. */
	token_count = 0;
	px_pos = 0;
	while (px_pos < px_count) {
		int base = px_pos * channels;

		run_len = 1;
		while (
			px_pos + run_len < px_count &&
			qoi_rle_pixel_eq(pixels, (px_pos + run_len) * channels,
				base, channels)
		) {
			run_len++;
		}

		if (run_len > threshold) {
			segments = (run_len + QOI_RLE_MAX_RUN - 1) / QOI_RLE_MAX_RUN;
			token_count += segments * 2;
		}
		else {
			int k;
			for (k = 0; k < run_len; k++) {
				token_count += qoi_rle_needs_escape(
					pixels + (size_t)(px_pos + k) * channels,
					channels) ? 2 : 1;
			}
		}

		px_pos += run_len;
	}

	/* The tokens are stored as an ordinary QOI image, one token per pixel,
	so they inherit QOI's pixel limit. */
	if (token_count < 1 || token_count >= (int)QOI_PIXELS_MAX) {
		return NULL;
	}

	tokens = (unsigned char *) QOI_MALLOC((size_t)token_count * 4);
	if (!tokens) {
		return NULL;
	}

	/* Second pass: emit the tokens. */
	t = 0;
	px_pos = 0;
	while (px_pos < px_count) {
		int base = px_pos * channels;

		run_len = 1;
		while (
			px_pos + run_len < px_count &&
			qoi_rle_pixel_eq(pixels, (px_pos + run_len) * channels,
				base, channels)
		) {
			run_len++;
		}

		if (run_len > threshold) {
			remaining = run_len;
			segments = (run_len + QOI_RLE_MAX_RUN - 1) / QOI_RLE_MAX_RUN;
			for (seg = 0; seg < segments; seg++) {
				seg_len = remaining > QOI_RLE_MAX_RUN
					? QOI_RLE_MAX_RUN : remaining;

				/* Length pixel (alpha 0, 24 bit count in rgb) followed
				by the pixel value to repeat. */
				qoi_rle_put(tokens, &t,
					(unsigned char)((seg_len >> 16) & 0xff),
					(unsigned char)((seg_len >>  8) & 0xff),
					(unsigned char)( seg_len        & 0xff), 0);
				qoi_rle_put(tokens, &t,
					pixels[base + 0], pixels[base + 1],
					pixels[base + 2],
					channels == 4 ? pixels[base + 3] : 255);

				remaining -= seg_len;
			}
		}
		else {
			int k;
			for (k = 0; k < run_len; k++) {
				qoi_rle_put_literal(tokens, &t,
					pixels + (size_t)(px_pos + k) * channels, channels);
			}
		}

		px_pos += run_len;
	}

	/* The token stream is encoded with the unmodified QOI encoder. */
	token_desc.width = (unsigned int)token_count;
	token_desc.height = 1;
	token_desc.channels = 4;
	token_desc.colorspace = desc->colorspace;

	inner = qoi_encode(tokens, &token_desc, &inner_len);
	QOI_FREE(tokens);
	if (!inner) {
		return NULL;
	}

	bytes = (unsigned char *) QOI_MALLOC(
		(size_t)QOI_RLE_HEADER_SIZE + (size_t)inner_len
	);
	if (!bytes) {
		QOI_FREE(inner);
		return NULL;
	}

	t = 0;
	qoi_write_32(bytes, &t, QOI_RLE_MAGIC);
	qoi_write_32(bytes, &t, desc->width);
	qoi_write_32(bytes, &t, desc->height);
	bytes[t++] = desc->channels;
	bytes[t++] = desc->colorspace;
	bytes[t++] = 0;
	bytes[t++] = 0;
	qoi_write_32(bytes, &t, (unsigned int)threshold);

	memcpy(bytes + t, inner, (size_t)inner_len);
	t += inner_len;

	QOI_FREE(inner);

	*out_len = t;
	return bytes;
}

void *qoi_decode_rle(const void *data, int size, qoi_desc *desc, int channels) {
	const unsigned char *bytes, *tokens;
	unsigned char *pixels;
	qoi_desc inner_desc;
	unsigned int width, height, threshold;
	unsigned char src_channels, colorspace;
	int p, px_total, out_channels, ti, token_count, out_pos;
	void *inner_pixels;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QOI_RLE_HEADER_SIZE + QOI_HEADER_SIZE + (int)sizeof(qoi_padding)
	) {
		return NULL;
	}

	bytes = (const unsigned char *)data;
	p = 0;

	if (
		qoi_read_32(bytes, &p) != QOI_RLE_MAGIC ||
		(width = qoi_read_32(bytes, &p)) == 0 ||
		(height = qoi_read_32(bytes, &p)) == 0 ||
		(src_channels = bytes[p++]) < 3 || src_channels > 4 ||
		((colorspace = bytes[p++]) > 1) ||
		bytes[p++] != 0 || bytes[p++] != 0 ||
		(threshold = qoi_read_32(bytes, &p)) < 1 ||
		threshold > (unsigned int)QOI_RLE_MAX_THRESHOLD
	) {
		return NULL;
	}

	if (height >= QOI_PIXELS_MAX / width) {
		return NULL;
	}
	px_total = (int)(width * height);

	inner_pixels = qoi_decode(bytes + QOI_RLE_HEADER_SIZE,
		size - QOI_RLE_HEADER_SIZE, &inner_desc, 4);
	if (!inner_pixels) {
		return NULL;
	}

	/* Only the four channel token form is valid: one token per pixel. */
	if (
		inner_desc.channels != 4 ||
		inner_desc.height != 1 ||
		inner_desc.width > (unsigned int)QOI_PIXELS_MAX ||
		inner_desc.width * inner_desc.height >= QOI_PIXELS_MAX
	) {
		QOI_FREE(inner_pixels);
		return NULL;
	}
	token_count = (int)(inner_desc.width * inner_desc.height);

	out_channels = channels == 0 ? src_channels : channels;
	pixels = (unsigned char *) QOI_MALLOC(
		(size_t)px_total * (size_t)out_channels
	);
	if (!pixels) {
		QOI_FREE(inner_pixels);
		return NULL;
	}

	tokens = (const unsigned char *)inner_pixels;

	/* Expand the token stream. A token with alpha 0 is the escape marker
	(0,0,0,0) or a run length; every other token is a literal. */
	ti = 0;
	out_pos = 0;
	while (ti < token_count) {
		const unsigned char *tok = tokens + (size_t)ti * 4;
		unsigned char r, g, b, a;

		if (tok[3] == 0 && tok[0] == 0 && tok[1] == 0 && tok[2] == 0) {
			/* Escape marker: the next token is a literal. */
			if (ti + 1 >= token_count) {
				QOI_FREE(inner_pixels);
				QOI_FREE(pixels);
				return NULL;
			}
			tok = tokens + (size_t)(ti + 1) * 4;
			r = tok[0];
			g = tok[1];
			b = tok[2];
			a = src_channels == 4 ? tok[3] : 255;
			ti += 2;

			if (out_pos >= px_total) {
				QOI_FREE(inner_pixels);
				QOI_FREE(pixels);
				return NULL;
			}
			{
				unsigned char *dst =
					pixels + (size_t)out_pos * out_channels;

				dst[0] = r;
				dst[1] = g;
				dst[2] = b;
				if (out_channels == 4) {
					dst[3] = a;
				}
				out_pos++;
			}
		}
		else if (tok[3] == 0) {
			/* Run length, followed by the pixel value to repeat. */
			unsigned int run;

			if (ti + 1 >= token_count) {
				QOI_FREE(inner_pixels);
				QOI_FREE(pixels);
				return NULL;
			}
			run = ((unsigned int)tok[0] << 16) |
				((unsigned int)tok[1] << 8) | (unsigned int)tok[2];
			if (run < 1 || run > (unsigned int)(px_total - out_pos)) {
				QOI_FREE(inner_pixels);
				QOI_FREE(pixels);
				return NULL;
			}

			tok = tokens + (size_t)(ti + 1) * 4;
			r = tok[0];
			g = tok[1];
			b = tok[2];
			a = src_channels == 4 ? tok[3] : 255;
			ti += 2;

			while (run-- > 0) {
				unsigned char *dst =
					pixels + (size_t)out_pos * out_channels;

				dst[0] = r;
				dst[1] = g;
				dst[2] = b;
				if (out_channels == 4) {
					dst[3] = a;
				}
				out_pos++;
			}
		}
		else {
			/* Literal pixel. */
			if (out_pos >= px_total) {
				QOI_FREE(inner_pixels);
				QOI_FREE(pixels);
				return NULL;
			}
			{
				unsigned char *dst =
					pixels + (size_t)out_pos * out_channels;

				dst[0] = tok[0];
				dst[1] = tok[1];
				dst[2] = tok[2];
				if (out_channels == 4) {
					dst[3] = tok[3];
				}
				out_pos++;
			}
			ti++;
		}
	}

	QOI_FREE(inner_pixels);

	/* The token stream has to describe the image exactly. */
	if (out_pos != px_total) {
		QOI_FREE(pixels);
		return NULL;
	}

	desc->width = width;
	desc->height = height;
	desc->channels = src_channels;
	desc->colorspace = colorspace;

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
