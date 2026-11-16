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

For sequences of images, the library can also maintain an appendable
multi-frame container (all functions require stdio);
- qoi_frames_create -- create an empty container with room for N frames
- qoi_frames_append -- append one frame and update header/offset table
- qoi_frames_write  -- write a complete container in a single pass
- qoi_frames_info   -- read the container header and frame count
- qoi_frames_read   -- read and decode one frame by index

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


/* Appendable multi-frame container.

A container file starts with a header and an offset table with `capacity`
slots; the encoded QOI streams of the frames follow. All frames share the
width, height, channels and colorspace stored in the header. Because the
table has a fixed position and a fixed number of slots, appending a frame
only writes the new frame at the end of the file and then rewrites two
small regions: the frame's table slot and the frame counter in the header.
The rest of the file - in particular all previously appended frames - is
never rewritten.

On-disk layout (all multi-byte values are big endian, matching QOI):

	struct qoi_frames_header_t {   // 28 bytes
		char     magic[4];     // [ 0] "qoia"
		uint32_t version;      // [ 4] 1
		uint32_t frame_count;  // [ 8] number of valid table slots
		uint32_t capacity;     // [12] number of slots in the table
		uint32_t width;        // [16] shared frame width
		uint32_t height;       // [20] shared frame height
		uint8_t  channels;     // [24] 3 = RGB, 4 = RGBA
		uint8_t  colorspace;   // [25] 0 = sRGB, 1 = linear
		uint16_t flags;        // [26] reserved, must be 0
	};

	struct qoi_frames_entry_t {    // one per slot, capacity total
		uint32_t offset;       // offset of the frame's "qoif" stream
		uint32_t size;         // size of the frame's QOI data in bytes
	};                             // 8 bytes

	<capacity * qoi_frames_entry_t>  // the offset table
	<frame 0 QOI data><frame 1 QOI data>...

Slots >= frame_count are zero-filled. Frames are located purely through
their table slot, so reading frame `index` never touches the other frames.
Each frame is encoded with the unchanged qoi_encode(), so a frame stored in
a container is byte-identical to the same image written with qoi_write().

Functions return a negative QOI_FRAMES_ERR_* value on failure; qoi_frames_append
returns the number of bytes written for the new frame and qoi_frames_write
returns the total file size on success. */

#define QOI_FRAMES_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'a'))
#define QOI_FRAMES_VERSION 1
#define QOI_FRAMES_HEADER_SIZE 28
#define QOI_FRAMES_ENTRY_SIZE 8

#define QOI_FRAMES_ERR_IO       (-1) /* fopen/fread/fwrite/fseek failed */
#define QOI_FRAMES_ERR_ARG      (-2) /* NULL or otherwise invalid argument */
#define QOI_FRAMES_ERR_FORMAT   (-3) /* not a container, or header/table bad */
#define QOI_FRAMES_ERR_MISMATCH (-4) /* frame desc differs from header */
#define QOI_FRAMES_ERR_FULL     (-5) /* frame_count == capacity */
#define QOI_FRAMES_ERR_RANGE    (-6) /* frame index out of range */

/* Create (or truncate) an empty container with room for `capacity` frames.
Return 0 on success or a negative QOI_FRAMES_ERR_* value. */
int qoi_frames_create(const char *filename, const qoi_desc *desc,
	unsigned int capacity);

/* Append one frame to an existing container. The frame's width, height,
channels and colorspace must match the container header; otherwise the file
is left untouched and QOI_FRAMES_ERR_MISMATCH is returned. Returns the size
of the appended QOI data or a negative QOI_FRAMES_ERR_* value. If the table
is full, QOI_FRAMES_ERR_FULL is returned and the file is left untouched. */
int qoi_frames_append(const char *filename, const void *data,
	const qoi_desc *desc);

/* Write `frame_count` frames in one pass. The resulting file is identical to
a container created with capacity == frame_count followed by one append per
frame, in order. Returns the total file size or a negative error value. */
int qoi_frames_write(const char *filename, const void *const *frames,
	const qoi_desc *desc, int frame_count);

/* Read the header and offset table summary. desc, frame_count_out and
capacity_out may be NULL. Returns 0 on success or a negative error value. */
int qoi_frames_info(const char *filename, qoi_desc *desc,
	unsigned int *frame_count_out, unsigned int *capacity_out);

/* Read and decode one frame by index (0 <= index < frame_count). Returns the
decoded pixels (free() them after use) or NULL on failure. channels follows
the same rules as qoi_read(). */
void *qoi_frames_read(const char *filename, int index, qoi_desc *desc,
	int channels);

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


