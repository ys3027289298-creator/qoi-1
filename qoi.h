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
QOAF - QOI Animation Frame container

An indexed container for sequences of independently encoded QOI images. Each
frame is encoded with the unchanged qoi_encode() function, so a frame pulled
out of a container is byte-identical to the same image encoded standalone.

On-disk layout (all multi-byte values are big endian):

	struct qoaf_header_t {        // all offsets shown in brackets
		char     magic[4];       // [ 0] "qoaf"
		uint32_t version;        // [ 4] 1
		uint32_t frame_count;    // [ 8] number of frames, 0 is rejected
		uint32_t width;          // [12] image width shared by all frames
		uint32_t height;         // [16] image height shared by all frames
		uint8_t  channels;       // [20] 3 = RGB, 4 = RGBA
		uint8_t  colorspace;     // [21] 0 = sRGB, 1 = linear
		uint16_t flags;          // [22] reserved, must be 0
		uint32_t table_crc;      // [24] FNV-1a 32 bit checksum of the
		                         //      index table that follows this header
	};                             // 28 bytes

	struct qoaf_entry_t {          // one entry per frame, frame_count total
		uint32_t offset;       // offset of the frame's "qoif" data
		uint32_t size;         // size of the frame's QOI data in bytes
		uint16_t delay_ms;     // frame delay in milliseconds (configurable)
		uint16_t flags;        // bit 0 = keyframe (configurable)
	};                             // 12 bytes per frame

	<frame_count * qoaf_entry_t>   // the index (offset) table
	<frame 0 QOI data><frame 1 QOI data>...  // tightly packed frame data

Frames are located purely through the offset table, which allows random access
to any frame by index as well as sequential playback with the player API. */

#define QOAF_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'a') <<  8 | ((unsigned int)'f'))
#define QOAF_VERSION 1
#define QOAF_HEADER_SIZE 28
#define QOAF_ENTRY_SIZE 12
#define QOAF_FRAMES_MAX ((unsigned int)16000000)
#define QOAF_FLAG_KEYFRAME 0x0001

/* Per-frame, user configurable options. opts may be NULL in qoaf_encode(),
in which case every frame gets delay_ms 0 and is marked as a keyframe. */
typedef struct {
	unsigned short delay_ms;
	unsigned char keyframe;
} qoaf_frame_opts;


/* Encode frame_count images, described by descs[0..frame_count-1] and pixel
buffers pixels[0..frame_count-1], into one indexed QOAF container in memory.
All frames must share the same dimensions, channels and colorspace. Returns
NULL on failure and sets *out_len to 0. The returned data is free()d by the
caller. */
void *qoaf_encode(const void *const *pixels, const qoi_desc *descs,
	const qoaf_frame_opts *opts, int frame_count, int *out_len);


/* Decode a single frame by index (0 <= index < frame_count). The returned
pixels are free()d by the caller. Returns NULL on failure, including a frame
count of 0 or a corrupted offset table. */
void *qoaf_decode_frame(const void *data, int size, int index,
	qoi_desc *desc, int channels, qoaf_frame_opts *opts);


/* Decode every frame in order. *out_frames receives a malloc()d array of
frame_count pixel pointers that the caller must free() individually before
freeing the array itself. desc receives the shared frame description and
out_descs, when non-NULL, receives a malloc()d array of per-frame options.
Returns the number of decoded frames or 0 on failure. */
int qoaf_decode_all(const void *data, int size, int channels,
	void ***out_frames, qoi_desc *desc, qoaf_frame_opts **out_descs);


/* Sequential player over a container. The player references the caller's
buffer without copying it and supports random access through
qoaf_player_seek(). */
typedef struct qoaf_player qoaf_player;

qoaf_player *qoaf_player_open(const void *data, int size);
void qoaf_player_close(qoaf_player *player);
int qoaf_player_seek(qoaf_player *player, int index);
void *qoaf_player_next(qoaf_player *player, qoi_desc *desc, int channels,
	qoaf_frame_opts *opts);
int qoaf_player_index(const qoaf_player *player);
int qoaf_player_count(const qoaf_player *player);


#ifndef QOI_NO_STDIO

/* Write an encoded QOAF container to the file system. Returns the number of
bytes written or 0 on failure. */
int qoaf_write(const char *filename, const void *data, int size);

/* Read a complete QOAF container from the file system. *out_len receives the
file size. The returned buffer is free()d by the caller. */
void *qoaf_read(const char *filename, int *out_len);

#endif /* QOI_NO_STDIO */


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
QOAF - QOI Animation Frame container implementation */

