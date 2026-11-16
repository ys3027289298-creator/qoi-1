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
- qoi_read                   -- read and decode a QOI file
- qoi_decode                 -- decode the raw bytes of a QOI image from memory
- qoi_write                  -- encode and write a QOI file
- qoi_encode                 -- encode an rgba buffer into a QOI image in memory
- qoi_encode_premultiplied   -- encode premultiplied rgba in a flagged stream
- qoi_decode_premultiplied   -- decode a stream to premultiplied or straight alpha

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


-- Premultiplied alpha

Render pipelines commonly keep pixels with premultiplied alpha, i.e. the
color channels have already been multiplied by the alpha channel. QOI's
default encoding expects straight (un-premultiplied) alpha. Converting to
straight and back on every encode is lossy for low alpha values and wastes
time, so qoi_encode_premultiplied() stores the premultiplied values as they
are and marks the stream with a flag in the header:

	colorspace byte bit 7 = QOI_PREMULTIPLIED (0x80)
	bits 0..6              = the regular colorspace (0 or 1)

A premultiplied stream has channels = 4; a 3 channel image has no alpha to
premultiply, so asking for premultiplied encoding with 3 channels, or
decoding a flagged stream as 3 channels, is a conflict and makes the
functions fail (return NULL). Readers that do not know the flag reject such
streams because the colorspace byte is then larger than 1.

The chunk stream is the regular one, with one extension that is only valid
when the flag is set. The escape chunk

	.- QOI_OP_ALPHA ---------------------------------------.
	|  Byte[0]  |  Byte[1]  |  Byte[2]  |    Byte[3]      |
	|  0x6a     |    op     |  (op arg) |  (alpha delta)  |
	`-----------------------------------------------------`

starts with 0x6a, which is QOI_OP_DIFF with the deltas (0,0,0). A regular
encoder never emits this byte: an all-zero RGB difference with an unchanged
alpha is a run of the previous pixel and is encoded as QOI_OP_RUN, while a
changed alpha is encoded as QOI_OP_RGBA. In a premultiplied stream it means
"predict alpha as a delta to the previous pixel and optionally predict RGB,
too". The second byte selects the RGB part:

	op 0x00..0x3f  alpha delta = op - 32 (range -32..31); RGB unchanged
	op 0x40..0x7f  QOI_OP_DIFF style RGB delta in the low 6 bits; the next
	               byte is the signed alpha delta (range -128..127)
	op 0x80..0xbf  QOI_OP_LUMA style RGB delta: low 6 bits are vg, the next
	               byte holds vg_r/vg_b, the last byte is the signed alpha
	               delta
	op 0xc0..0xff  reserved; a decoder rejects it

All chunks are still byte aligned. The escape gives alpha-only changes 2
bytes instead of 5 and combined RGB+alpha changes 3 or 4 bytes instead of
5, which is profitable because premultiplied color deltas scale with alpha
and smooth alpha ramps are common. Fully transparent premultiplied pixels
are all (0,0,0,0), so large transparent regions collapse into QOI_OP_RUN
chunks even when the straight color under alpha 0 was arbitrary.

qoi_decode_premultiplied() either returns the stored premultiplied values
unchanged (un_premultiply = 0) or converts them back to straight alpha
(un_premultiply != 0). The conversion is

	straight = (premultiplied * 255 + alpha / 2) / alpha   (alpha > 0)
	straight = 0                                            (alpha = 0)

Both modes derive from the same decoded pixels, so the two outputs are
consistent by construction. Note that the conversion back to straight is
not always exact for very small alpha values: 8 bit premultiplied alpha
simply has fewer distinct color values where alpha is small. Callers that
care about the exact original colors should stay in premultiplied space.

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

/* Flag in the colorspace header byte of a premultiplied-alpha stream. See
"-- Premultiplied alpha" above for the stream extension. */
#define QOI_PREMULTIPLIED 0x80

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


/* Encode a premultiplied-alpha RGBA buffer into a QOI image in memory.

The input must have 4 channels: the values are stored as given, without any
conversion to straight alpha, and the stream is marked with
QOI_PREMULTIPLIED in its header. desc->channels must be 4; a 3 channel
description is rejected as a conflict. The image pixels themselves must
also be premultiplied (color <= alpha for every pixel). If any pixel has a
color channel larger than its alpha channel the input is not a valid
premultiplied image. Such pixels are not rejected, because that would make
the function dependent on the whole image, but the result of converting
them back to straight alpha is meaningless - alpha == 0 pixels are the
common case and are handled by storing (0,0,0,0).

The function either returns NULL on failure (invalid parameters, 3
channels, or malloc failed) or a pointer to the encoded data on success.
On success the out_len is set to the size in bytes of the encoded data.

The returned qoi data should be free()d after use. */