/* -----------------------------------------------------------------------------
Appendable multi-frame container */

/* The largest capacity whose offset table still fits into the 2GB budget that
qoi's int based offsets can address. */
#define QOI_FRAMES_CAPACITY_MAX \
	((unsigned int)(0x7fffffff - QOI_FRAMES_HEADER_SIZE) / QOI_FRAMES_ENTRY_SIZE)

typedef struct {
	unsigned int frame_count;
	unsigned int capacity;
	unsigned int width;
	unsigned int height;
	unsigned char channels;
	unsigned char colorspace;
} qoi_frames_hdr;

static unsigned int qoi_frames_get_32(const unsigned char *b) {
	return (unsigned int)b[0] << 24 | (unsigned int)b[1] << 16 |
		(unsigned int)b[2] << 8 | (unsigned int)b[3];
}

static void qoi_frames_put_32(unsigned char *b, unsigned int v) {
	b[0] = (0xff000000 & v) >> 24;
	b[1] = (0x00ff0000 & v) >> 16;
	b[2] = (0x0000ff00 & v) >> 8;
	b[3] = (0x000000ff & v);
}

static unsigned int qoi_frames_table_end(const qoi_frames_hdr *h) {
	return QOI_FRAMES_HEADER_SIZE + h->capacity * QOI_FRAMES_ENTRY_SIZE;
}

static int qoi_frames_desc_valid(const qoi_desc *desc) {
	return desc != NULL &&
		desc->width != 0 && desc->height != 0 &&
		desc->channels >= 3 && desc->channels <= 4 &&
		desc->colorspace <= 1 &&
		desc->height < QOI_PIXELS_MAX / desc->width;
}

/* Serialize the 28 byte header. */
static void qoi_frames_put_header(unsigned char *b, const qoi_frames_hdr *h) {
	qoi_frames_put_32(b + 0, QOI_FRAMES_MAGIC);
	qoi_frames_put_32(b + 4, QOI_FRAMES_VERSION);
	qoi_frames_put_32(b + 8, h->frame_count);
	qoi_frames_put_32(b + 12, h->capacity);
	qoi_frames_put_32(b + 16, h->width);
	qoi_frames_put_32(b + 20, h->height);
	b[24] = h->channels;
	b[25] = h->colorspace;
	b[26] = 0;
	b[27] = 0;
}

/* Read and validate the header of an open container. On success the file
position is unspecified; callers seek explicitly. */
static int qoi_frames_read_header(FILE *f, qoi_frames_hdr *h) {
	unsigned char b[QOI_FRAMES_HEADER_SIZE];
	long file_size;

	if (fseek(f, 0, SEEK_END) != 0) {
		return QOI_FRAMES_ERR_IO;
	}
	file_size = ftell(f);
	if (file_size < 0) {
		return QOI_FRAMES_ERR_IO;
	}
	if (file_size < QOI_FRAMES_HEADER_SIZE || fseek(f, 0, SEEK_SET) != 0) {
		return QOI_FRAMES_ERR_FORMAT;
	}
	if (fread(b, 1, QOI_FRAMES_HEADER_SIZE, f) != QOI_FRAMES_HEADER_SIZE) {
		return QOI_FRAMES_ERR_IO;
	}

	if (
		qoi_frames_get_32(b + 0) != QOI_FRAMES_MAGIC ||
		qoi_frames_get_32(b + 4) != QOI_FRAMES_VERSION ||
		b[26] != 0 || b[27] != 0
	) {
		return QOI_FRAMES_ERR_FORMAT;
	}

	h->frame_count = qoi_frames_get_32(b + 8);
	h->capacity = qoi_frames_get_32(b + 12);
	h->width = qoi_frames_get_32(b + 16);
	h->height = qoi_frames_get_32(b + 20);
	h->channels = b[24];
	h->colorspace = b[25];

	if (
		h->capacity == 0 || h->capacity > QOI_FRAMES_CAPACITY_MAX ||
		h->frame_count > h->capacity ||
		h->width == 0 || h->height == 0 ||
		h->channels < 3 || h->channels > 4 ||
		h->colorspace > 1 ||
		h->height >= QOI_PIXELS_MAX / h->width
	) {
		return QOI_FRAMES_ERR_FORMAT;
	}
	if (file_size < (long)qoi_frames_table_end(h)) {
		return QOI_FRAMES_ERR_FORMAT;
	}

	return 0;
}

