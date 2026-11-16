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

-- Endianness

The canonical QOI byte stream stores the two 32 bit header fields in big
endian order and starts with the magic "qoif". This implementation keeps
that format as the default and adds an explicitly selected little endian
stream variant:

	QOI_ENDIAN_BIG     canonical stream: magic "qoif", width/height BE
	QOI_ENDIAN_LITTLE  variant:         magic "qoil", width/height LE

Exactly one of these macros may be defined to 1 when compiling the
implementation; when neither is defined, QOI_ENDIAN_BIG is used. The two
variants share the same chunk encoding. Chunk payloads are byte oriented
and therefore completely endian neutral; only the magic and the two 32 bit
header fields differ. The pixel payload is always stored as r,g,b,a in
that order, independent of the host byte order.

The magic makes the order of a stream detectable: "qoif" means the fields
are big endian, "qoil" means they are little endian. A stream whose magic
and header fields disagree was written with mixed endianness; it is
reported by qoi_probe_endian() as QOI_ORDER_MIXED and rejected by
qoi_decode(). Correctly tagged streams of either order are accepted by
qoi_decode()/qoi_read() regardless of the order the library was built for,
so files stay portable between the two configurations.

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


/* -- Endian configuration --------------------------------------------------

A build selects the byte order used for newly encoded header fields with one
of these macros, defined before including this header:

    QOI_ENDIAN_BIG     write the canonical big endian header (default)
    QOI_ENDIAN_LITTLE  write the little endian header variant

They are mutually exclusive; defining both is a compile time error. Without
either macro the library builds in big endian mode. The value is not
inspected, so both -DQOI_ENDIAN_LITTLE and -DQOI_ENDIAN_LITTLE=1 work.

Both configurations also read either variant. The variant is recorded in the
magic: "qoif" marks big endian width/height fields, "qoil" marks little
endian ones. Files therefore stay interchangeable between the two builds.
The chunk payload, including the pixel bytes, is endian neutral. */

#if defined(QOI_ENDIAN_BIG) && defined(QOI_ENDIAN_LITTLE)
	#error "define only one of QOI_ENDIAN_BIG and QOI_ENDIAN_LITTLE"
#endif

/* Runtime byte-order values. */
#define QOI_ORDER_NONE   0
#define QOI_ORDER_BIG    1
#define QOI_ORDER_LITTLE 2
#define QOI_ORDER_MIXED  3

/* Return the byte order this build uses when encoding: QOI_ORDER_BIG or
QOI_ORDER_LITTLE. */
int qoi_build_endian(void);

/* Return the byte order of the machine executing the code: QOI_ORDER_BIG
or QOI_ORDER_LITTLE. This is detected from memory layout, not from the build
configuration, and is independent of qoi_build_endian(). */
int qoi_host_endian(void);

/* Read the header of a QOI stream and report the byte order it was written
in: QOI_ORDER_BIG for "qoif" with big endian fields, QOI_ORDER_LITTLE for
"qoil" with little endian fields, QOI_ORDER_MIXED when the magic and the
field order disagree (a stream written with mixed endianness), or
QOI_ORDER_NONE when the data is too short, the magic is unknown, or the
header is invalid in every interpretation.

A stream is classified as mixed when its header is not structurally valid in
the order announced by the magic but is valid when the width and height
fields are read in the opposite order, or when only one of the two fields is
consistent with the announced order. qoi_decode() rejects such streams. */
int qoi_probe_endian(const void *data, int size);

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned char channels;
	unsigned char colorspace;
} qoi_desc;

/* Encode raw RGB or RGBA pixels into a QOI image with an explicitly chosen
header byte order, independent of the build configuration. order must be
QOI_ORDER_BIG or QOI_ORDER_LITTLE. In every other respect this behaves like
qoi_encode(); with order equal to qoi_build_endian() the output is byte
identical to qoi_encode(). */
void *qoi_encode_endian(const void *data, const qoi_desc *desc, int order,
	int *out_len);

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
#define QOI_MAGIC_LE \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'l'))
#define QOI_HEADER_SIZE 14

/* The byte order this build writes with. The default preserves the original
canonical stream byte for byte. Defining QOI_ENDIAN_LITTLE (empty or to any
value) selects the little endian header variant; defining QOI_ENDIAN_BIG
selects the canonical one explicitly. */
#if defined(QOI_ENDIAN_LITTLE)
	#define QOI_ENCODE_ORDER QOI_ORDER_LITTLE
#else
	#define QOI_ENCODE_ORDER QOI_ORDER_BIG
#endif
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

/* -----------------------------------------------------------------------------
Explicit endian accessors.

All multi-byte fields of the stream go through these functions; no code below
relies on the host byte order. The 32 bit header fields are stored in the
order announced by the magic. Pixel buffers are byte sequences in r,g,b,a
order, read and written byte by byte. */

