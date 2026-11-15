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


/* Multi-frame container ("qoic"): a sequence of independently encoded QOI
images, stored in one file or buffer. Each frame keeps its own pixel data
and a display duration (delay) in milliseconds. The frames are played back
in order; the start time of frame k is the sum of the delays of all frames
before it.

Container layout (all multi-byte integers are big endian):

	container header (8 bytes)
		char     magic[4];   // magic bytes "qoic"
		uint32_t frame_count;

	per frame, repeated frame_count times:
		frame header (8 bytes)
			uint32_t size;        // size of the QOI payload in bytes
			uint32_t delay_ms;    // display duration in milliseconds
		qoi payload (size bytes) // a complete, standalone QOI image

Every frame is encoded with the regular qoi_encode() function, so the
single-frame encoding is unchanged and each frame can also be decoded on its
own with qoi_decode(). */

typedef struct {
	const void *data;     /* raw pixel data of one frame */
	const qoi_desc *desc; /* width/height/channels/colorspace of the frame */
	int delay_ms;         /* display duration of the frame in milliseconds */
} qoi_frame;


/* Encode a sequence of frames into a multi-frame container in memory. The
qoi_desc describes the pixel data of every frame. Each frame additionally
carries its own qoi_desc, which must match the common description; frames
with inconsistent width, height, channels or colorspace are reported as
failure. frame_count must be greater than 0 and every frame's delay_ms must
be greater than or equal to 0; a negative delay or a frame_count of 0 is
reported as failure.

The function either returns NULL on failure (invalid parameters or malloc
failed) or a pointer to the encoded container on success. On success the
out_len is set to the size in bytes of the encoded container.

The returned data should be free()d after use. */
void *qoi_container_encode(const qoi_frame *frames, int frame_count,
	const qoi_desc *desc, int *out_len);


/* Read the common description and the number of frames of a multi-frame
container. Returns 0 on failure (invalid data) or the number of frames on
success. On success the qoi_desc struct is filled with the width, height,
channels and colorspace shared by all frames. */
int qoi_container_info(const void *data, int size, qoi_desc *desc);


/* Read the description and delay of a single frame, without decoding its
pixels. index is zero based and must be smaller than the frame count
reported by qoi_container_info().

Returns 0 on failure (invalid data or an out of range index) or 1 on
success. On success the qoi_desc struct is filled with the frame's
description and *delay_ms (if not NULL) with the frame's delay. */
int qoi_container_frame_info(const void *data, int size, int index,
	qoi_desc *desc, unsigned int *delay_ms);


/* Decode all frames of a multi-frame container. If channels is 0, the
number of channels from the file header is used. If channels is 3 or 4 the
output format is forced into this number of channels.

The function either returns NULL on failure (invalid data or malloc failed)
or an array of frame_count pixel buffers on success; the qoi_desc struct is
filled with the description shared by all frames. Each buffer and the array
itself have to be free()d after use. */
void **qoi_container_decode(const void *data, int size, qoi_desc *desc,
	int channels);


/* Decode only the frame at the given zero based index, skipping all frames
before it. The pixels produced are identical to decoding the whole
container and then taking the frame at the same index.

The function either returns NULL on failure (invalid data, an out of range
index or malloc failed) or a pointer to the decoded pixels. On success the
qoi_desc struct is filled with the frame's description and *delay_ms (if
not NULL) with the frame's delay.

The returned pixel data should be free()d after use. */
void *qoi_container_decode_frame(const void *data, int size, int index,
	qoi_desc *desc, int channels, unsigned int *delay_ms);


#ifdef __cplusplus
}
#endif
#endif /* QOI_H */


/* -----------------------------------------------------------------------------
Implementation */

#ifdef QOI_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>
#include <limits.h>

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

#define QOI_CONTAINER_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'c'))
#define QOI_CONTAINER_HEADER_SIZE 8
#define QOI_FRAME_HEADER_SIZE 8