#define QOAF_MASK_KEYFRAME 0x0001u

struct qoaf_player {
	const unsigned char *data;
	int size;
	unsigned int frame_count;
	int pos;
};

static unsigned int qoaf_crc32(const unsigned char *data, int len) {
	unsigned int crc = 2166136261u;
	int i;
	for (i = 0; i < len; i++) {
		crc ^= data[i];
		crc *= 16777619u;
	}
	return crc;
}

/* Validate the container header and the offset table. Returns the number of
frames or 0 when the data is not a valid QOAF container. */
static unsigned int qoaf_validate(const void *data, int size) {
	const unsigned char *bytes;
	unsigned int magic, version, frame_count, width, height, table_crc, calc_crc;
	unsigned int table_bytes, data_start, prev_end, i;
	unsigned char channels, colorspace, flags_hi, flags_lo;
	int p = 0;

	if (data == NULL || size < QOAF_HEADER_SIZE) {
		return 0;
	}
	bytes = (const unsigned char *)data;

	magic = qoi_read_32(bytes, &p);
	version = qoi_read_32(bytes, &p);
	frame_count = qoi_read_32(bytes, &p);
	/* width, height */
	width = qoi_read_32(bytes, &p);
	height = qoi_read_32(bytes, &p);
	/* channels [20], colorspace [21], flags [22..23] */
	channels = bytes[p++];
	colorspace = bytes[p++];
	flags_hi = bytes[p++];
	flags_lo = bytes[p++];
	/* table_crc [24..27] */
	table_crc = qoi_read_32(bytes, &p);

	if (
		magic != QOAF_MAGIC || version != QOAF_VERSION ||
		frame_count == 0 || frame_count > QOAF_FRAMES_MAX ||
		width == 0 || height == 0 ||
		channels < 3 || channels > 4 || colorspace > 1 ||
		flags_hi != 0 || flags_lo != 0 ||
		height >= QOI_PIXELS_MAX / width
	) {
		return 0;
	}

	if ((unsigned long long)frame_count * QOAF_ENTRY_SIZE > (unsigned int)-1) {
		return 0;
	}
	table_bytes = frame_count * QOAF_ENTRY_SIZE;

	if ((unsigned long long)size < (unsigned long long)QOAF_HEADER_SIZE + table_bytes) {
		return 0;
	}

	calc_crc = qoaf_crc32(bytes + QOAF_HEADER_SIZE, (int)table_bytes);
	if (calc_crc != table_crc) {
		return 0;
	}

	data_start = QOAF_HEADER_SIZE + table_bytes;
	prev_end = data_start;

	for (i = 0; i < frame_count; i++) {
		const unsigned char *entry = bytes + QOAF_HEADER_SIZE + i * QOAF_ENTRY_SIZE;
		unsigned int offset, frame_size;
		int ep = 0;

		offset = qoi_read_32(entry, &ep);
		frame_size = qoi_read_32(entry, &ep);

		/* Entries must be tight, in order and fully inside the file. */
		if (offset != prev_end || frame_size == 0) {
			return 0;
		}
		if ((unsigned long long)offset + frame_size > (unsigned long long)size) {
			return 0;
		}
		prev_end = offset + frame_size;
	}

	return frame_count;
}

static void *qoaf_decode_at(const void *data, int index,
	qoi_desc *desc, int channels, qoaf_frame_opts *opts)
{
	const unsigned char *bytes = (const unsigned char *)data;
	const unsigned char *entry;
	unsigned int offset, frame_size;
	int ep = 0, hp = 0;
	void *pixels;

	entry = bytes + QOAF_HEADER_SIZE + (unsigned int)index * QOAF_ENTRY_SIZE;
	offset = qoi_read_32(entry, &ep);
	frame_size = qoi_read_32(entry, &ep);

	if (opts != NULL) {
		opts->delay_ms = (unsigned short)(
			((unsigned int)entry[8] << 8) | (unsigned int)entry[9]
		);
		opts->keyframe = (unsigned char)(
			(((unsigned int)entry[10] << 8) | (unsigned int)entry[11]) &
			QOAF_MASK_KEYFRAME
		);
	}

	/* The frame itself must be a valid QOI image. */
	if (
		frame_size < QOI_HEADER_SIZE + (int)sizeof(qoi_padding) ||
		qoi_read_32(bytes + offset, &hp) != QOI_MAGIC
	) {
		return NULL;
	}

	pixels = qoi_decode(bytes + offset, (int)frame_size, desc, channels);
	return pixels;
}