static void qoi_write_u32_be(unsigned char *bytes, int *p, unsigned int v) {
	bytes[(*p)++] = (unsigned char)((v >> 24) & 0xff);
	bytes[(*p)++] = (unsigned char)((v >> 16) & 0xff);
	bytes[(*p)++] = (unsigned char)((v >>  8) & 0xff);
	bytes[(*p)++] = (unsigned char)( v        & 0xff);
}

static void qoi_write_u32_le(unsigned char *bytes, int *p, unsigned int v) {
	bytes[(*p)++] = (unsigned char)( v        & 0xff);
	bytes[(*p)++] = (unsigned char)((v >>  8) & 0xff);
	bytes[(*p)++] = (unsigned char)((v >> 16) & 0xff);
	bytes[(*p)++] = (unsigned char)((v >> 24) & 0xff);
}

static unsigned int qoi_read_u32_be(const unsigned char *bytes, int *p) {
	unsigned int a = bytes[(*p)++];
	unsigned int b = bytes[(*p)++];
	unsigned int c = bytes[(*p)++];
	unsigned int d = bytes[(*p)++];
	return a << 24 | b << 16 | c << 8 | d;
}

static unsigned int qoi_read_u32_le(const unsigned char *bytes, int *p) {
	unsigned int a = bytes[(*p)++];
	unsigned int b = bytes[(*p)++];
	unsigned int c = bytes[(*p)++];
	unsigned int d = bytes[(*p)++];
	return d << 24 | c << 16 | b << 8 | a;
}

static unsigned int qoi_read_u32_at(const unsigned char *bytes, int offset,
	int order)
{
	int p = offset;
	return (order == QOI_ORDER_LITTLE) ?
		qoi_read_u32_le(bytes, &p) : qoi_read_u32_be(bytes, &p);
}

static void qoi_write_u32(unsigned char *bytes, int *p, unsigned int v,
	int order)
{
	if (order == QOI_ORDER_LITTLE) {
		qoi_write_u32_le(bytes, p, v);
	}
	else {
		qoi_write_u32_be(bytes, p, v);
	}
}

static unsigned int qoi_read_u32(const unsigned char *bytes, int *p, int order) {
	return (order == QOI_ORDER_LITTLE) ?
		qoi_read_u32_le(bytes, p) : qoi_read_u32_be(bytes, p);
}

/* Pixel accessors. Every pixel byte read from or written to a stream goes
through these helpers; they name the channel position explicitly instead of
relying on the host's byte order or on the layout of the pixel union. */
static unsigned char qoi_read_channel(const unsigned char *bytes, int px_pos,
	int channel)
{
	return bytes[px_pos + channel];
}

static void qoi_write_channel(unsigned char *bytes, int px_pos, int channel,
	unsigned char v)
{
	bytes[px_pos + channel] = v;
}

/* Compare two pixels. This uses the union's native word, which never enters
the stream and therefore has no endianness significance. */
static int qoi_px_same(qoi_rgba_t a, qoi_rgba_t b) {
	return a.v == b.v;
}

/* Read one pixel from a byte oriented r,g,b,a buffer. `prev` supplies the
alpha value for 3 channel pixels, which do not carry alpha. */
static qoi_rgba_t qoi_px_read(const unsigned char *pixels, int px_pos,
	int channels, qoi_rgba_t prev)
{
	qoi_rgba_t px;

	px.rgba.r = qoi_read_channel(pixels, px_pos, 0);
	px.rgba.g = qoi_read_channel(pixels, px_pos, 1);
	px.rgba.b = qoi_read_channel(pixels, px_pos, 2);
	px.rgba.a = (channels == 4) ?
		qoi_read_channel(pixels, px_pos, 3) : prev.rgba.a;
	return px;
}

/* Write one pixel to a byte oriented r,g,b,a buffer. The channel order is
fixed by the format, independent of the endianness of the stream header. */
static void qoi_px_write(unsigned char *pixels, int px_pos, int channels,
	qoi_rgba_t px)
{
	qoi_write_channel(pixels, px_pos, 0, px.rgba.r);
	qoi_write_channel(pixels, px_pos, 1, px.rgba.g);
	qoi_write_channel(pixels, px_pos, 2, px.rgba.b);
	if (channels == 4) {
		qoi_write_channel(pixels, px_pos, 3, px.rgba.a);
	}
}

/* Validate width/height/channels/colorspace exactly like the decoder does. */
static int qoi_header_fields_ok(unsigned int width, unsigned int height,
	unsigned char channels, unsigned char colorspace)
{
	return
		width != 0 && height != 0 &&
		channels >= 3 && channels <= 4 &&
		colorspace <= 1 &&
		height < QOI_PIXELS_MAX / width;
}

/* A single dimension on its own is plausible for a real image. */
static int qoi_dim_plausible(unsigned int v) {
	return v != 0 && v <= QOI_PIXELS_MAX;
}