void *qoi_encode_premultiplied(const void *data, const qoi_desc *desc,
	int *out_len);


/* Decode a QOI image that may carry the QOI_PREMULTIPLIED flag from memory.

Streams without the flag decode exactly like qoi_decode(). Streams with
the flag may contain the QOI_OP_ALPHA extension chunk, which qoi_decode()
rejects, and hence must be decoded with this function. desc->colorspace is
filled with the regular colorspace, with the flag masked out.

If channels is 0 or 4 the output has 4 channels. If channels is 3 the
function fails for a flagged stream, because a premultiplied image can not
be represented without its alpha channel; for an unflagged stream 3
channels behave like qoi_decode().

With un_premultiply == 0 the stored premultiplied values are returned
unchanged. Otherwise every pixel is converted back to straight alpha as
described in "-- Premultiplied alpha"; pixels with alpha 0 become
(0,0,0,0).

The function either returns NULL on failure (invalid data, invalid
parameters, a channel conflict, or malloc failed) or a pointer to the
decoded pixels. On success, the qoi_desc struct is filled with the
description from the file header.

The returned pixel data should be free()d after use. */

void *qoi_decode_premultiplied(const void *data, int size, qoi_desc *desc,
	int channels, int un_premultiply);


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
Premultiplied alpha support.

QOI_OP_ALPHA is an escape chunk that is only valid in streams with the
QOI_PREMULTIPLIED flag set. It starts with the byte QOI_OP_DIFF would use for
a zero rgb difference (0x6a); the regular encoder never emits that byte,
because an unchanged pixel is a run and an unchanged rgb with a changed alpha
is QOI_OP_RGBA. The prediction exploits two properties of premultiplied
data: color and alpha move together (so a color delta can ride along with the
alpha delta) and fully transparent pixels are all zero. */

#define QOI_OP_ALPHA  0x6a

