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


/* ---- LZ77 post-processing layer -----------------------------------------

An optional, self-contained LZ77 transform that can be applied to any byte
buffer (e.g. the output of qoi_encode) to squeeze out redundancy the QOI
format leaves behind, and later be reversed to recover the exact original
bytes. No third party code is used.

A qolz stream consists of a 12 byte big-endian header

	"qolz"  uint8 log2(window_size)  uint8 log2(min_match)  uint16 reserved
	uint32 original_size

followed by one or more blocks. Each block holds a 32-bit big-endian token
count (its high bit is the block type: 0 = LZ77 tokens, 1 = raw stored
bytes), a 32-bit big-endian payload length and either a bit-packed LZ77
token stream or the verbatim input bytes:

	flag 1 bit     0 = literal, followed by 8 data bits
	               1 = match,   followed by
	                              length    : variable length unsigned integer,
	                                           stored value = match_len - min_match
	                              distance-1: variable length unsigned integer
	                              distance  : 1..window_size, most recent byte

A raw block contains exactly token_count verbatim bytes. The encoder emits
raw blocks whenever LZ77 would not make the block smaller, so an
incompressible stream never grows beyond the framing overhead. In an LZ77
block exactly token_count tokens are read; any padding bits that fill the
final byte are ignored.

A variable length unsigned integer is encoded in groups of 7 bits, low group
first, each prefixed by a continuation bit (1 = another group follows).

window_size and min_match must both be powers of two. window_size must be in
the range 256..65536, min_match in the range 3..256, and min_match must not
exceed window_size. Invalid parameters make the functions return NULL. */

/* LZ77-encode size bytes from data using a sliding window of window_size
bytes and a minimum match length of min_match. Returns a malloc'd buffer of
*out_len bytes, or NULL on invalid parameters or malloc failure. */

void *qoi_lz77_encode(const void *data, int size, int *out_len,
	int window_size, int min_match);


/* Invert qoi_lz77_encode. The window_size and min_match are read from the
stream header and need not be supplied by the caller. Returns a malloc'd
buffer of *out_len bytes, or NULL on invalid parameters, malformed data or
malloc failure. */

void *qoi_lz77_decode(const void *data, int size, int *out_len);


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
LZ77 layer - self-contained, no third party code. See the documentation above
the qoi_lz77_encode() declaration for the stream format. */

#define QOLZ_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'l') <<  8 | ((unsigned int)'z'))
#define QOLZ_HEADER_SIZE    12
#define QOLZ_BLOCK_HDR_SIZE 8
#define QOLZ_BLOCK_MAX      (16 * 1024 * 1024)
#define QOLZ_BLOCK_LZ77     0
#define QOLZ_BLOCK_RAW      1
#define QOLZ_MATCH_MAX      65535
#define QOLZ_HASH_LOG       16
#define QOLZ_HASH_SIZE      (1 << QOLZ_HASH_LOG)
#define QOLZ_CHAIN_MAX      32

typedef struct {
	unsigned char *data;
	int cap;
	int pos;       /* bytes flushed */
	unsigned int acc;
	int nbits;
} qolz_writer_t;

typedef struct {
	const unsigned char *data;
	int size;
	int pos;
	unsigned int acc;
	int nbits;
} qolz_reader_t;

static int qolz_is_pow2(int v) {
	return v > 0 && (v & (v - 1)) == 0;
}

/* Exact log2 of a power of two; undefined for other values (callers
validate with qolz_is_pow2 first). */
static int qolz_log2(int v) {
	int log = 0;
	while (v > 1) {
		v >>= 1;
		log++;
	}
	return log;
}

/* Finalize one block at out[start]: write the 8 byte header (token count
with the block-type flag in the high bit, payload length) and the payload.
An LZ77 payload is taken from lz/lz_len; when it is not strictly smaller
than the raw input, the verbatim bytes src[src_off .. src_off+in_len) are
stored in a raw block instead. Returns the number of bytes the block
occupies, or 0 on failure. */
static int qolz_write_block(unsigned char *out, int start, int cap,
	const unsigned char *src, int src_off, int in_len,
	int token_count, const unsigned char *lz, int lz_len) {
	int use_raw = (in_len == 0) || (lz_len >= in_len);
	int type = use_raw ? QOLZ_BLOCK_RAW : QOLZ_BLOCK_LZ77;
	int count = use_raw ? in_len : token_count;
	int payload_len = use_raw ? in_len : lz_len;
	unsigned int header_word =
		(unsigned int)count | (type ? 0x80000000u : 0u);

	if (start + QOLZ_BLOCK_HDR_SIZE + payload_len > cap ||
		payload_len > QOLZ_BLOCK_MAX || count < 0) {
		return 0;
	}

	out[start + 0] = (unsigned char)((header_word >> 24) & 0xff);
	out[start + 1] = (unsigned char)((header_word >> 16) & 0xff);
	out[start + 2] = (unsigned char)((header_word >>  8) & 0xff);
	out[start + 3] = (unsigned char)(header_word & 0xff);
	out[start + 4] = (unsigned char)((payload_len >> 24) & 0xff);
	out[start + 5] = (unsigned char)((payload_len >> 16) & 0xff);
	out[start + 6] = (unsigned char)((payload_len >>  8) & 0xff);
	out[start + 7] = (unsigned char)(payload_len & 0xff);

	if (use_raw) {
		memcpy(out + start + QOLZ_BLOCK_HDR_SIZE,
			src + src_off, (size_t)in_len);
	}
	else {
		memcpy(out + start + QOLZ_BLOCK_HDR_SIZE,
			lz, (size_t)lz_len);
	}
	return QOLZ_BLOCK_HDR_SIZE + payload_len;
}