/* Check that every valid table entry points at a frame that lies inside the
file. This catches a corrupted or truncated table before any frame is read. */
static int qoi_frames_check_table(FILE *f, const qoi_frames_hdr *h) {
	unsigned char b[QOI_FRAMES_ENTRY_SIZE];
	unsigned int i, table_end;
	long file_size;

	if (fseek(f, 0, SEEK_END) != 0) {
		return QOI_FRAMES_ERR_IO;
	}
	file_size = ftell(f);
	if (file_size < 0) {
		return QOI_FRAMES_ERR_IO;
	}
	table_end = qoi_frames_table_end(h);

	for (i = 0; i < h->frame_count; i++) {
		unsigned int offset, size;

		if (fseek(f, (long)(QOI_FRAMES_HEADER_SIZE + i * QOI_FRAMES_ENTRY_SIZE), SEEK_SET) != 0) {
			return QOI_FRAMES_ERR_IO;
		}
		if (fread(b, 1, QOI_FRAMES_ENTRY_SIZE, f) != QOI_FRAMES_ENTRY_SIZE) {
			return QOI_FRAMES_ERR_IO;
		}

		offset = qoi_frames_get_32(b + 0);
		size = qoi_frames_get_32(b + 4);

		if (
			offset < table_end || size < QOI_HEADER_SIZE + sizeof(qoi_padding) ||
			(long)offset > file_size || (long)size > file_size - (long)offset
		) {
			return QOI_FRAMES_ERR_FORMAT;
		}
	}

	return 0;
}

int qoi_frames_create(const char *filename, const qoi_desc *desc,
	unsigned int capacity) {
	unsigned char header[QOI_FRAMES_HEADER_SIZE];
	qoi_frames_hdr h;
	FILE *f;
	unsigned int table_size, i;

	if (filename == NULL || !qoi_frames_desc_valid(desc)) {
		return QOI_FRAMES_ERR_ARG;
	}
	if (capacity == 0 || capacity > QOI_FRAMES_CAPACITY_MAX) {
		return QOI_FRAMES_ERR_ARG;
	}

	h.frame_count = 0;
	h.capacity = capacity;
	h.width = desc->width;
	h.height = desc->height;
	h.channels = desc->channels;
	h.colorspace = desc->colorspace;
	qoi_frames_put_header(header, &h);

	table_size = capacity * QOI_FRAMES_ENTRY_SIZE;

	f = fopen(filename, "wb");
	if (!f) {
		return QOI_FRAMES_ERR_IO;
	}

	if (
		fwrite(header, 1, QOI_FRAMES_HEADER_SIZE, f) != QOI_FRAMES_HEADER_SIZE
	) {
		fclose(f);
		return QOI_FRAMES_ERR_IO;
	}

	/* Zero-fill the table. Writing it in chunks keeps stack use constant. */
	{
		static const unsigned char zero[QOI_FRAMES_ENTRY_SIZE] = {0,0,0,0,0,0,0,0};
		for (i = 0; i < table_size; i += QOI_FRAMES_ENTRY_SIZE) {
			if (fwrite(zero, 1, QOI_FRAMES_ENTRY_SIZE, f) != QOI_FRAMES_ENTRY_SIZE) {
				fclose(f);
				return QOI_FRAMES_ERR_IO;
			}
		}
	}

	if (fflush(f) != 0 || ferror(f)) {
		fclose(f);
		return QOI_FRAMES_ERR_IO;
	}
	fclose(f);
	return 0;
}