/* Walk the frame directory of a container. When offsets is not NULL it is
filled with the payload offset, payload size and delay of every frame.
Returns the number of frames on success or 0 if the data is not a valid
container. */
static int qoi_container_scan(const unsigned char *bytes, int size,
	int *offsets, int *sizes, unsigned int *delays)
{
	unsigned int magic, frame_count;
	int pos, i;

	if (
		bytes == NULL ||
		size < QOI_CONTAINER_HEADER_SIZE
	) {
		return 0;
	}

	pos = 0;
	magic = qoi_read_32(bytes, &pos);
	frame_count = qoi_read_32(bytes, &pos);

	if (magic != QOI_CONTAINER_MAGIC || frame_count == 0) {
		return 0;
	}

	for (i = 0; i < (int)frame_count; i++) {
		unsigned int frame_size, delay_ms;
		long long next_pos;

		if (pos + QOI_FRAME_HEADER_SIZE > size) {
			return 0;
		}

		frame_size = qoi_read_32(bytes, &pos);
		delay_ms = qoi_read_32(bytes, &pos);

		if (frame_size == 0) {
			return 0;
		}

		next_pos = (long long)pos + (long long)frame_size;
		if (next_pos > size) {
			return 0;
		}

		if (offsets != NULL) {
			offsets[i] = pos;
			sizes[i] = (int)frame_size;
		}
		if (delays != NULL) {
			delays[i] = delay_ms;
		}

		pos = (int)next_pos;
	}

	if (pos != size) {
		return 0;
	}

	return (int)frame_count;
}

/* Read and validate the 14 byte header of an embedded QOI payload without
decoding any pixels. Returns 1 on success and fills desc, or 0 on failure. */
static int qoi_container_read_payload_header(const unsigned char *bytes,
	int payload_size, qoi_desc *desc)
{
	unsigned int magic;
	int p = 0;

	if (payload_size < QOI_HEADER_SIZE + (int)sizeof(qoi_padding)) {
		return 0;
	}

	magic = qoi_read_32(bytes, &p);
	desc->width = qoi_read_32(bytes, &p);
	desc->height = qoi_read_32(bytes, &p);
	desc->channels = bytes[p++];
	desc->colorspace = bytes[p++];

	return (
		magic == QOI_MAGIC &&
		desc->width != 0 && desc->height != 0 &&
		desc->channels >= 3 && desc->channels <= 4 &&
		desc->colorspace <= 1 &&
		desc->height < QOI_PIXELS_MAX / desc->width
	);
}