void *qoaf_encode(const void *const *pixels, const qoi_desc *descs,
	const qoaf_frame_opts *opts, int frame_count, int *out_len)
{
	unsigned char **frames;
	int *frame_sizes;
	unsigned char *bytes;
	unsigned int table_bytes, data_start, total, table_crc;
	int i, p;

	if (out_len != NULL) {
		*out_len = 0;
	}

	if (
		pixels == NULL || descs == NULL || out_len == NULL ||
		frame_count <= 0 || (unsigned int)frame_count > QOAF_FRAMES_MAX
	) {
		return NULL;
	}

	if (
		descs[0].width == 0 || descs[0].height == 0 ||
		descs[0].channels < 3 || descs[0].channels > 4 ||
		descs[0].colorspace > 1 ||
		descs[0].height >= QOI_PIXELS_MAX / descs[0].width
	) {
		return NULL;
	}

	table_bytes = (unsigned int)frame_count * QOAF_ENTRY_SIZE;
	data_start = QOAF_HEADER_SIZE + table_bytes;

	frames = (unsigned char **)QOI_MALLOC((size_t)frame_count * sizeof(void *));
	frame_sizes = (int *)QOI_MALLOC((size_t)frame_count * sizeof(int));
	if (frames == NULL || frame_sizes == NULL) {
		QOI_FREE(frames);
		QOI_FREE(frame_sizes);
		return NULL;
	}

	total = data_start;

	for (i = 0; i < frame_count; i++) {
		if (
			pixels[i] == NULL ||
			descs[i].width != descs[0].width ||
			descs[i].height != descs[0].height ||
			descs[i].channels != descs[0].channels ||
			descs[i].colorspace != descs[0].colorspace
		) {
			goto encode_fail;
		}

		frames[i] = (unsigned char *)qoi_encode(pixels[i], &descs[i], &frame_sizes[i]);
		if (frames[i] == NULL || frame_sizes[i] <= 0) {
			goto encode_fail;
		}

		if ((unsigned long long)total + (unsigned int)frame_sizes[i] > (unsigned int)-1) {
			goto encode_fail;
		}
		total += (unsigned int)frame_sizes[i];
	}

	bytes = (unsigned char *)QOI_MALLOC(total);
	if (bytes == NULL) {
		goto encode_fail;
	}

	/* Container header */
	p = 0;
	qoi_write_32(bytes, &p, QOAF_MAGIC);
	qoi_write_32(bytes, &p, QOAF_VERSION);
	qoi_write_32(bytes, &p, (unsigned int)frame_count);
	qoi_write_32(bytes, &p, descs[0].width);
	qoi_write_32(bytes, &p, descs[0].height);
	bytes[p++] = descs[0].channels;   /* [20] */
	bytes[p++] = descs[0].colorspace; /* [21] */
	bytes[p++] = 0;                   /* [22] flags high */
	bytes[p++] = 0;                   /* [23] flags low  */
	qoi_write_32(bytes, &p, 0);       /* [24..27] table_crc, patched below */

	/* Offset table, then frame data */
	p = QOAF_HEADER_SIZE;
	{
		unsigned int offset = data_start;
		for (i = 0; i < frame_count; i++) {
			unsigned short delay = opts ? opts[i].delay_ms : 0;
			unsigned short flags = (opts && opts[i].keyframe) ? QOAF_FLAG_KEYFRAME : 0;

			qoi_write_32(bytes, &p, offset);
			qoi_write_32(bytes, &p, (unsigned int)frame_sizes[i]);
			bytes[p++] = (unsigned char)(delay >> 8);
			bytes[p++] = (unsigned char)(delay & 0xff);
			bytes[p++] = (unsigned char)(flags >> 8);
			bytes[p++] = (unsigned char)(flags & 0xff);

			offset += (unsigned int)frame_sizes[i];
		}
	}

	table_crc = qoaf_crc32(bytes + QOAF_HEADER_SIZE, (int)table_bytes);
	{
		int crc_p = 24;
		qoi_write_32(bytes, &crc_p, table_crc);
	}

	{
		unsigned int offset = data_start;
		for (i = 0; i < frame_count; i++) {
			memcpy(bytes + offset, frames[i], frame_sizes[i]);
			offset += (unsigned int)frame_sizes[i];
			QOI_FREE(frames[i]);
		}
	}

	QOI_FREE(frames);
	QOI_FREE(frame_sizes);

	*out_len = (int)total;
	return bytes;

encode_fail:
	for (i = 0; i < frame_count; i++) {
		if (frames[i] != NULL) {
			QOI_FREE(frames[i]);
		}
	}
	QOI_FREE(frames);
	QOI_FREE(frame_sizes);
	return NULL;
}