static void qolz_writer_init(qolz_writer_t *w, void *data, int cap) {
	w->data = (unsigned char *)data;
	w->cap = cap;
	w->pos = 0;
	w->acc = 0;
	w->nbits = 0;
}

static void qolz_put(qolz_writer_t *w, unsigned int value, int nbits) {
	w->acc |= (value & ((1u << nbits) - 1)) << w->nbits;
	w->nbits += nbits;
	while (w->nbits >= 8) {
		w->data[w->pos++] = (unsigned char)(w->acc & 0xff);
		w->acc >>= 8;
		w->nbits -= 8;
	}
}

static void qolz_flush(qolz_writer_t *w) {
	while (w->nbits > 0) {
		w->data[w->pos++] = (unsigned char)(w->acc & 0xff);
		w->acc >>= 8;
		w->nbits -= 8;
	}
	w->acc = 0;
	w->nbits = 0;
}

static void qolz_put_varuint(qolz_writer_t *w, unsigned int value) {
	do {
		unsigned int group = value & 0x7f;
		value >>= 7;
		qolz_put(w, group | (value ? 0x80 : 0x00), 8);
	} while (value);
}

static int qolz_reader_init(qolz_reader_t *r, const void *data, int size) {
	if (size < 0) {
		return 0;
	}
	r->data = (const unsigned char *)data;
	r->size = size;
	r->pos = 0;
	r->acc = 0;
	r->nbits = 0;
	return 1;
}

/* Read nbits bits. Returns 0 when the buffered stream does not hold enough
bits. Block headers state the exact token count, so padding bits in the
final byte are never requested. */
static int qolz_get(qolz_reader_t *r, unsigned int *out, int nbits) {
	unsigned int value = 0;
	int got = 0;
	while (got < nbits) {
		int take;
		if (r->nbits == 0) {
			if (r->pos >= r->size) {
				return 0;
			}
			r->acc = r->data[r->pos++];
			r->nbits = 8;
		}
		take = r->nbits;
		if (take > nbits - got) {
			take = nbits - got;
		}
		value |= (r->acc & ((1u << take) - 1)) << got;
		r->acc >>= take;
		r->nbits -= take;
		got += take;
	}
	*out = value;
	return 1;
}