void *qoi_container_encode(const qoi_frame *frames, int frame_count,
	const qoi_desc *desc, int *out_len)
{
	int i, p, total_size;
	long long required;
	unsigned char *bytes;
	int *frame_sizes;
	void **frame_payloads;

	if (
		frames == NULL || desc == NULL || out_len == NULL ||
		frame_count <= 0
	) {
		return NULL;
	}

	/* Encode and validate every frame before writing the container. All
	frames must carry pixel data and share the geometry of the common
	qoi_desc. */
	required = QOI_CONTAINER_HEADER_SIZE;
	frame_sizes = (int *) QOI_MALLOC((size_t)frame_count * sizeof(int));
	frame_payloads = (void **) QOI_MALLOC((size_t)frame_count * sizeof(void *));
	if (!frame_sizes) {
		return NULL;
	}
	if (!frame_payloads) {
		QOI_FREE(frame_sizes);
		return NULL;
	}

	for (i = 0; i < frame_count; i++) {
		qoi_desc frame_desc;
		void *encoded;
		int encoded_len;

		if (
			frames[i].data == NULL || frames[i].desc == NULL ||
			frames[i].delay_ms < 0 ||
			frames[i].desc->width != desc->width ||
			frames[i].desc->height != desc->height ||
			frames[i].desc->channels != desc->channels ||
			frames[i].desc->colorspace != desc->colorspace
		) {
			int j;
			for (j = 0; j < i; j++) {
				QOI_FREE(frame_payloads[j]);
			}
			QOI_FREE(frame_payloads);
			QOI_FREE(frame_sizes);
			return NULL;
		}

		/* qoi_encode() rejects zero dimensions, bad channels, etc. */
		encoded = qoi_encode(frames[i].data, desc, &encoded_len);
		if (encoded == NULL) {
			int j;
			for (j = 0; j < i; j++) {
				QOI_FREE(frame_payloads[j]);
			}
			QOI_FREE(frame_payloads);
			QOI_FREE(frame_sizes);
			return NULL;
		}

		/* Every payload is a standalone QOI image. qoi_encode() already
		guarantees the header matches desc; reading the header back just
		verifies the payload starts with a well formed QOI header. */
		if (!qoi_container_read_payload_header(
			(const unsigned char *)encoded, encoded_len, &frame_desc)) {
			int j;
			for (j = 0; j < i; j++) {
				QOI_FREE(frame_payloads[j]);
			}
			QOI_FREE(frame_payloads);
			QOI_FREE(encoded);
			QOI_FREE(frame_sizes);
			return NULL;
		}

		if (
			frame_desc.width != desc->width ||
			frame_desc.height != desc->height ||
			frame_desc.channels != desc->channels ||
			frame_desc.colorspace != desc->colorspace
		) {
			int j;
			for (j = 0; j < i; j++) {
				QOI_FREE(frame_payloads[j]);
			}
			QOI_FREE(frame_payloads);
			QOI_FREE(encoded);
			QOI_FREE(frame_sizes);
			return NULL;
		}

		frame_payloads[i] = encoded;
		frame_sizes[i] = encoded_len;
		required += QOI_FRAME_HEADER_SIZE + (long long)encoded_len;

		if (required > (long long)INT_MAX) {
			int j;
			for (j = 0; j <= i; j++) {
				QOI_FREE(frame_payloads[j]);
			}
			QOI_FREE(frame_payloads);
			QOI_FREE(frame_sizes);
			return NULL;
		}
	}

	total_size = (int)required;
	bytes = (unsigned char *) QOI_MALLOC(total_size);
	if (!bytes) {
		for (i = 0; i < frame_count; i++) {
			QOI_FREE(frame_payloads[i]);
		}
		QOI_FREE(frame_payloads);
		QOI_FREE(frame_sizes);
		return NULL;
	}

	p = 0;
	qoi_write_32(bytes, &p, QOI_CONTAINER_MAGIC);
	qoi_write_32(bytes, &p, (unsigned int)frame_count);

	for (i = 0; i < frame_count; i++) {
		qoi_write_32(bytes, &p, (unsigned int)frame_sizes[i]);
		qoi_write_32(bytes, &p, (unsigned int)frames[i].delay_ms);
		memcpy(bytes + p, frame_payloads[i], frame_sizes[i]);
		p += frame_sizes[i];
		QOI_FREE(frame_payloads[i]);
	}

	QOI_FREE(frame_payloads);
	QOI_FREE(frame_sizes);

	if (p != total_size) {
		QOI_FREE(bytes);
		return NULL;
	}

	*out_len = total_size;
	return bytes;
}

int qoi_container_info(const void *data, int size, qoi_desc *desc) {
	const unsigned char *bytes;
	int frame_count, payload_offset;

	if (data == NULL || desc == NULL) {
		return 0;
	}

	frame_count = qoi_container_scan((const unsigned char *)data, size,
		NULL, NULL, NULL);
	if (frame_count == 0) {
		return 0;
	}

	/* The first payload always starts right after the two headers. */
	bytes = (const unsigned char *)data;
	payload_offset = QOI_CONTAINER_HEADER_SIZE + QOI_FRAME_HEADER_SIZE;
	if (!qoi_container_read_payload_header(bytes + payload_offset,
		size - payload_offset, desc)) {
		return 0;
	}

	return frame_count;
}