void *qoaf_decode_frame(const void *data, int size, int index,
	qoi_desc *desc, int channels, qoaf_frame_opts *opts)
{
	unsigned int frame_count;

	if (
		channels != 0 && channels != 3 && channels != 4
	) {
		return NULL;
	}

	frame_count = qoaf_validate(data, size);
	if (frame_count == 0 || index < 0 || (unsigned int)index >= frame_count) {
		return NULL;
	}

	return qoaf_decode_at(data, index, desc, channels, opts);
}

int qoaf_decode_all(const void *data, int size, int channels,
	void ***out_frames, qoi_desc *desc, qoaf_frame_opts **out_descs)
{
	unsigned int frame_count;
	void **frames;
	qoaf_frame_opts *descs_opts;
	unsigned int i;

	if (
		out_frames == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4)
	) {
		return 0;
	}

	frame_count = qoaf_validate(data, size);
	if (frame_count == 0) {
		return 0;
	}

	frames = (void **)QOI_MALLOC((size_t)frame_count * sizeof(void *));
	descs_opts = (qoaf_frame_opts *)QOI_MALLOC((size_t)frame_count * sizeof(qoaf_frame_opts));
	if (frames == NULL || descs_opts == NULL) {
		QOI_FREE(frames);
		QOI_FREE(descs_opts);
		return 0;
	}

	for (i = 0; i < frame_count; i++) {
		frames[i] = qoaf_decode_at(data, (int)i, desc, channels, &descs_opts[i]);
		if (frames[i] == NULL) {
			unsigned int j;
			for (j = 0; j < i; j++) {
				QOI_FREE(frames[j]);
			}
			QOI_FREE(frames);
			QOI_FREE(descs_opts);
			return 0;
		}
	}

	*out_frames = frames;
	if (out_descs != NULL) {
		*out_descs = descs_opts;
	}
	else {
		QOI_FREE(descs_opts);
	}
	return (int)frame_count;
}

qoaf_player *qoaf_player_open(const void *data, int size) {
	unsigned int frame_count = qoaf_validate(data, size);
	qoaf_player *player;

	if (frame_count == 0) {
		return NULL;
	}

	player = (qoaf_player *)QOI_MALLOC(sizeof(qoaf_player));
	if (player == NULL) {
		return NULL;
	}

	player->data = (const unsigned char *)data;
	player->size = size;
	player->frame_count = frame_count;
	player->pos = 0;
	return player;
}

void qoaf_player_close(qoaf_player *player) {
	QOI_FREE(player);
}

int qoaf_player_seek(qoaf_player *player, int index) {
	if (
		player == NULL || index < 0 ||
		(unsigned int)index >= player->frame_count
	) {
		return 0;
	}
	player->pos = index;
	return 1;
}

void *qoaf_player_next(qoaf_player *player, qoi_desc *desc, int channels,
	qoaf_frame_opts *opts)
{
	void *pixels;

	if (
		player == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		(unsigned int)player->pos >= player->frame_count
	) {
		return NULL;
	}

	pixels = qoaf_decode_at(player->data, player->pos, desc, channels, opts);
	if (pixels != NULL) {
		player->pos++;
	}
	return pixels;
}

int qoaf_player_index(const qoaf_player *player) {
	return player ? player->pos : -1;
}

int qoaf_player_count(const qoaf_player *player) {
	return player ? (int)player->frame_count : 0;
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

int qoaf_write(const char *filename, const void *data, int size) {
	FILE *f;
	int err;

	if (filename == NULL || data == NULL || size <= 0) {
		return 0;
	}

	f = fopen(filename, "wb");
	if (!f) {
		return 0;
	}

	fwrite(data, 1, size, f);
	fflush(f);
	err = ferror(f);
	fclose(f);
	return err ? 0 : size;
}

void *qoaf_read(const char *filename, int *out_len) {
	FILE *f;
	long size;
	void *data;
	size_t bytes_read;

	if (filename == NULL || out_len == NULL) {
		return NULL;
	}
	*out_len = 0;

	f = fopen(filename, "rb");
	if (!f) {
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}

	data = QOI_MALLOC((size_t)size);
	if (!data) {
		fclose(f);
		return NULL;
	}

	bytes_read = fread(data, 1, (size_t)size, f);
	fclose(f);

	if (bytes_read != (size_t)size) {
		QOI_FREE(data);
		return NULL;
	}

	*out_len = (int)size;
	return data;
}

#endif /* QOI_NO_STDIO */
#endif /* QOI_IMPLEMENTATION */