static int qolz_get_varuint(qolz_reader_t *r, unsigned int *out) {
	unsigned int value = 0;
	int shift = 0;
	unsigned int byte;
	do {
		if (shift > 28 || !qolz_get(r, &byte, 8)) {
			return 0;
		}
		value |= (byte & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);
	*out = value;
	return 1;
}

void *qoi_lz77_encode(const void *data, int size, int *out_len,
	int window_size, int min_match) {
	const unsigned char *src;
	unsigned char *out;
	int *chain;
	int head[QOLZ_HASH_SIZE];
	int out_cap, out_pos, block_start, block_tokens;
	int block_in_pos;
	int pos, i;
	qolz_writer_t w;

	if (
		data == NULL || out_len == NULL || size < 0 ||
		!qolz_is_pow2(window_size) || window_size < 256 ||
		window_size > 65536 ||
		!qolz_is_pow2(min_match) || min_match < 3 || min_match > 256 ||
		min_match > window_size
	) {
		return NULL;
	}

	/* chain[] is indexed by byte position, so it needs one slot per
	input byte. head[] maps a hash to the most recent position. */
	chain = (int *) QOI_MALLOC((size_t)(size > 0 ? size : 1) * sizeof(int));
	if (!chain) {
		return NULL;
	}

	/* Worst case: header + one block header per block + 9 bits per byte,
	rounded up generously. A block holds at most QOLZ_BLOCK_MAX payload
	bytes, so the number of blocks is bounded. */
	out_cap = QOLZ_HEADER_SIZE + QOLZ_BLOCK_HDR_SIZE +
		(size > 0 ? (int)(((long long)size * 9 + 7) / 8) + 64 : 0) +
		(int)(((long long)size * 9 / (8LL * QOLZ_BLOCK_MAX) + 1) *
			QOLZ_BLOCK_HDR_SIZE);
	out = (unsigned char *) QOI_MALLOC((size_t)out_cap);
	if (!out) {
		QOI_FREE(chain);
		return NULL;
	}

	for (i = 0; i < QOLZ_HASH_SIZE; i++) {
		head[i] = -1;
	}

	src = (const unsigned char *)data;
	out_pos = 0;

	qoi_write_32(out, &out_pos, QOLZ_MAGIC);
	out[out_pos++] = (unsigned char)qolz_log2(window_size);
	out[out_pos++] = (unsigned char)qolz_log2(min_match);
	out[out_pos++] = 0; /* reserved */
	out[out_pos++] = 0;
	qoi_write_32(out, &out_pos, (unsigned int)size);

	block_start = out_pos;
	out_pos += QOLZ_BLOCK_HDR_SIZE; /* backpatched later */
	block_tokens = 0;
	block_in_pos = 0;
	qolz_writer_init(&w, out + out_pos, out_cap - out_pos);
	pos = 0;
	while (pos < size) {
		unsigned int h = 0;
		int candidate, probes, best_len = 0, best_dist = 0;
		int match_len;

		if (size - pos >= min_match) {
			int limit = pos - window_size;
			if (limit < 0) {
				limit = 0;
			}

			h = (unsigned int)src[pos] * 2654435761u;
			h ^= (unsigned int)src[pos + 1] * 2246822519u;
			h ^= (unsigned int)src[pos + 2] * 3266489917u;
			h &= (QOLZ_HASH_SIZE - 1);

			candidate = head[h];
			for (probes = 0;
				candidate >= limit && probes < QOLZ_CHAIN_MAX;
				probes++
			) {
				int len = 0;
				int max_len = size - pos;
				if (max_len > QOLZ_MATCH_MAX) {
					max_len = QOLZ_MATCH_MAX;
				}
				while (
					len < max_len &&
					src[candidate + len] == src[pos + len]
				) {
					len++;
				}
				if (len > best_len) {
					best_len = len;
					best_dist = pos - candidate;
					if (len == max_len) {
						break;
					}
				}
				if (candidate == 0 || chain[candidate] >= candidate) {
					break;
				}
				candidate = chain[candidate];
			}
		}

		if (best_len >= min_match) {
			match_len = best_len;
			block_tokens++;
			qolz_put(&w, 1, 1);
			qolz_put_varuint(&w, (unsigned int)(match_len - min_match));
			qolz_put_varuint(&w, (unsigned int)(best_dist - 1));

			/* Insert every position covered by the match so matches
			starting inside the run remain findable. */
			{
				int end = pos + match_len;
				int j;
				for (j = pos; j < end && j < size; j++) {
					unsigned int hj;
					if (size - j < 3) {
						break;
					}
					hj = (unsigned int)src[j] * 2654435761u;
					hj ^= (unsigned int)src[j + 1] * 2246822519u;
					hj ^= (unsigned int)src[j + 2] * 3266489917u;
					hj &= (QOLZ_HASH_SIZE - 1);
					chain[j] = head[hj];
					head[hj] = j;
				}
			}
			pos += match_len;
		}
		else {
			block_tokens++;
			qolz_put(&w, 0, 1);
			qolz_put(&w, src[pos], 8);
			if (size - pos >= 3) {
				chain[pos] = head[h];
				head[h] = pos;
			}
			pos++;
		}

		/* Flush a block before it can exceed QOLZ_BLOCK_MAX input
		bytes. */
		if (pos - block_in_pos >= QOLZ_BLOCK_MAX && pos < size) {
			int written;
			qolz_flush(&w);
			written = qolz_write_block(out, block_start, out_cap,
				src, block_in_pos, pos - block_in_pos,
				block_tokens, w.data, w.pos);
			if (!written) {
				QOI_FREE(chain);
				QOI_FREE(out);
				return NULL;
			}
			out_pos = block_start + written;
			block_start = out_pos;
			out_pos += QOLZ_BLOCK_HDR_SIZE;
			block_tokens = 0;
			block_in_pos = pos;
			qolz_writer_init(&w, out + out_pos, out_cap - out_pos);
		}
	}

	qolz_flush(&w);
	{
		int written = qolz_write_block(out, block_start, out_cap,
			src, block_in_pos, size - block_in_pos,
			block_tokens, w.data, w.pos);
		if (!written) {
			QOI_FREE(chain);
			QOI_FREE(out);
			return NULL;
		}
		out_pos = block_start + written;
	}

	QOI_FREE(chain);
	*out_len = out_pos;
	return out;
}

void *qoi_lz77_decode(const void *data, int size, int *out_len) {
	const unsigned char *src;
	unsigned char *out;
	int window_size, min_match;
	int p = 0, out_cap, out_pos = 0;

	if (data == NULL || out_len == NULL ||
		size < QOLZ_HEADER_SIZE + QOLZ_BLOCK_HDR_SIZE) {
		return NULL;
	}

	src = (const unsigned char *)data;

	if (qoi_read_32(src, &p) != QOLZ_MAGIC) {
		return NULL;
	}
	/* Reject exponents that could not have passed the encoder's
	validation before doing the shift. */
	if (src[4] < 8 || src[4] > 16 || src[5] < 2 || src[5] > 8 ||
		src[5] > src[4]) {
		return NULL;
	}
	window_size = 1 << src[p++];
	min_match = 1 << src[p++];
	p += 2; /* reserved bytes */
	{
		unsigned int original_size = qoi_read_32(src, &p);
		if (original_size > (unsigned int)0x7fffffff) {
			return NULL;
		}
		out_cap = (int)original_size;
	}

	if (
		!qolz_is_pow2(window_size) || window_size < 256 ||
		window_size > 65536 ||
		!qolz_is_pow2(min_match) || min_match < 3 || min_match > 256 ||
		min_match > window_size
	) {
		return NULL;
	}

	out = (unsigned char *) QOI_MALLOC((size_t)out_cap ? (size_t)out_cap : 1);
	if (!out) {
		return NULL;
	}

	while (p < size) {
		int block_type;
		unsigned int token_count, payload_len;
		qolz_reader_t r;
		unsigned int flag;
		unsigned int token;

		if (p + QOLZ_BLOCK_HDR_SIZE > size) {
			goto fail;
		}
		{
			unsigned int hword =
				((unsigned int)src[p] << 24) |
				((unsigned int)src[p + 1] << 16) |
				((unsigned int)src[p + 2] << 8) |
				(unsigned int)src[p + 3];
			payload_len =
				((unsigned int)src[p + 4] << 24) |
				((unsigned int)src[p + 5] << 16) |
				((unsigned int)src[p + 6] << 8) |
				(unsigned int)src[p + 7];
			block_type = (int)((hword >> 31) & 1u);
			token_count = hword & 0x7fffffffu;
		}
		p += QOLZ_BLOCK_HDR_SIZE;

		if ((long long)p + (long long)payload_len > (long long)size ||
			(block_type != QOLZ_BLOCK_LZ77 &&
				block_type != QOLZ_BLOCK_RAW)) {
			goto fail;
		}

		if (block_type == QOLZ_BLOCK_RAW) {
			/* Verbatim bytes: count and payload length are equal. */
			if (token_count != payload_len ||
				(long long)out_pos + (long long)payload_len >
					(long long)out_cap) {
				goto fail;
			}
			memcpy(out + out_pos, src + p, payload_len);
			out_pos += (int)payload_len;
			p += (int)payload_len;
			continue;
		}

		if (!qolz_reader_init(&r, src + p, (int)payload_len)) {
			goto fail;
		}
		p += (int)payload_len;

		/* Read exactly token_count tokens; padding bits in the final
		byte are never touched. */
		for (token = 0; token < token_count; token++) {
			if (!qolz_get(&r, &flag, 1)) {
				goto fail;
			}
			if (flag == 0) {
				unsigned int value;
				if (!qolz_get(&r, &value, 8) ||
					out_pos >= out_cap) {
					goto fail;
				}
				out[out_pos++] = (unsigned char)value;
			}
			else {
				unsigned int len_extra, dist_m1;
				int match_len, distance, start, k;
				if (!qolz_get_varuint(&r, &len_extra) ||
					!qolz_get_varuint(&r, &dist_m1)) {
					goto fail;
				}
				match_len = (int)len_extra + min_match;
				distance = (int)dist_m1 + 1;
				if (match_len > QOLZ_MATCH_MAX ||
					distance > window_size ||
					distance > out_pos ||
					match_len > out_cap - out_pos) {
					goto fail;
				}
				/* Byte-wise copy: overlapping matches (distance <
				match_len) must repeat the earlier bytes. */
				start = out_pos - distance;
				for (k = 0; k < match_len; k++) {
					out[out_pos++] = out[start + k];
				}
			}
		}
	}

	if (out_pos != out_cap) {
		goto fail;
	}

	*out_len = out_pos;
	return out;

fail:
	QOI_FREE(out);
	return NULL;
}
#endif /* QOI_IMPLEMENTATION */