int qoi_container_frame_info(const void *data, int size, int index,
	qoi_desc *desc, unsigned int *delay_ms)
{
	const unsigned char *bytes;
	int frame_count, pos, i;

	if (data == NULL || desc == NULL || index < 0) {
		return 0;
	}

	frame_count = qoi_container_scan((const unsigned char *)data, size,
		NULL, NULL, NULL);
	if (frame_count == 0 || index >= frame_count) {
		return 0;
	}

	bytes = (const unsigned char *)data;
	pos = QOI_CONTAINER_HEADER_SIZE;
	for (i = 0; i < index; i++) {
		unsigned int frame_size;
		int p = pos;
		frame_size = qoi_read_32(bytes, &p);
		pos = p + 4 + (int)frame_size;
	}

	{
		int p = pos;
		unsigned int frame_size = qoi_read_32(bytes, &p);
		unsigned int delay = qoi_read_32(bytes, &p);

		if (!qoi_container_read_payload_header(bytes + p,
			(int)frame_size, desc)) {
			return 0;
		}
		if (delay_ms != NULL) {
			*delay_ms = delay;
		}
	}

	return 1;
}

void **qoi_container_decode(const void *data, int size, qoi_desc *desc,
	int channels)
{
	const unsigned char *bytes;
	int frame_count, i;
	int *offsets, *sizes;
	void **frames;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4)
	) {
		return NULL;
	}

	bytes = (const unsigned char *)data;

	{
		int count = qoi_container_scan(bytes, size, NULL, NULL, NULL);
		if (count == 0) {
			return NULL;
		}
		frame_count = count;
	}

	offsets = (int *) QOI_MALLOC((size_t)frame_count * sizeof(int));
	sizes = (int *) QOI_MALLOC((size_t)frame_count * sizeof(int));
	frames = (void **) QOI_MALLOC((size_t)frame_count * sizeof(void *));

	if (!offsets || !sizes || !frames) {
		QOI_FREE(offsets);
		QOI_FREE(sizes);
		QOI_FREE(frames);
		return NULL;
	}

	if (
		qoi_container_scan(bytes, size, offsets, sizes, NULL) !=
		frame_count
	) {
		QOI_FREE(offsets);
		QOI_FREE(sizes);
		QOI_FREE(frames);
		return NULL;
	}

	for (i = 0; i < frame_count; i++) {
		qoi_desc frame_desc;

		frames[i] = qoi_decode(bytes + offsets[i], sizes[i],
			&frame_desc, channels);
		if (frames[i] == NULL) {
			int j;
			for (j = 0; j < i; j++) {
				QOI_FREE(frames[j]);
			}
			QOI_FREE(offsets);
			QOI_FREE(sizes);
			QOI_FREE(frames);
			return NULL;
		}

		if (
			frame_desc.width != desc->width ||
			frame_desc.height != desc->height ||
			frame_desc.colorspace != desc->colorspace
		) {
			int j;
			for (j = 0; j <= i; j++) {
				QOI_FREE(frames[j]);
			}
			QOI_FREE(offsets);
			QOI_FREE(sizes);
			QOI_FREE(frames);
			return NULL;
		}
	}

	QOI_FREE(offsets);
	QOI_FREE(sizes);
	return frames;
}

void *qoi_container_decode_frame(const void *data, int size, int index,
	qoi_desc *desc, int channels, unsigned int *delay_ms)
{
	const unsigned char *bytes;
	int frame_count, pos, i;
	unsigned int frame_size, delay;
	void *pixels;

	if (
		data == NULL || desc == NULL || index < 0 ||
		(channels != 0 && channels != 3 && channels != 4)
	) {
		return NULL;
	}

	frame_count = qoi_container_scan((const unsigned char *)data, size,
		NULL, NULL, NULL);
	if (frame_count == 0 || index >= frame_count) {
		return NULL;
	}

	/* Traverse the directory to locate frame `index`; no earlier frame
	is ever decoded. */
	bytes = (const unsigned char *)data;
	pos = QOI_CONTAINER_HEADER_SIZE;
	for (i = 0; i < index; i++) {
		int p = pos;
		unsigned int skip_size = qoi_read_32(bytes, &p);
		pos = p + 4 + (int)skip_size;
	}

	{
		int p = pos;
		frame_size = qoi_read_32(bytes, &p);
		delay = qoi_read_32(bytes, &p);
		pos = p;
	}

	pixels = qoi_decode(bytes + pos, (int)frame_size, desc, channels);
	if (pixels == NULL) {
		return NULL;
	}

	if (delay_ms != NULL) {
		*delay_ms = delay;
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
