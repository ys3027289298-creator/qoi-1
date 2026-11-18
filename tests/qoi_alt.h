/*
qoi_alt.h - Independent, byte-at-a-time QOI codec (verification path).

SPDX-License-Identifier: MIT

This is deliberately written in a different style from qoi.h to serve as an
independent cross-check of the production codec:

  * No struct, union or packed 32 bit pixel is ever formed. A pixel is just
    four slots in a flat uint8_t table: index_r[i], index_g[i], index_b[i],
    index_a[i]. There is therefore nothing whose value could depend on the
    host byte order - every operation is an arithmetic on individual bytes.
  * Header fields are assembled from explicit bytes with shifts, mirroring
    the on-disk big endian layout rather than the host layout.
  * Channel differences use `int` (the integer-promotion result type of
    uint8_t), so no signed-char overflow can occur.
  * The encode and decode loops are structured differently from qoi.h
    (single switch in the decoder, helper macros for byte emission).

Public API mirrors qoi_encode/qoi_decode under the alt_ prefix.
*/

#ifndef QOI_ALT_H
#define QOI_ALT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ALT_HEADER_SIZE   14
#define ALT_PADDING_SIZE   8
#define ALT_PIXELS_MAX     ((uint32_t)400000000u)
#define ALT_TABLE_SIZE     64

/* -------------------------------------------------------------------------- */
/* Big endian byte helpers - the wire format is BE regardless of the host.    */
/* -------------------------------------------------------------------------- */

static inline void alt_emit_be32(uint8_t *out, int *pos, uint32_t value) {
	out[(*pos)++] = (uint8_t)((value >> 24) & 0xffu);
	out[(*pos)++] = (uint8_t)((value >> 16) & 0xffu);
	out[(*pos)++] = (uint8_t)((value >>  8) & 0xffu);
	out[(*pos)++] = (uint8_t)( value        & 0xffu);
}

