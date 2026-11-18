/*
qoi_be.h - QOI codec running against an EMULATED big endian machine view.

SPDX-License-Identifier: MIT

Purpose: the production code used to form whole-pixel 32 bit words via a
union (qoi_rgba_t.v). On a little endian and a big endian machine the same
four bytes map to different integer values; any code whose *logic* depended
on that integer value would decode the same stream differently.

This file reproduces that word-oriented programming style but routes every
multi-byte memory access through byte-reversing accessors, so the abstract
machine behaves as if it were big endian while actually running on the
(little endian) host:

  alt_be_load32(p)  reads four bytes at p and returns the value a big endian
                    machine would see when loading those bytes as a uint32.
  alt_be_store32()  is the inverse.

The packed pixel word used here has the documented QOI layout R<<24|G<<16|
B<<8|A, i.e. exactly the integer a big endian machine gets from a native
4-byte load of an in-memory RGBA pixel. The color table and equality checks
operate on these words, as the original code did on qoi_rgba_t.v.

If the production codec is truly endian agnostic, this emulated-big-endian
codec must produce byte-identical streams and pixels.
*/

#ifndef QOI_BE_H
#define QOI_BE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- Emulated big endian memory accessors -------------------------------- */

static inline uint32_t alt_be_load32(const uint8_t *p) {
	/* Big endian load: first byte is the most significant. */
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline void alt_be_store32(uint8_t *p, uint32_t word) {
	p[0] = (uint8_t)(word >> 24);
	p[1] = (uint8_t)(word >> 16);
	p[2] = (uint8_t)(word >>  8);
	p[3] = (uint8_t)(word);
}

static inline uint32_t alt_be_load_be32(const uint8_t *p) {
	/* The wire format is itself big endian, so a BE machine loads it
	directly; the accessor is spelled out to keep the model explicit. */
	return alt_be_load32(p);
}

/* Word channel accessors on the emulated big endian packed pixel. */
static inline uint8_t alt_be_r(uint32_t w) { return (uint8_t)(w >> 24); }
static inline uint8_t alt_be_g(uint32_t w) { return (uint8_t)(w >> 16); }
static inline uint8_t alt_be_b(uint32_t w) { return (uint8_t)(w >>  8); }
static inline uint8_t alt_be_a(uint32_t w) { return (uint8_t)(w);       }

static inline uint32_t alt_be_make(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	return ((uint32_t)r << 24) | ((uint32_t)g << 16) |
	       ((uint32_t)b <<  8) |  (uint32_t)a;
}

static inline int alt_be_hash(uint32_t w) {
	return (int)((uint32_t)alt_be_r(w) * 3u +
	             (uint32_t)alt_be_g(w) * 5u +
	             (uint32_t)alt_be_b(w) * 7u +
	             (uint32_t)alt_be_a(w) * 11u) & 63;
}

static const uint8_t be_endmark[8] = {0,0,0,0,0,0,0,1};

/* --- Encoder -------------------------------------------------------------- */

static uint8_t *alt_be_encode(const uint8_t *pixels, uint32_t width,
                              uint32_t height, int channels, int *out_len) {
	if (pixels == NULL || out_len == NULL || width == 0 || height == 0 ||
	    (channels != 3 && channels != 4) ||
	    height >= 400000000u / width) {
		return NULL;
	}

	uint64_t count64 = (uint64_t)width * height;
	int cap = (int)(count64 * (channels + 1) + 14 + 8);
	uint8_t *stream = (uint8_t *)malloc((size_t)cap);
	if (!stream) {
		return NULL;
	}

	int pos = 0;
	/* Header: each field is a BE word in memory. */
	alt_be_store32(stream + pos, 0x716f6966u); pos += 4;
	alt_be_store32(stream + pos, width);       pos += 4;
	alt_be_store32(stream + pos, height);      pos += 4;
	stream[pos++] = (uint8_t)channels;
	stream[pos++] = 0;

	uint32_t table[64];
	memset(table, 0, sizeof(table));

	/* Initial pixel {0,0,0,255}; on the emulated BE machine this is the
	word 0x000000ff. */
	uint32_t prev = alt_be_make(0, 0, 0, 255);
	uint32_t cur  = prev;
	int run = 0;
	int total = (int)count64 * channels;
	int last = total - channels;

	for (int pxc = 0; pxc < total; pxc += channels) {
		uint8_t r = pixels[pxc];
		uint8_t g = pixels[pxc + 1];
		uint8_t b = pixels[pxc + 2];
		uint8_t a = (channels == 4) ? pixels[pxc + 3] : alt_be_a(prev);
		cur = alt_be_make(r, g, b, a);

		if (cur == prev) {
			run++;
			if (run == 62 || pxc == last) {
				stream[pos++] = (uint8_t)(0xc0 | (run - 1));
				run = 0;
			}
		}
		else {
			if (run > 0) {
				stream[pos++] = (uint8_t)(0xc0 | (run - 1));
				run = 0;
			}
			int slot = alt_be_hash(cur);
			if (table[slot] == cur) {
				stream[pos++] = (uint8_t)slot;
			}
			else {
				table[slot] = cur;
				if (alt_be_a(cur) == alt_be_a(prev)) {
					int dr = (int)alt_be_r(cur) - (int)alt_be_r(prev);
					int dg = (int)alt_be_g(cur) - (int)alt_be_g(prev);
					int db = (int)alt_be_b(cur) - (int)alt_be_b(prev);
					int drdg = dr - dg;
					int dbdg = db - dg;

					if (dr > -3 && dr < 2 && dg > -3 && dg < 2 &&
					    db > -3 && db < 2) {
						stream[pos++] = (uint8_t)(0x40 |
							((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2));
					}
					else if (drdg > -9 && drdg < 8 &&
					         dg > -33 && dg < 32 &&
					         dbdg > -9 && dbdg < 8) {
						stream[pos++] = (uint8_t)(0x80 | (dg + 32));
						stream[pos++] = (uint8_t)(((drdg + 8) << 4) | (dbdg + 8));
					}
					else {
						stream[pos++] = 0xfe;
						stream[pos++] = alt_be_r(cur);
						stream[pos++] = alt_be_g(cur);
						stream[pos++] = alt_be_b(cur);
					}
				}
				else {
					stream[pos++] = 0xff;
					stream[pos++] = alt_be_r(cur);
					stream[pos++] = alt_be_g(cur);
					stream[pos++] = alt_be_b(cur);
					stream[pos++] = alt_be_a(cur);
				}
			}
		}
		prev = cur;
	}

	memcpy(stream + pos, be_endmark, 8);
	pos += 8;
	*out_len = pos;
	return stream;
}

/* --- Decoder -------------------------------------------------------------- */

static uint8_t *alt_be_decode(const uint8_t *stream, int size,
                              uint32_t *out_w, uint32_t *out_h,
                              int *out_channels, int want_channels) {
	if (stream == NULL || size < 14 + 8 ||
	    (want_channels != 0 && want_channels != 3 && want_channels != 4)) {
		return NULL;
	}

	int pos = 0;
	uint32_t magic = alt_be_load_be32(stream + pos); pos += 4;
	uint32_t width  = alt_be_load_be32(stream + pos); pos += 4;
	uint32_t height = alt_be_load_be32(stream + pos); pos += 4;
	int hdr_channels = stream[pos++];
	int colorspace   = stream[pos++];

	if (magic != 0x716f6966u || width == 0 || height == 0 ||
	    hdr_channels < 3 || hdr_channels > 4 || colorspace > 1 ||
	    height >= 400000000u / width) {
		return NULL;
	}

	int channels = (want_channels == 0) ? hdr_channels : want_channels;
	uint64_t count64 = (uint64_t)width * height;
	int total = (int)count64 * channels;
	uint8_t *pixels = (uint8_t *)malloc((size_t)total);
	if (!pixels) {
		return NULL;
	}

	uint32_t table[64];
	memset(table, 0, sizeof(table));
	uint32_t px = alt_be_make(0, 0, 0, 255);
	int run = 0;
	int chunk_end = size - 8;

	for (int pxc = 0; pxc < total; pxc += channels) {
		if (run > 0) {
			run--;
		}
		else if (pos < chunk_end) {
			uint8_t tag = stream[pos++];
			if (tag == 0xfe) {
				/* Read bytes into named locals first: the order of evaluation
				of function arguments is unspecified, so passing stream[pos++]
				straight into alt_be_make() would be unsequenced side effects
				(undefined behavior) and could scramble the channels. */
				uint8_t nr = stream[pos++];
				uint8_t ng = stream[pos++];
				uint8_t nb_ = stream[pos++];
				px = alt_be_make(nr, ng, nb_, alt_be_a(px));
			}
			else if (tag == 0xff) {
				uint8_t nr = stream[pos++];
				uint8_t ng = stream[pos++];
				uint8_t nb_ = stream[pos++];
				uint8_t na = stream[pos++];
				px = alt_be_make(nr, ng, nb_, na);
			}
			else if ((tag & 0xc0) == 0x00) {
				px = table[tag & 0x3f];
			}
			else if ((tag & 0xc0) == 0x40) {
				uint8_t r = (uint8_t)(alt_be_r(px) + ((tag >> 4) & 0x03) - 2);
				uint8_t g = (uint8_t)(alt_be_g(px) + ((tag >> 2) & 0x03) - 2);
				uint8_t b = (uint8_t)(alt_be_b(px) +  (tag       & 0x03) - 2);
				px = alt_be_make(r, g, b, alt_be_a(px));
			}
			else if ((tag & 0xc0) == 0x80) {
				uint8_t second = stream[pos++];
				int dg = (tag & 0x3f) - 32;
				uint8_t r = (uint8_t)(alt_be_r(px) + dg - 8 + ((second >> 4) & 0x0f));
				uint8_t g = (uint8_t)(alt_be_g(px) + dg);
				uint8_t b = (uint8_t)(alt_be_b(px) + dg - 8 +  (second       & 0x0f));
				px = alt_be_make(r, g, b, alt_be_a(px));
			}
			else {
				run = tag & 0x3f;
			}
			table[alt_be_hash(px)] = px;
		}

		/* Emit bytes as a big endian machine would from the packed word. */
		pixels[pxc]     = alt_be_r(px);
		pixels[pxc + 1] = alt_be_g(px);
		pixels[pxc + 2] = alt_be_b(px);
		if (channels == 4) {
			pixels[pxc + 3] = alt_be_a(px);
		}
	}

	*out_w = width;
	*out_h = height;
	*out_channels = channels;
	return pixels;
}

#endif /* QOI_BE_H */