/* Encode a premultiplied RGBA buffer. See the declaration for details. */
void *qoi_encode_premultiplied(const void *data, const qoi_desc *desc,
	int *out_len
) {
	int i, max_size, p, run;
	int px_len, px_end, px_pos;
	unsigned char *bytes;
	const unsigned char *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px, px_prev;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels != 4 || /* no alpha to premultiply: conflict */
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	max_size =
		desc->width * desc->height * 5 +
		QOI_HEADER_SIZE + sizeof(qoi_padding);

	p = 0;
	bytes = (unsigned char *) QOI_MALLOC(max_size);
	if (!bytes) {
		return NULL;
	}

	qoi_write_32(bytes, &p, QOI_MAGIC);
	qoi_write_32(bytes, &p, desc->width);
	qoi_write_32(bytes, &p, desc->height);
	bytes[p++] = 4;
	bytes[p++] = desc->colorspace | QOI_PREMULTIPLIED;

	pixels = (const unsigned char *)data;

	QOI_ZEROARR(index);

	run = 0;
	px_prev.rgba.r = 0;
	px_prev.rgba.g = 0;
	px_prev.rgba.b = 0;
	px_prev.rgba.a = 255;
	px = px_prev;

	px_len = desc->width * desc->height * 4;
	px_end = px_len - 4;

	for (px_pos = 0; px_pos < px_len; px_pos += 4) {
		px.rgba.r = pixels[px_pos + 0];
		px.rgba.g = pixels[px_pos + 1];
		px.rgba.b = pixels[px_pos + 2];
		px.rgba.a = pixels[px_pos + 3];

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

				{
					signed char vr = px.rgba.r - px_prev.rgba.r;
					signed char vg = px.rgba.g - px_prev.rgba.g;
					signed char vb = px.rgba.b - px_prev.rgba.b;
					signed char va = px.rgba.a - px_prev.rgba.a;

					signed char vg_r = vr - vg;
					signed char vg_b = vb - vg;

					if (va == 0) {
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
					else if (
						vr == 0 && vg == 0 && vb == 0 &&
						va > -33 && va < 32
					) {
						/* Alpha only: 2 bytes instead of 5. */
						bytes[p++] = QOI_OP_ALPHA;
						bytes[p++] = va + 32;
					}
					else if (
						vr > -3 && vr < 2 &&
						vg > -3 && vg < 2 &&
						vb > -3 && vb < 2
					) {
						/* Small color delta plus full alpha delta. */
						bytes[p++] = QOI_OP_ALPHA;
						bytes[p++] = QOI_OP_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2);
						bytes[p++] = va;
					}
					else if (
						vg_r > -9 && vg_r < 8 &&
						vg   > -33 && vg   < 32 &&
						vg_b > -9 && vg_b < 8
					) {
						/* Green-based color delta plus full alpha delta. */
						bytes[p++] = QOI_OP_ALPHA;
						bytes[p++] = QOI_OP_LUMA | (vg + 32);
						bytes[p++] = (vg_r + 8) << 4 | (vg_b + 8);
						bytes[p++] = va;
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
		}
		px_prev = px;
	}

	for (i = 0; i < (int)sizeof(qoi_padding); i++) {
		bytes[p++] = qoi_padding[i];
	}

	*out_len = p;
	return bytes;
}

/* Convert one 8 bit premultiplied channel back to straight alpha. */
static unsigned char qoi_unpremultiply(unsigned char c, unsigned char a) {
	int v;
	if (a == 0) {
		return 0;
	}
	v = (c * 255 + a / 2) / a;
	return (unsigned char)(v > 255 ? 255 : v);
}

/* Decode a QOI image, accepting the premultiplied-alpha extension. See the
declaration for details. */
void *qoi_decode_premultiplied(const void *data, int size, qoi_desc *desc,
	int channels, int un_premultiply
) {
	const unsigned char *bytes;
	unsigned int header_magic;
	unsigned char *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px;
	int px_len, chunks_len, px_pos;
	int p = 0, run = 0, premultiplied;

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
	premultiplied = (bytes[p] & QOI_PREMULTIPLIED) != 0;
	desc->colorspace = bytes[p++] & (unsigned char)~QOI_PREMULTIPLIED;

	if (
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		header_magic != QOI_MAGIC ||
		desc->height >= QOI_PIXELS_MAX / desc->width ||
		(premultiplied && desc->channels != 4) /* flag vs channels */
	) {
		return NULL;
	}

	if (channels == 0) {
		channels = desc->channels;
	}

	/* A premultiplied image can only be returned with its alpha channel. */
	if (premultiplied && channels != 4) {
		return NULL;
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
				if (p + 3 > chunks_len) { goto fail; }
				px.rgba.r = bytes[p++];
				px.rgba.g = bytes[p++];
				px.rgba.b = bytes[p++];
			}
			else if (b1 == QOI_OP_RGBA) {
				if (p + 4 > chunks_len) { goto fail; }
				px.rgba.r = bytes[p++];
				px.rgba.g = bytes[p++];
				px.rgba.b = bytes[p++];
				px.rgba.a = bytes[p++];
			}
			else if (premultiplied && b1 == QOI_OP_ALPHA) {
				int op;

				if (p + 1 > chunks_len) { goto fail; }
				op = bytes[p++];

				if (op < 0x40) {
					px.rgba.a += (unsigned char)(op - 32);
				}
				else if (op < 0x80) {
					if (p + 1 > chunks_len) { goto fail; }
					px.rgba.r += ((op >> 4) & 0x03) - 2;
					px.rgba.g += ((op >> 2) & 0x03) - 2;
					px.rgba.b += ( op       & 0x03) - 2;
					px.rgba.a += bytes[p++];
				}
				else if (op < 0xc0) {
					int b3, vg;
					if (p + 2 > chunks_len) { goto fail; }
					b3 = bytes[p++];
					vg = (op & 0x3f) - 32;
					px.rgba.r += vg - 8 + ((b3 >> 4) & 0x0f);
					px.rgba.g += vg;
					px.rgba.b += vg - 8 +  (b3       & 0x0f);
					px.rgba.a += bytes[p++];
				}
				else {
					goto fail;
				}
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
				int b2;
				if (p + 1 > chunks_len) { goto fail; }
				b2 = bytes[p++];
				{
					int vg = (b1 & 0x3f) - 32;
					px.rgba.r += vg - 8 + ((b2 >> 4) & 0x0f);
					px.rgba.g += vg;
					px.rgba.b += vg - 8 +  (b2       & 0x0f);
				}
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

	if (premultiplied && un_premultiply) {
		for (px_pos = 0; px_pos < px_len; px_pos += 4) {
			unsigned char a = pixels[px_pos + 3];
			pixels[px_pos + 0] = qoi_unpremultiply(pixels[px_pos + 0], a);
			pixels[px_pos + 1] = qoi_unpremultiply(pixels[px_pos + 1], a);
			pixels[px_pos + 2] = qoi_unpremultiply(pixels[px_pos + 2], a);
		}
	}

	return pixels;

fail:
	QOI_FREE(pixels);
	return NULL;
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