static inline uint32_t alt_fetch_be32(const uint8_t *in, int *pos) {
	uint32_t b0 = in[(*pos)++];
	uint32_t b1 = in[(*pos)++];
	uint32_t b2 = in[(*pos)++];
	uint32_t b3 = in[(*pos)++];
	return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

/* Spec hash, computed from four independent bytes. */
static inline int alt_hash(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
	return (int)((r * 3u + g * 5u + b * 7u + a * 11u) & (ALT_TABLE_SIZE - 1u));
}

static const uint8_t alt_endmark[ALT_PADDING_SIZE] = {0,0,0,0,0,0,0,1};

/* -------------------------------------------------------------------------- */
/* Encoder                                                                     */
/* -------------------------------------------------------------------------- */

static uint8_t *alt_encode(const uint8_t *pixels, uint32_t width,
                           uint32_t height, int channels, int *out_len) {
	if (pixels == NULL || out_len == NULL || width == 0 || height == 0 ||
	    (channels != 3 && channels != 4) ||
	    height >= ALT_PIXELS_MAX / width) {
		return NULL;
	}

	uint64_t count64 = (uint64_t)width * height;
	int    cap = (int)(count64 * (channels + 1) + ALT_HEADER_SIZE + ALT_PADDING_SIZE);
	uint8_t *stream = (uint8_t *)malloc((size_t)cap);
	if (stream == NULL) {
		return NULL;
	}

	int pos = 0;
	/* "qoif" written as its literal byte values - no multi-byte char constant. */
	alt_emit_be32(stream, &pos, 0x716f6966u);
	alt_emit_be32(stream, &pos, width);
	alt_emit_be32(stream, &pos, height);
	stream[pos++] = (uint8_t)channels;
	stream[pos++] = 0; /* sRGB */

	/* Flat color table; zero-initialized per spec. */
	uint8_t tab_r[ALT_TABLE_SIZE], tab_g[ALT_TABLE_SIZE];
	uint8_t tab_b[ALT_TABLE_SIZE], tab_a[ALT_TABLE_SIZE];
	memset(tab_r, 0, sizeof(tab_r));
	memset(tab_g, 0, sizeof(tab_g));
	memset(tab_b, 0, sizeof(tab_b));
	memset(tab_a, 0, sizeof(tab_a));

	uint8_t pr = 0, pg = 0, pb = 0, pa = 255;
	int run = 0;
	int total = (int)count64 * channels;
	int last_pixel_start = total - channels;

	#define ALT_FLUSH_RUN() do {                              \
		if (run > 0) {                                        \
			stream[pos++] = (uint8_t)(0xc0u | (run - 1));     \
			run = 0;                                          \
		}                                                      \
	} while (0)

	for (int pxc = 0; pxc < total; pxc += channels) {
		uint8_t r = pixels[pxc];
		uint8_t g = pixels[pxc + 1];
		uint8_t b = pixels[pxc + 2];
		uint8_t a = (channels == 4) ? pixels[pxc + 3] : pa;

		if (r == pr && g == pg && b == pb && a == pa) {
			run++;
			if (run == 62 || pxc == last_pixel_start) {
				ALT_FLUSH_RUN();
			}
		}
		else {
			ALT_FLUSH_RUN();
			int slot = alt_hash(r, g, b, a);
			if (tab_r[slot] == r && tab_g[slot] == g &&
			    tab_b[slot] == b && tab_a[slot] == a) {
				stream[pos++] = (uint8_t)slot; /* 00xxxxxx */
			}
			else {
				tab_r[slot] = r; tab_g[slot] = g;
				tab_b[slot] = b; tab_a[slot] = a;

				if (a == pa) {
					/* int diffs: byte values promote to int, range -255..255. */
					int dr = (int)r - (int)pr;
					int dg = (int)g - (int)pg;
					int db = (int)b - (int)pb;
					int drdg = dr - dg;
					int dbdg = db - dg;

					if (dr > -3 && dr < 2 && dg > -3 && dg < 2 &&
					    db > -3 && db < 2) {
						stream[pos++] = (uint8_t)(0x40u |
							((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2));
					}
					else if (drdg > -9 && drdg < 8 &&
					         dg   > -33 && dg  < 32 &&
					         dbdg > -9 && dbdg < 8) {
						stream[pos++] = (uint8_t)(0x80u | (dg + 32));
						stream[pos++] = (uint8_t)(((drdg + 8) << 4) |
						                          (dbdg + 8));
					}
					else {
						stream[pos++] = 0xfe;
						stream[pos++] = r;
						stream[pos++] = g;
						stream[pos++] = b;
					}
				}
				else {
					stream[pos++] = 0xff;
					stream[pos++] = r;
					stream[pos++] = g;
					stream[pos++] = b;
					stream[pos++] = a;
				}
			}
		}
		pr = r; pg = g; pb = b; pa = a;
	}
	#undef ALT_FLUSH_RUN

	memcpy(stream + pos, alt_endmark, ALT_PADDING_SIZE);
	pos += ALT_PADDING_SIZE;

	*out_len = pos;
	return stream;
}

/* -------------------------------------------------------------------------- */
/* Decoder                                                                     */
/* -------------------------------------------------------------------------- */

static uint8_t *alt_decode(const uint8_t *stream, int size,
                           uint32_t *out_w, uint32_t *out_h,
                           int *out_channels, int want_channels) {
	if (stream == NULL || size < ALT_HEADER_SIZE + ALT_PADDING_SIZE ||
	    (want_channels != 0 && want_channels != 3 && want_channels != 4)) {
		return NULL;
	}

	int pos = 0;
	uint32_t magic = alt_fetch_be32(stream, &pos);
	uint32_t width  = alt_fetch_be32(stream, &pos);
	uint32_t height = alt_fetch_be32(stream, &pos);
	int hdr_channels = stream[pos++];
	int colorspace   = stream[pos++];

	if (magic != 0x716f6966u || width == 0 || height == 0 ||
	    hdr_channels < 3 || hdr_channels > 4 || colorspace > 1 ||
	    height >= ALT_PIXELS_MAX / width) {
		return NULL;
	}

	int channels = (want_channels == 0) ? hdr_channels : want_channels;
	uint64_t count64 = (uint64_t)width * height;
	int total = (int)count64 * channels;
	uint8_t *pixels = (uint8_t *)malloc((size_t)total);
	if (pixels == NULL) {
		return NULL;
	}

	uint8_t tab_r[ALT_TABLE_SIZE], tab_g[ALT_TABLE_SIZE];
	uint8_t tab_b[ALT_TABLE_SIZE], tab_a[ALT_TABLE_SIZE];
	memset(tab_r, 0, sizeof(tab_r));
	memset(tab_g, 0, sizeof(tab_g));
	memset(tab_b, 0, sizeof(tab_b));
	memset(tab_a, 0, sizeof(tab_a));

	uint8_t r = 0, g = 0, b = 0, a = 255;
	int run = 0;
	int chunk_end = size - ALT_PADDING_SIZE;

	for (int pxc = 0; pxc < total; pxc += channels) {
		if (run > 0) {
			run--;
		}
		else if (pos < chunk_end) {
			uint8_t tag = stream[pos++];
			switch (tag) {
				case 0xfe: /* QOI_OP_RGB */
					r = stream[pos++];
					g = stream[pos++];
					b = stream[pos++];
					break;
				case 0xff: /* QOI_OP_RGBA */
					r = stream[pos++];
					g = stream[pos++];
					b = stream[pos++];
					a = stream[pos++];
					break;
				default: {
					int hi = tag & 0xc0u;
					if (hi == 0x00u) { /* QOI_OP_INDEX */
						int slot = tag & 0x3fu;
						r = tab_r[slot];
						g = tab_g[slot];
						b = tab_b[slot];
						a = tab_a[slot];
					}
					else if (hi == 0x40u) { /* QOI_OP_DIFF */
						r = (uint8_t)(r + ((tag >> 4) & 0x03u) - 2);
						g = (uint8_t)(g + ((tag >> 2) & 0x03u) - 2);
						b = (uint8_t)(b +  (tag       & 0x03u) - 2);
					}
					else if (hi == 0x80u) { /* QOI_OP_LUMA */
						uint8_t second = stream[pos++];
						int dg = (tag & 0x3fu) - 32;
						r = (uint8_t)(r + dg - 8 + ((second >> 4) & 0x0fu));
						g = (uint8_t)(g + dg);
						b = (uint8_t)(b + dg - 8 +  (second       & 0x0fu));
					}
					else { /* QOI_OP_RUN */
						run = tag & 0x3fu;
					}
				}
			}
			int slot = alt_hash(r, g, b, a);
			tab_r[slot] = r; tab_g[slot] = g;
			tab_b[slot] = b; tab_a[slot] = a;
		}

		pixels[pxc]     = r;
		pixels[pxc + 1] = g;
		pixels[pxc + 2] = b;
		if (channels == 4) {
			pixels[pxc + 3] = a;
		}
	}

	*out_w = width;
	*out_h = height;
	*out_channels = channels;
	return pixels;
}

#endif /* QOI_ALT_H */