int qoi_frames_append(const char *filename, const void *data,
	const qoi_desc *desc) {
	unsigned char entry[QOI_FRAMES_ENTRY_SIZE];
	qoi_frames_hdr h;
	unsigned char *encoded;
	FILE *f;
	int encoded_size, ret = 0;
	long offset;

	if (filename == NULL || data == NULL || !qoi_frames_desc_valid(desc)) {
		return QOI_FRAMES_ERR_ARG;
	}

	f = fopen(filename, "r+b");
	if (!f) {
		return QOI_FRAMES_ERR_IO;
	}

	ret = qoi_frames_read_header(f, &h);
	if (ret != 0) {
		fclose(f);
		return ret;
	}

	/* The frame must belong to this container's sequence. Checking before
	encoding leaves the file untouched on a mismatch. */
	if (
		desc->width != h.width || desc->height != h.height ||
		desc->channels != h.channels || desc->colorspace != h.colorspace
	) {
		fclose(f);
		return QOI_FRAMES_ERR_MISMATCH;
	}
	if (h.frame_count >= h.capacity) {
		fclose(f);
		return QOI_FRAMES_ERR_FULL;
	}

	encoded = (unsigned char *)qoi_encode(data, desc, &encoded_size);
	if (!encoded) {
		fclose(f);
		return QOI_FRAMES_ERR_ARG;
	}

	/* New frame goes at the end of the file; nothing before it changes. */
	if (fseek(f, 0, SEEK_END) != 0) {
		QOI_FREE(encoded);
		fclose(f);
		return QOI_FRAMES_ERR_IO;
	}
	offset = ftell(f);
	if (offset < 0 || offset > 0x7fffffffL ||
		encoded_size > 0x7fffffffL - offset) {
		QOI_FREE(encoded);
		fclose(f);
		return QOI_FRAMES_ERR_IO;
	}

	if (fwrite(encoded, 1, encoded_size, f) != (size_t)encoded_size) {
		QOI_FREE(encoded);
		fclose(f);
		return QOI_FRAMES_ERR_IO;
	}
	QOI_FREE(encoded);

	/* Fill the new table slot, then bump the counter. The counter is the
	validity marker, so it is written last. */
	qoi_frames_put_32(entry + 0, (unsigned int)offset);
	qoi_frames_put_32(entry + 4, (unsigned int)encoded_size);
	if (
		fseek(f, (long)(QOI_FRAMES_HEADER_SIZE +
			h.frame_count * QOI_FRAMES_ENTRY_SIZE), SEEK_SET) != 0 ||
		fwrite(entry, 1, QOI_FRAMES_ENTRY_SIZE, f) != QOI_FRAMES_ENTRY_SIZE
	) {
		fclose(f);
		return QOI_FRAMES_ERR_IO;
	}

	{
		unsigned char counter[4];
		qoi_frames_put_32(counter, h.frame_count + 1);
		if (
			fseek(f, 8, SEEK_SET) != 0 ||
			fwrite(counter, 1, 4, f) != 4 ||
			fflush(f) != 0 || ferror(f)
		) {
			fclose(f);
			return QOI_FRAMES_ERR_IO;
		}
	}

	fclose(f);
	return encoded_size;
}

int qoi_frames_write(const char *filename, const void *const *frames,
	const qoi_desc *desc, int frame_count) {
	unsigned char header[QOI_FRAMES_HEADER_SIZE];
	unsigned char *table = NULL;
	unsigned char **encoded = NULL;
	int *sizes = NULL;
	qoi_frames_hdr h;
	FILE *f = NULL;
	unsigned int total, offset;
	int i, ret = 0;

	if (
		filename == NULL || frames == NULL || !qoi_frames_desc_valid(desc) ||
		frame_count <= 0 ||
		(unsigned int)frame_count > QOI_FRAMES_CAPACITY_MAX
	) {
		return QOI_FRAMES_ERR_ARG;
	}

	encoded = (unsigned char **)QOI_MALLOC(sizeof(unsigned char *) * frame_count);
	sizes = (int *)QOI_MALLOC(sizeof(int) * frame_count);
	if (!encoded || !sizes) {
		QOI_FREE(encoded);
		QOI_FREE(sizes);
		return QOI_FRAMES_ERR_ARG;
	}
	for (i = 0; i < frame_count; i++) {
		encoded[i] = NULL;
		sizes[i] = 0;
	}

	for (i = 0; i < frame_count; i++) {
		encoded[i] = (unsigned char *)qoi_encode(frames[i], desc, &sizes[i]);
		if (!encoded[i]) {
			ret = QOI_FRAMES_ERR_ARG;
			goto done;
		}
	}

	h.frame_count = (unsigned int)frame_count;
	h.capacity = (unsigned int)frame_count;
	h.width = desc->width;
	h.height = desc->height;
	h.channels = desc->channels;
	h.colorspace = desc->colorspace;
	qoi_frames_put_header(header, &h);

	table = (unsigned char *)QOI_MALLOC(
		(size_t)frame_count * QOI_FRAMES_ENTRY_SIZE);
	if (!table) {
		ret = QOI_FRAMES_ERR_ARG;
		goto done;
	}

	offset = qoi_frames_table_end(&h);
	for (i = 0; i < frame_count; i++) {
		if (sizes[i] < 0 || offset > 0x7fffffffU - (unsigned int)sizes[i]) {
			ret = QOI_FRAMES_ERR_ARG;
			goto done;
		}
		qoi_frames_put_32(table + i * QOI_FRAMES_ENTRY_SIZE, offset);
		qoi_frames_put_32(table + i * QOI_FRAMES_ENTRY_SIZE + 4, (unsigned int)sizes[i]);
		offset += (unsigned int)sizes[i];
	}
	total = offset;

	f = fopen(filename, "wb");
	if (!f) {
		ret = QOI_FRAMES_ERR_IO;
		goto done;
	}

	if (
		fwrite(header, 1, QOI_FRAMES_HEADER_SIZE, f) != QOI_FRAMES_HEADER_SIZE ||
		fwrite(table, 1, (size_t)frame_count * QOI_FRAMES_ENTRY_SIZE, f) !=
			(size_t)frame_count * QOI_FRAMES_ENTRY_SIZE
	) {
		ret = QOI_FRAMES_ERR_IO;
		goto done;
	}

	for (i = 0; i < frame_count; i++) {
		if (fwrite(encoded[i], 1, (size_t)sizes[i], f) != (size_t)sizes[i]) {
			ret = QOI_FRAMES_ERR_IO;
			goto done;
		}
	}

	if (fflush(f) != 0 || ferror(f)) {
		ret = QOI_FRAMES_ERR_IO;
		goto done;
	}

	ret = (int)total;

done:
	if (f) {
		fclose(f);
	}
	if (encoded) {
		for (i = 0; i < frame_count; i++) {
			if (encoded[i]) {
				QOI_FREE(encoded[i]);
			}
		}
	}
	QOI_FREE(encoded);
	QOI_FREE(sizes);
	QOI_FREE(table);
	return ret;
}