int qoi_build_endian(void) {
	return QOI_ENCODE_ORDER;
}

int qoi_host_endian(void) {
	union {
		unsigned int v;
		unsigned char b[sizeof(unsigned int)];
	} probe;
	probe.v = 1;
	return (probe.b[0] == 1) ? QOI_ORDER_LITTLE : QOI_ORDER_BIG;
}

int qoi_probe_endian(const void *data, int size) {
	const unsigned char *bytes = (const unsigned char *)data;
	unsigned int magic;
	unsigned int w_announced, h_announced, w_other, h_other;
	int order, other;

	if (data == NULL || size < QOI_HEADER_SIZE) {
		return QOI_ORDER_NONE;
	}

	magic = qoi_read_u32_at(bytes, 0, QOI_ORDER_BIG);
	if (magic == QOI_MAGIC) {
		order = QOI_ORDER_BIG;
	}
	else if (magic == QOI_MAGIC_LE) {
		order = QOI_ORDER_LITTLE;
	}
	else {
		return QOI_ORDER_NONE;
	}

	other = (order == QOI_ORDER_BIG) ? QOI_ORDER_LITTLE : QOI_ORDER_BIG;
	w_announced = qoi_read_u32_at(bytes, 4, order);
	h_announced = qoi_read_u32_at(bytes, 8, order);
	w_other     = qoi_read_u32_at(bytes, 4, other);
	h_other     = qoi_read_u32_at(bytes, 8, other);

	if (qoi_header_fields_ok(w_announced, h_announced, bytes[12], bytes[13])) {
		return order;
	}

	/* The header is not valid in the order its magic announces. If it is
	structurally valid in the opposite order, the magic comes from one
	endianness and both fields from the other: a mixed endian stream. */
	if (qoi_header_fields_ok(w_other, h_other, bytes[12], bytes[13])) {
		return QOI_ORDER_MIXED;
	}

	/* Also catch streams where only one of the two 32 bit fields was
	written in the other order: no interpretation is fully valid, but each
	field on its own is plausible under a different order. */
	if (bytes[12] >= 3 && bytes[12] <= 4 && bytes[13] <= 1 && (
		(qoi_dim_plausible(w_announced) && qoi_dim_plausible(h_other)) ||
		(qoi_dim_plausible(w_other) && qoi_dim_plausible(h_announced))
	)) {
		return QOI_ORDER_MIXED;
	}

	return QOI_ORDER_NONE;
}

void *qoi_encode_endian(const void *data, const qoi_desc *desc, int order,
	int *out_len)
{
	int i, max_size, p, run;
	int px_len, px_end, px_pos, channels;
	unsigned char *bytes;
	const unsigned char *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px, px_prev;
	unsigned int magic;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		(order != QOI_ORDER_BIG && order != QOI_ORDER_LITTLE) ||
		!qoi_header_fields_ok(desc->width, desc->height, desc->channels,
			desc->colorspace)
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

	magic = (order == QOI_ORDER_LITTLE) ? QOI_MAGIC_LE : QOI_MAGIC;
	/* The magic is four ASCII bytes in natural order ("qoif"/"qoil"); only
	the two integer fields below follow the selected byte order. */
	qoi_write_u32(bytes, &p, magic, QOI_ORDER_BIG);
	qoi_write_u32(bytes, &p, desc->width, order);
	qoi_write_u32(bytes, &p, desc->height, order);
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
		px = qoi_px_read(pixels, px_pos, channels, px_prev);

		if (qoi_px_same(px, px_prev)) {
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

			if (qoi_px_same(index[index_pos], px)) {
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

void *qoi_encode(const void *data, const qoi_desc *desc, int *out_len) {
	return qoi_encode_endian(data, desc, QOI_ENCODE_ORDER, out_len);
}

void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels) {
	const unsigned char *bytes;
	unsigned int header_magic, magic_be;
	int order;
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

	magic_be = qoi_read_u32_at(bytes, 0, QOI_ORDER_BIG);
	if (magic_be == QOI_MAGIC) {
		order = QOI_ORDER_BIG;
	}
	else if (magic_be == QOI_MAGIC_LE) {
		order = QOI_ORDER_LITTLE;
	}
	else {
		return NULL;
	}

	p = 0;
	/* The magic is always four ASCII bytes in natural order. */
	header_magic = qoi_read_u32_be(bytes, &p);
	desc->width = qoi_read_u32(bytes, &p, order);
	desc->height = qoi_read_u32(bytes, &p, order);
	desc->channels = bytes[p++];
	desc->colorspace = bytes[p++];

	if (
		!qoi_header_fields_ok(desc->width, desc->height, desc->channels,
			desc->colorspace) ||
		header_magic != ((order == QOI_ORDER_LITTLE) ? QOI_MAGIC_LE :
			QOI_MAGIC)
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

		qoi_px_write(pixels, px_pos, channels, px);
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
