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
QOI-Mask (qoim) - per-block channel-mask coding

An optional, self-contained extension that never touches the regular qoi_*
functions above: regular QOI encoding and decoding are byte-for-byte unaffected.

The image is split into blocks of block_sz x block_sz pixels. For every block
each channel is classified as either
  - constant: all values in the block lie within an inclusive tolerance band
              (max - min <= tolerance); one representative value is stored in
              the block header and the channel contributes no per-pixel bytes,
  - varying:  every value is stored verbatim, one byte per pixel.

A block header holds an 8-bit mask (bit set => channel is varying), followed by
one representative byte for each constant channel, in R,G,B,(A) order. The
per-pixel data of the block stores the varying channels only, interleaved in
channel order, row by row, left to right.

File layout:
  16-byte header: magic "qoim", width (BE32), height (BE32), then four
                  single bytes: channels, block_sz, tolerance, reserved
                  (reserved must be 0). The colorspace is always QOI_SRGB;
                  like regular QOI it is not part of the pixel coding.
  blocks as described above, covering width*height pixels (the last block may
  extend past the image border; out-of-border pixels are not stored)
  8-byte end marker: 0x00 * 7, 0x01

tolerance == 0 is lossless; tolerance > 0 makes constant channels approximate
(the representative value is the rounded mean of the block's channel values),
varying channels are always stored exactly. */

typedef struct {
	unsigned char block_sz;  /* edge length of a square block, 1..255 */
	unsigned char tolerance; /* inclusive constant-band tolerance, 0..255 */
} qoim_options;

/* Reasons reported by qoim_check() / qoim_decode() when a stream is rejected.
The regular qoi_* functions do not use these. */
typedef enum {
	QOIM_OK = 0,
	QOIM_ERR_ARG,        /* invalid function arguments                  */
	QOIM_ERR_MAGIC,      /* bad magic / unsupported version             */
	QOIM_ERR_HEADER,     /* bad width/height/channels/block_sz/options  */
	QOIM_ERR_TRUNCATED,  /* stream ended before all blocks were covered */
	QOIM_ERR_MASK,       /* reserved mask bits set / mask mismatch      */
	QOIM_ERR_ENDMARKER,  /* missing 0x00*7,0x01 end marker              */
	QOIM_ERR_NOMEM       /* allocation failed                           */
} qoim_status;

/* Encode raw RGB/RGBA pixels into a qoim stream. Returns NULL on invalid
arguments or allocation failure; *out_len receives the stream size. The
resulting buffer must be free()d (or QOI_FREE'd) by the caller. */
void *qoim_encode(const void *data, const qoi_desc *desc,
	const qoim_options *opt, int *out_len);

/* Decode a qoim stream. Returns NULL (and sets *status when status is not
NULL) on any malformed input, including a mask that is inconsistent with the
block data and a missing end marker. On success *status is QOIM_OK. The
resulting buffer must be free()d by the caller. */
void *qoim_decode(const void *data, int size, qoi_desc *desc, int channels,
	qoim_status *status);

/* Verify that every block's mask is consistent with its stored data: constant
channels must actually stay within the header's tolerance band and varying
channels must be present for every pixel. Returns QOIM_OK on a consistent
stream, otherwise the first detected violation. Does not allocate. */
qoim_status qoim_check(const void *data, int size);

/* Aggregate mask statistics for an encoded qoim stream. hit_channels /
hit_blocks count constant (masked-off) channel occurrences; the _total
fields receive the number of examinations. Returns QOIM_OK or an error
status. Any of the pointers may be NULL. */
qoim_status qoim_stats(const void *data, int size,
	int *hit_channels, int *total_channels,
	int *hit_blocks, int *total_blocks);


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
QOI-Mask implementation - independent of the regular qoi_* codec above.
Nothing in this section is referenced by qoi_encode/qoi_decode, so the
regular codec stays byte-for-byte identical. */

#define QOIM_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'m'))
#define QOIM_HEADER_SIZE 16
#define QOIM_MASK_R 0x01
#define QOIM_MASK_G 0x02
#define QOIM_MASK_B 0x04
#define QOIM_MASK_A 0x08

static unsigned char qoim_mask_all(int channels) {
	return (unsigned char)(channels == 3 ? 0x07 : 0x0f);
}

/* Validate a qoim stream's structure and, when pixels is not NULL, reconstruct
the image into it (stride = out_channels). When the stat pointers are not
NULL, mask hit counters are accumulated. Every read is bounds checked. */
static qoim_status qoim_walk(const unsigned char *bytes, int size,
	unsigned int width, unsigned int height, int channels, int block_sz,
	int out_channels, unsigned char *pixels,
	int *hit_px_ch, int *total_px_ch,
	int *hit_blocks, int *total_blocks) {
	int p = QOIM_HEADER_SIZE;
	size_t nblocks_x, nblocks_y, nb, bi;
	int s_hit_px = 0, s_full = 0;
	size_t pixel_cap = pixels
		? (size_t)width * height * (size_t)out_channels
		: (size_t)0;

	nblocks_x = ((size_t)width + (size_t)block_sz - 1) / (size_t)block_sz;
	nblocks_y = ((size_t)height + (size_t)block_sz - 1) / (size_t)block_sz;
	nb = nblocks_x * nblocks_y;

	for (bi = 0; bi < nb; bi++) {
		size_t blk_col = bi % nblocks_x;
		size_t blk_row = bi / nblocks_x;
		size_t x0 = blk_col * (size_t)block_sz;
		size_t y0 = blk_row * (size_t)block_sz;
		size_t x1 = x0 + (size_t)block_sz; if (x1 > width)  x1 = width;
		size_t y1 = y0 + (size_t)block_sz; if (y1 > height) y1 = height;
		unsigned char mask, rep[4];
		int c, x, y;

		if (p >= size) {
			return QOIM_ERR_TRUNCATED;
		}
		mask = bytes[p++];
		if ((unsigned char)(mask & (unsigned char)~qoim_mask_all(channels)) != 0) {
			return QOIM_ERR_MASK; /* reserved/unknown mask bits */
		}

		for (c = 0; c < channels; c++) {
			if (!(mask & (1 << c))) {
				if (p >= size) {
					return QOIM_ERR_TRUNCATED;
				}
				rep[c] = bytes[p++];
			}
		}

		if (mask == 0) {
			s_full++;
		}

		for (y = (int)y0; y < (int)y1; y++) {
			for (x = (int)x0; x < (int)x1; x++) {
				size_t out_idx = ((size_t)y * width + x) * (size_t)out_channels;
				if (pixels && out_idx + (size_t)channels > pixel_cap) {
					return QOIM_ERR_TRUNCATED;
				}
				for (c = 0; c < channels; c++) {
					unsigned char v;
					if (mask & (1 << c)) {
						if (p >= size) {
							return QOIM_ERR_TRUNCATED;
						}
						v = bytes[p++];
					}
					else {
						v = rep[c];
						s_hit_px++;
					}
					if (pixels) {
						pixels[out_idx + (size_t)c] = v;
					}
				}
				if (pixels && channels == 3 && out_channels == 4) {
					if (out_idx + 4 > pixel_cap) {
						return QOIM_ERR_TRUNCATED;
					}
					pixels[out_idx + 3] = 255;
				}
			}
		}
	}

	/* The end marker must follow immediately; trailing bytes are rejected. */
	if (p + (int)sizeof(qoi_padding) != size) {
		return QOIM_ERR_ENDMARKER;
	}
	{
		int e;
		for (e = 0; e < (int)sizeof(qoi_padding); e++) {
			if (bytes[p + e] != qoi_padding[e]) {
				return QOIM_ERR_ENDMARKER;
			}
		}
	}

	if (hit_px_ch)   { *hit_px_ch = s_hit_px; }
	if (total_px_ch) { *total_px_ch = (int)((size_t)width * height * channels); }
	if (hit_blocks)  { *hit_blocks = s_full; }
	if (total_blocks){ *total_blocks = (int)nb; }
	return QOIM_OK;
}

void *qoim_encode(const void *data, const qoi_desc *desc,
	const qoim_options *opt, int *out_len) {
	unsigned int width, height;
	int channels, block_sz, tolerance, p = 0;
	size_t total_px, nblocks_x, nblocks_y, nb, max_size, bi;
	unsigned char *bytes;
	const unsigned char *pixels;

	if (
		data == NULL || out_len == NULL || desc == NULL || opt == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		opt->block_sz == 0 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	width = desc->width;
	height = desc->height;
	channels = desc->channels;
	block_sz = opt->block_sz;
	tolerance = opt->tolerance;
	pixels = (const unsigned char *)data;
	total_px = (size_t)width * (size_t)height;
	nblocks_x = ((size_t)width + (size_t)block_sz - 1) / (size_t)block_sz;
	nblocks_y = ((size_t)height + (size_t)block_sz - 1) / (size_t)block_sz;
	nb = nblocks_x * nblocks_y;

	/* Worst case: header + per block (mask + one value per constant channel)
	+ one verbatim byte per pixel per channel + end marker. */
	max_size = (size_t)QOIM_HEADER_SIZE
		+ nb * (1 + (size_t)channels)
		+ total_px * (size_t)channels
		+ sizeof(qoi_padding);

	bytes = (unsigned char *)QOI_MALLOC(max_size);
	if (!bytes) {
		return NULL;
	}

	qoi_write_32(bytes, &p, QOIM_MAGIC);
	qoi_write_32(bytes, &p, width);
	qoi_write_32(bytes, &p, height);
	bytes[p++] = (unsigned char)channels;
	bytes[p++] = (unsigned char)block_sz;
	bytes[p++] = (unsigned char)tolerance;
	bytes[p++] = 0; /* reserved, must be zero */

	for (bi = 0; bi < nb; bi++) {
		size_t blk_col = bi % nblocks_x;
		size_t blk_row = bi / nblocks_x;
		size_t x0 = blk_col * (size_t)block_sz;
		size_t y0 = blk_row * (size_t)block_sz;
		size_t x1 = x0 + (size_t)block_sz; if (x1 > width)  x1 = width;
		size_t y1 = y0 + (size_t)block_sz; if (y1 > height) y1 = height;

		unsigned char mn[4], mx[4], rep[4];
		unsigned char mask = 0;
		unsigned int sum[4];
		size_t cnt;
		int c, x, y;

		for (c = 0; c < channels; c++) {
			mn[c] = 255;
			mx[c] = 0;
			sum[c] = 0;
		}
		cnt = 0;
		for (y = (int)y0; y < (int)y1; y++) {
			for (x = (int)x0; x < (int)x1; x++) {
				const unsigned char *px = pixels +
					((size_t)y * width + x) * (size_t)channels;
				for (c = 0; c < channels; c++) {
					unsigned char v = px[c];
					if (v < mn[c]) mn[c] = v;
					if (v > mx[c]) mx[c] = v;
					sum[c] += v;
				}
				cnt++;
			}
		}

		for (c = 0; c < channels; c++) {
			/* rounded mean, half up */
			rep[c] = (unsigned char)((sum[c] + cnt / 2) / cnt);
			if ((int)mx[c] - (int)mn[c] > tolerance) {
				mask |= (unsigned char)(1 << c);
			}
		}

		bytes[p++] = mask;

		/* constant channels: one representative value each */
		for (c = 0; c < channels; c++) {
			if (!(mask & (1 << c))) {
				bytes[p++] = rep[c];
			}
		}

		/* varying channels: verbatim per-pixel bytes, channel interleaved */
		for (y = (int)y0; y < (int)y1; y++) {
			for (x = (int)x0; x < (int)x1; x++) {
				const unsigned char *px = pixels +
					((size_t)y * width + x) * (size_t)channels;
				for (c = 0; c < channels; c++) {
					if (mask & (1 << c)) {
						bytes[p++] = px[c];
					}
				}
			}
		}

		/* Mask/data consistency guard: a channel advertised as constant must
		have every block value within tolerance of the stored value. The
		classification above guarantees this, but refuse to emit a stream that
		violates its own block header rather than producing bad data. */
		for (c = 0; c < channels; c++) {
			if (!(mask & (1 << c))) {
				for (y = (int)y0; y < (int)y1; y++) {
					for (x = (int)x0; x < (int)x1; x++) {
						unsigned char v = pixels[
							((size_t)y * width + x) * (size_t)channels + c];
						int d = (int)v - (int)rep[c];
						if (d < 0) d = -d;
						if (d > tolerance) {
							QOI_FREE(bytes);
							return NULL;
						}
					}
				}
			}
		}
	}

	{
		int e;
		for (e = 0; e < (int)sizeof(qoi_padding); e++) {
			bytes[p++] = qoi_padding[e];
		}
	}

	if ((size_t)p > max_size) {
		QOI_FREE(bytes);
		return NULL;
	}

	*out_len = p;
	return bytes;
}

void *qoim_decode(const void *data, int size, qoi_desc *desc, int channels,
	qoim_status *status) {
	const unsigned char *bytes;
	unsigned int header_magic, width, height;
	unsigned char file_channels, block_sz, tolerance, reserved;
	int out_channels;
	int rp = 0;
	unsigned char *pixels;
	size_t px_len;
	qoim_status st;

	if (status) { *status = QOIM_ERR_ARG; }
	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QOIM_HEADER_SIZE + (int)sizeof(qoi_padding)
	) {
		return NULL;
	}
	bytes = (const unsigned char *)data;

	header_magic = qoi_read_32(bytes, &rp);
	width = qoi_read_32(bytes, &rp);
	height = qoi_read_32(bytes, &rp);
	file_channels = bytes[rp++];
	block_sz = bytes[rp++];
	tolerance = bytes[rp++];
	reserved = bytes[rp++];

	if (header_magic != QOIM_MAGIC) {
		if (status) { *status = QOIM_ERR_MAGIC; }
		return NULL;
	}
	if (
		width == 0 || height == 0 ||
		file_channels < 3 || file_channels > 4 ||
		block_sz == 0 ||
		reserved != 0 ||
		height >= QOI_PIXELS_MAX / width
	) {
		if (status) { *status = QOIM_ERR_HEADER; }
		return NULL;
	}

	out_channels = (channels == 0) ? file_channels : channels;

	/* The block payload is interleaved in the stream's own channel count.
	Upsampling RGB to RGBA (3 -> 4, filling alpha 255) is supported, but
	dropping the alpha channel from an RGBA stream cannot be done safely on
	this layout, so reject the downconvert explicitly. */
	if (out_channels < file_channels) {
		if (status) { *status = QOIM_ERR_ARG; }
		return NULL;
	}

	/* Validate the whole structure without touching pixel memory first. A
	crafted header (e.g. a block_sz that makes the declared block grid reach
	beyond the truncated payload) must be rejected here rather than cause an
	out-of-bounds write during reconstruction. */
	st = qoim_walk(bytes, size, width, height, file_channels, block_sz,
		out_channels, NULL, NULL, NULL, NULL, NULL);
	if (st != QOIM_OK) {
		if (status) { *status = st; }
		return NULL;
	}

	px_len = (size_t)width * height * (size_t)out_channels;
	pixels = (unsigned char *)QOI_MALLOC(px_len);
	if (!pixels) {
		if (status) { *status = QOIM_ERR_NOMEM; }
		return NULL;
	}

	st = qoim_walk(bytes, size, width, height, file_channels, block_sz,
		out_channels, pixels, NULL, NULL, NULL, NULL);
	if (st != QOIM_OK) {
		QOI_FREE(pixels);
		if (status) { *status = st; }
		return NULL;
	}

	desc->width = width;
	desc->height = height;
	desc->channels = file_channels;
	desc->colorspace = QOI_SRGB;
	(void)tolerance;
	if (status) { *status = QOIM_OK; }
	return pixels;
}

qoim_status qoim_check(const void *data, int size) {
	const unsigned char *bytes;
	unsigned int header_magic, width, height;
	unsigned char file_channels, block_sz, reserved;
	int rp = 0;

	if (data == NULL || size < QOIM_HEADER_SIZE + (int)sizeof(qoi_padding)) {
		return QOIM_ERR_ARG;
	}
	bytes = (const unsigned char *)data;

	header_magic = qoi_read_32(bytes, &rp);
	width = qoi_read_32(bytes, &rp);
	height = qoi_read_32(bytes, &rp);
	file_channels = bytes[rp++];
	block_sz = bytes[rp++];
	rp++; /* tolerance */
	reserved = bytes[rp++];

	if (header_magic != QOIM_MAGIC) { return QOIM_ERR_MAGIC; }
	if (
		width == 0 || height == 0 ||
		file_channels < 3 || file_channels > 4 ||
		block_sz == 0 ||
		reserved != 0 ||
		height >= QOI_PIXELS_MAX / width
	) {
		return QOIM_ERR_HEADER;
	}

	return qoim_walk(bytes, size, width, height, file_channels, block_sz,
		file_channels, NULL, NULL, NULL, NULL, NULL);
}

qoim_status qoim_stats(const void *data, int size,
	int *hit_channels, int *total_channels,
	int *hit_blocks, int *total_blocks) {
	const unsigned char *bytes;
	unsigned int header_magic, width, height;
	unsigned char file_channels, block_sz;
	int rp = 0;
	int hc = 0, tc = 0, hb = 0, tb = 0;
	qoim_status st;

	if (data == NULL || size < QOIM_HEADER_SIZE + (int)sizeof(qoi_padding)) {
		return QOIM_ERR_ARG;
	}
	bytes = (const unsigned char *)data;

	header_magic = qoi_read_32(bytes, &rp);
	width = qoi_read_32(bytes, &rp);
	height = qoi_read_32(bytes, &rp);
	file_channels = bytes[rp++];
	block_sz = bytes[rp++];
	rp += 2; /* tolerance + reserved */

	if (header_magic != QOIM_MAGIC) { return QOIM_ERR_MAGIC; }
	if (
		width == 0 || height == 0 ||
		file_channels < 3 || file_channels > 4 ||
		block_sz == 0 ||
		height >= QOI_PIXELS_MAX / width
	) {
		return QOIM_ERR_HEADER;
	}

	st = qoim_walk(bytes, size, width, height, file_channels, block_sz,
		file_channels, NULL, &hc, &tc, &hb, &tb);
	if (st != QOIM_OK) {
		return st;
	}
	if (hit_channels)   { *hit_channels = hc; }
	if (total_channels) { *total_channels = tc; }
	if (hit_blocks)     { *hit_blocks = hb; }
	if (total_blocks)   { *total_blocks = tb; }
	return QOIM_OK;
}

#endif /* QOI_IMPLEMENTATION */