int qoi_frames_info(const char *filename, qoi_desc *desc,
	unsigned int *frame_count_out, unsigned int *capacity_out) {
	qoi_frames_hdr h;
	FILE *f;
	int ret;

	if (filename == NULL) {
		return QOI_FRAMES_ERR_ARG;
	}

	f = fopen(filename, "rb");
	if (!f) {
		return QOI_FRAMES_ERR_IO;
	}
	ret = qoi_frames_read_header(f, &h);
	if (ret == 0) {
		ret = qoi_frames_check_table(f, &h);
	}
	fclose(f);
	if (ret != 0) {
		return ret;
	}

	if (desc) {
		desc->width = h.width;
		desc->height = h.height;
		desc->channels = h.channels;
		desc->colorspace = h.colorspace;
	}
	if (frame_count_out) {
		*frame_count_out = h.frame_count;
	}
	if (capacity_out) {
		*capacity_out = h.capacity;
	}
	return 0;
}

void *qoi_frames_read(const char *filename, int index, qoi_desc *desc,
	int channels) {
	unsigned char entry[QOI_FRAMES_ENTRY_SIZE];
	unsigned char *raw;
	void *pixels;
	qoi_frames_hdr h;
	unsigned int offset, size;
	FILE *f;
	int ret;

	if (
		filename == NULL || index < 0 ||
		(channels != 0 && channels != 3 && channels != 4)
	) {
		return NULL;
	}

	f = fopen(filename, "rb");
	if (!f) {
		return NULL;
	}
	ret = qoi_frames_read_header(f, &h);
	if (ret == 0 && (unsigned int)index >= h.frame_count) {
		ret = QOI_FRAMES_ERR_RANGE;
	}
	if (ret == 0 && fseek(f, (long)(QOI_FRAMES_HEADER_SIZE +
		(unsigned int)index * QOI_FRAMES_ENTRY_SIZE), SEEK_SET) != 0) {
		ret = QOI_FRAMES_ERR_IO;
	}
	if (ret == 0 && fread(entry, 1, QOI_FRAMES_ENTRY_SIZE, f) != QOI_FRAMES_ENTRY_SIZE) {
		ret = QOI_FRAMES_ERR_IO;
	}
	if (ret != 0) {
		fclose(f);
		return NULL;
	}

	offset = qoi_frames_get_32(entry + 0);
	size = qoi_frames_get_32(entry + 4);

	/* Defend against a single damaged slot even if the rest validated. */
	{
		long file_size;
		if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0) {
			fclose(f);
			return NULL;
		}
		if (
			(long)offset < (long)qoi_frames_table_end(&h) ||
			size < QOI_HEADER_SIZE + sizeof(qoi_padding) ||
			(long)offset > file_size ||
			(long)size > file_size - (long)offset
		) {
			fclose(f);
			return NULL;
		}
	}

	raw = (unsigned char *)QOI_MALLOC(size);
	if (!raw) {
		fclose(f);
		return NULL;
	}
	if (fseek(f, (long)offset, SEEK_SET) != 0 || fread(raw, 1, size, f) != size) {
		QOI_FREE(raw);
		fclose(f);
		return NULL;
	}
	fclose(f);

	pixels = qoi_decode(raw, (int)size, desc, channels);
	QOI_FREE(raw);
	return pixels;
}

#endif /* QOI_NO_STDIO */
#endif /* QOI_IMPLEMENTATION */
