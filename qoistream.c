/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT


qoistream - encode raw pixels to QOI inside a fixed memory budget

A batch-farm worker gets a hard per-job memory allowance. An 8K frame is
126 MiB of RGBA, which does not fit a 512 MiB job together with
qoi_encode()'s worst-case output buffer (5 bytes per pixel, another 158 MiB).
qoistream therefore reads the input in chunks, encodes it in bounded memory
and writes the QOI byte stream to numbered fragments, which are concatenated
in order afterwards. The result is byte-for-byte identical to qoi_encode() on
the whole image.

The chunk, staging buffer and fragment sizes are derived from the quota. Each
fragment is closed at an input offset chosen by that run, and a checkpoint
(encoder state, output offset and a hash of the consumed input) is written
right after it. A killed run resumes from the last completed fragment instead
of starting over. Because the checkpoint records an input position rather than
a position in the segmentation, the retry may use a different quota: the new
run cuts the remaining input into fragments of its own size and still produces
exactly the same bytes.

Usage:

	qoistream --input <raw> --width <w> --height <h> [options]

	--input <file>        raw pixel file, width*height*channels bytes
	--output <file>       final QOI file (default: <input>.qoi)
	--width <n>           image width in pixels
	--height <n>          image height in pixels
	--channels <3|4>      channels per pixel (default: 4)
	--colorspace <0|1>    0 = sRGB, 1 = linear (default: 0)
	--quota <bytes>       hard memory allowance (default: 512 MiB)
	--quota-mb <n>        same, in MiB, for convenience
	--chunk <bytes>       read chunk size (default: derived from the quota)
	--buf <bytes>         QOI staging buffer size (default: derived)
	--segment <bytes>     target output bytes per fragment (default: derived)
	--workdir <dir>       fragment directory (default: <output>.parts)
	--verify              stream-decode the result and compare it to the input
	--keep                keep the fragment directory after assembly
	--quiet               only print the summary line
	--help                this text

Exit status: 0 on success, 1 on a usage/IO/consistency error, 2 if the memory
budget cannot be met (the encoding is never attempted in that case).

Work dir layout:

	part-000000.qoi ...   consecutive fragments of the QOI byte stream
	state.bin             checkpoint: encoder state, offsets, input hash
	manifest              image parameters

A checkpoint only counts if the manifest matches this image, the parts on disk
cover exactly the checkpointed prefix, and the input prefix still hashes to the
recorded value; otherwise the fragments are discarded and encoding starts from
scratch. The work dir is removed after the output has been assembled.
*/

/* The driver uses clock_gettime(); ask for the POSIX declarations even under
-std=c99, where <time.h> otherwise hides them. */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 199309L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

#ifdef _WIN32
	#include <direct.h>
	#define QS_MKDIR(p) _mkdir(p)
	#define QS_STAT struct _stat
	#define QS_STATFN _stat
#else
	#include <unistd.h>
	#define QS_MKDIR(p) mkdir((p), 0777)
	#define QS_STAT struct stat
	#define QS_STATFN stat
#endif

#define QS_CHUNK_MIN       (4u << 10)
#define QS_CHUNK_MAX       (8u << 20)
#define QS_STAGE_DEFAULT   (1u << 16)
#define QS_STAGE_MAX       (1u << 20)
#define QS_QUOTA_DEFAULT   (512ull << 20)
#define QS_PATH_MAX        4096
#define QS_MANIFEST_MAGIC  "qoistream 2\n"
#define QS_STATE_MAGIC     0x71737463u /* "qstc" */
#define QS_STATE_VERSION   2u

/* Memory that is not one of the two big buffers: control structs, the
checkpoint, stdio buffers for the open files, the C stack and the loader's
mappings. The bulk of it is measured at startup (VmHWM before anything is
allocated); QS_FIXED_MARGIN covers growth after that measurement, and the sum
is charged against the quota so the printed total is an upper bound on the
process watermark rather than a guess. */
#define QS_FIXED_MARGIN   (512ull << 10)

typedef struct {
	const char *input;
	const char *output;
	const char *workdir;
	unsigned int width;
	unsigned int height;
	int channels;
	int colorspace;
	unsigned long long quota;
	unsigned int chunk;
	unsigned int stage;
	unsigned long long segment;
	int verify;
	int quiet;
	int keep;                       /* keep the fragments after assembly */
} qs_options;

/* Buffers whose sizes are part of the memory budget. */
typedef struct {
	qoi_stream_encoder enc;
	unsigned char *chunk;
	unsigned char *stage;
	unsigned int chunk_cap;
	unsigned int stage_cap;
	unsigned long long bytes;
} qs_memory;

typedef struct {
	FILE *f;
	unsigned int index;
	unsigned long long total;
	unsigned long long here;
	const char *dir;
} qs_sink;

typedef struct {
	unsigned int parts;             /* fragments on disk for this image */
	unsigned long long out_bytes;
	int resumed;                    /* continued from a checkpoint */
	int assembled_only;             /* stream was already complete */
} qs_run;

/* state.bin: fixed header, then the library's state blob. finished != 0 means
the byte stream is complete and only assembly is left. */
typedef struct {
	unsigned int parts;
	unsigned int finished;
	unsigned long long out_bytes;
	unsigned long long consumed;
	unsigned long long input_hash;
	int state_len;
	unsigned char state[QOI_STREAM_STATE_SIZE];
} qs_checkpoint;

/* ---------------------------------------------------------------------------
Output and small file helpers */

static int qs_join(char *dst, size_t cap, const char *a, const char *b) {
	size_t na = strlen(a), nb = strlen(b);
	if (na + nb + 1 > cap) {
		return 0;
	}
	memcpy(dst, a, na);
	memcpy(dst + na, b, nb + 1);
	return 1;
}

static int qs_fail_msg(const char *what, const char *detail) {
	fprintf(stderr, "qoistream: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	return 1;
}

static int qs_part_path(char *dst, size_t cap, const char *dir, unsigned int n) {
	char tail[32];
	sprintf(tail, "/part-%06u.qoi", n);
	return qs_join(dst, cap, dir, tail);
}

static int qs_work_path(char *dst, size_t cap, const char *dir, const char *name) {
	return qs_join(dst, cap, dir, name);
}

static long long qs_file_size(const char *path) {
	QS_STAT st;
	if (QS_STATFN(path, &st) != 0) {
		return -1;
	}
	return (long long)st.st_size;
}

static int qs_write_file(const char *path, const void *data, size_t len) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return 0;
	}
	if (len && fwrite(data, 1, len, f) != len) {
		fclose(f);
		return 0;
	}
	if (fflush(f) != 0 || ferror(f)) {
		fclose(f);
		return 0;
	}
	return fclose(f) == 0;
}

/* Read a whole file; returns malloc'd data (NUL terminated) or NULL. Only used
for the manifest and the checkpoint, both of which have fixed small sizes. */
static unsigned char *qs_read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	long sz;
	unsigned char *buf;
	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = (unsigned char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[sz] = 0;
	*len = (size_t)sz;
	return buf;
}

static int qs_mkdir_p(const char *path) {
	char tmp[QS_PATH_MAX];
	size_t i, n;
	if (!path || !*path) {
		return 0;
	}
	n = strlen(path);
	if (n >= sizeof tmp) {
		return 0;
	}
	memcpy(tmp, path, n + 1);
	for (i = 1; i < n; i++) {
		if (tmp[i] == '/' || tmp[i] == '\\') {
			char keep = tmp[i];
			tmp[i] = 0;
			(void)QS_MKDIR(tmp);
			tmp[i] = keep;
		}
	}
	(void)QS_MKDIR(tmp);
	return 1;
}

/* ---------------------------------------------------------------------------
Sink: append bytes to the current part file */

static int qs_sink_write(void *user, const void *bytes, int len) {
	qs_sink *s = (qs_sink *)user;
	if (len <= 0) {
		return 0;
	}
	if (fwrite(bytes, 1, (size_t)len, s->f) != (size_t)len) {
		return 1;
	}
	s->total += (unsigned long long)len;
	s->here += (unsigned long long)len;
	return 0;
}

static int qs_part_open(qs_sink *s, unsigned int index) {
	char path[QS_PATH_MAX];
	if (!qs_part_path(path, sizeof path, s->dir, index)) {
		return 0;
	}
	if (s->f) {
		if (fflush(s->f) != 0 || fclose(s->f) != 0) {
			s->f = NULL;
			return 0;
		}
		s->f = NULL;
	}
	s->f = fopen(path, "wb");
	if (!s->f) {
		return 0;
	}
	s->index = index;
	s->here = 0;
	return 1;
}

static int qs_part_close(qs_sink *s) {
	if (!s->f) {
		return 1;
	}
	if (fflush(s->f) != 0 || ferror(s->f)) {
		fclose(s->f);
		s->f = NULL;
		return 0;
	}
	if (fclose(s->f) != 0) {
		s->f = NULL;
		return 0;
	}
	s->f = NULL;
	return 1;
}

/* ---------------------------------------------------------------------------
Input hash

The checkpoint records a hash of the input prefix it has covered, so a resumed
run can prove that the fragments on disk still describe the file it is about
to continue reading. FNV-1a over single bytes does not depend on how the input
was split into reads, so a hash taken while encoding and a hash taken while
resuming are comparable even when the two runs use different chunk sizes. */

#define QS_HASH_INIT 0xcbf29ce484222325ull

static unsigned long long qs_hash_update(unsigned long long h, const void *buf, size_t n) {
	const unsigned char *p = (const unsigned char *)buf;
	while (n > 0) {
		h ^= (unsigned long long)*p++;
		h *= 0x100000001b3ull;
		n--;
	}
	return h;
}

/* Hash the first len bytes of path, reading through buf. */
static int qs_hash_file(const char *path, unsigned long long len,
	unsigned char *buf, size_t cap, unsigned long long *out
) {
	FILE *f = fopen(path, "rb");
	unsigned long long h = QS_HASH_INIT;
	if (!f) {
		return 0;
	}
	while (len > 0) {
		size_t want = len > (unsigned long long)cap ? cap : (size_t)len;
		if (fread(buf, 1, want, f) != want) {
			fclose(f);
			return 0;
		}
		h = qs_hash_update(h, buf, want);
		len -= (unsigned long long)want;
	}
	fclose(f);
	*out = h;
	return 1;
}

/* ---------------------------------------------------------------------------
Checkpoint and manifest */

static unsigned int qs_crc32(const unsigned char *p, size_t n) {
	unsigned int c = 0xffffffffu;
	size_t i;
	int k;
	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (unsigned int)(-(int)(c & 1)));
		}
	}
	return ~c;
}

static void qs_put32(unsigned char *b, size_t *p, unsigned int v) {
	b[(*p)++] = (unsigned char)(v & 0xff);
	b[(*p)++] = (unsigned char)((v >> 8) & 0xff);
	b[(*p)++] = (unsigned char)((v >> 16) & 0xff);
	b[(*p)++] = (unsigned char)((v >> 24) & 0xff);
}

static void qs_put64(unsigned char *b, size_t *p, unsigned long long v) {
	qs_put32(b, p, (unsigned int)(v & 0xffffffffu));
	qs_put32(b, p, (unsigned int)(v >> 32));
}

static unsigned int qs_get32(const unsigned char *b, size_t *p) {
	unsigned int v = (unsigned int)b[*p];
	v |= (unsigned int)b[*p + 1] << 8;
	v |= (unsigned int)b[*p + 2] << 16;
	v |= (unsigned int)b[*p + 3] << 24;
	*p += 4;
	return v;
}

static unsigned long long qs_get64(const unsigned char *b, size_t *p) {
	unsigned long long lo = qs_get32(b, p);
	unsigned long long hi = qs_get32(b, p);
	return lo | (hi << 32);
}

/* Layout: magic, version, parts, finished, out_bytes, consumed, input_hash,
state_len, reserved, state[QOI_STREAM_STATE_SIZE], crc32. */
#define QS_CKPT_HEAD (4u * 6u + 8u * 3u)
#define QS_CKPT_BYTES (QS_CKPT_HEAD + QOI_STREAM_STATE_SIZE + 4u)

static int qs_checkpoint_write(const char *dir, const qs_checkpoint *ck) {
	unsigned char blob[QS_CKPT_BYTES];
	char path[QS_PATH_MAX], tmp[QS_PATH_MAX];
	size_t p = 0;
	int written;
	if (!qs_work_path(path, sizeof path, dir, "/state.bin") ||
		!qs_join(tmp, sizeof tmp, path, ".tmp")) {
		return 0;
	}
	qs_put32(blob, &p, QS_STATE_MAGIC);
	qs_put32(blob, &p, QS_STATE_VERSION);
	qs_put32(blob, &p, ck->parts);
	qs_put32(blob, &p, ck->finished);
	qs_put64(blob, &p, ck->out_bytes);
	qs_put64(blob, &p, ck->consumed);
	qs_put64(blob, &p, ck->input_hash);
	qs_put32(blob, &p, (unsigned int)ck->state_len);
	qs_put32(blob, &p, 0);
	memcpy(blob + p, ck->state, QOI_STREAM_STATE_SIZE);
	p += QOI_STREAM_STATE_SIZE;
	qs_put32(blob, &p, qs_crc32(blob, p));
	/* Write a temporary file and rename it over the real one: rename is
	atomic, so a kill during the write leaves the previous checkpoint intact. */
	written = qs_write_file(tmp, blob, p);
	if (written) {
		written = rename(tmp, path) == 0;
	}
	if (!written) {
		remove(tmp);
	}
	return written;
}

static int qs_checkpoint_read(const char *dir, qs_checkpoint *ck) {
	char path[QS_PATH_MAX];
	unsigned char *blob;
	size_t len = 0, p = 0;
	int ok = 0;
	if (!qs_work_path(path, sizeof path, dir, "/state.bin")) {
		return 0;
	}
	blob = qs_read_file(path, &len);
	if (!blob) {
		return 0;
	}
	if (len == QS_CKPT_BYTES) {
		unsigned int magic = qs_get32(blob, &p);
		unsigned int version = qs_get32(blob, &p);
		unsigned int parts = qs_get32(blob, &p);
		unsigned int finished = qs_get32(blob, &p);
		unsigned long long out_bytes = qs_get64(blob, &p);
		unsigned long long consumed = qs_get64(blob, &p);
		unsigned long long input_hash = qs_get64(blob, &p);
		unsigned int state_len = qs_get32(blob, &p);
		(void)qs_get32(blob, &p);
		p += QOI_STREAM_STATE_SIZE;
		if (magic == QS_STATE_MAGIC && version == QS_STATE_VERSION &&
			state_len > 0 && state_len <= QOI_STREAM_STATE_SIZE &&
			finished <= 1 &&
			qs_get32(blob, &p) == qs_crc32(blob, QS_CKPT_BYTES - 4)) {
			ck->parts = parts;
			ck->finished = finished;
			ck->out_bytes = out_bytes;
			ck->consumed = consumed;
			ck->input_hash = input_hash;
			ck->state_len = (int)state_len;
			memcpy(ck->state, blob + QS_CKPT_HEAD, QOI_STREAM_STATE_SIZE);
			ok = 1;
		}
	}
	free(blob);
	return ok;
}

/* The manifest identifies the image, not the run parameters: a retry with a
different quota, chunk or fragment size is still the same job. */
static int qs_manifest_text(char *out, size_t cap, const qs_options *o) {
	int n = snprintf(out, cap,
		QS_MANIFEST_MAGIC
		"width=%u\nheight=%u\nchannels=%d\ncolorspace=%d\n",
		o->width, o->height, o->channels, o->colorspace
	);
	return n > 0 && (size_t)n < cap;
}

static int qs_manifest_write(const qs_options *o, const char *dir) {
	char path[QS_PATH_MAX], text[512];
	if (!qs_work_path(path, sizeof path, dir, "/manifest") ||
		!qs_manifest_text(text, sizeof text, o)) {
		return 0;
	}
	return qs_write_file(path, text, strlen(text));
}

static int qs_manifest_matches(const qs_options *o, const char *dir) {
	char path[QS_PATH_MAX], expect[512];
	unsigned char *have;
	size_t len = 0;
	int ok;
	if (!qs_work_path(path, sizeof path, dir, "/manifest") ||
		!qs_manifest_text(expect, sizeof expect, o)) {
		return 0;
	}
	have = qs_read_file(path, &len);
	if (!have) {
		return 0;
	}
	ok = len == strlen(expect) && memcmp(have, expect, len) == 0;
	free(have);
	return ok;
}

/* ---------------------------------------------------------------------------
Part bookkeeping */

/* Remove every part file, starting at first and stopping at the first gap. */
static void qs_drop_parts(const char *dir, unsigned int first) {
	unsigned int i;
	char path[QS_PATH_MAX];
	for (i = first; i < first + 1000000u; i++) {
		if (!qs_part_path(path, sizeof path, dir, i) || qs_file_size(path) < 0) {
			break;
		}
		remove(path);
	}
}

/* Longest consecutive run of parts present in dir and its total size. */
static void qs_scan_parts(const char *dir, unsigned int *count, unsigned long long *bytes) {
	unsigned int i;
	*count = 0;
	*bytes = 0;
	for (i = 0; ; i++) {
		char path[QS_PATH_MAX];
		long long sz;
		if (!qs_part_path(path, sizeof path, dir, i) || (sz = qs_file_size(path)) < 0) {
			break;
		}
		*count = i + 1;
		*bytes += (unsigned long long)sz;
	}
}

/* Forget every trace of a previous run in dir (used when the checkpoint does
not describe this image or does not match the parts on disk). */
static void qs_reset_workdir(const char *dir) {
	char path[QS_PATH_MAX];
	qs_drop_parts(dir, 0);
	if (qs_work_path(path, sizeof path, dir, "/state.bin")) {
		remove(path);
	}
	if (qs_work_path(path, sizeof path, dir, "/state.bin.tmp")) {
		remove(path);
	}
}

/* Reclaim the work dir after the output has been assembled. */
static void qs_clear_workdir(const char *dir) {
	char path[QS_PATH_MAX];
	qs_reset_workdir(dir);
	if (qs_work_path(path, sizeof path, dir, "/manifest")) {
		remove(path);
	}
#ifndef _WIN32
	(void)rmdir(dir);
#endif
}

/* ---------------------------------------------------------------------------
Budget arithmetic */

static int qs_parse_ull(const char *s, unsigned long long *out) {
	char *end = NULL;
	unsigned long long v;
	if (!s || !*s || *s == '-') {
		return 0;
	}
	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno != 0 || !end || *end != '\0') {
		return 0;
	}
	*out = v;
	return 1;
}

static int qs_parse_uint(const char *s, unsigned int *out) {
	unsigned long long v;
	if (!qs_parse_ull(s, &v) || v > 0xffffffffull) {
		return 0;
	}
	*out = (unsigned int)v;
	return 1;
}

static int qs_parse_int(const char *s, int *out) {
	unsigned long long v;
	if (!qs_parse_ull(s, &v) || v > 0x7fffffffull) {
		return 0;
	}
	*out = (int)v;
	return 1;
}

/* Shrink the chunk/staging sizes until the fixed overhead plus both buffers
fits the quota; returns the committed byte count or 0. fixed is the measured
baseline plus the margin, so the total is an upper bound on VmHWM. */
static unsigned long long qs_budget_alloc(
	qs_memory *m, unsigned int *chunk, unsigned int *stage,
	unsigned long long quota, unsigned long long fixed, int quiet
) {
	unsigned long long usable;
	unsigned int c = *chunk, s = *stage;

	m->chunk = NULL;
	m->stage = NULL;
	m->bytes = 0;

	if (quota <= fixed + QS_CHUNK_MIN + QOI_STREAM_MIN_BUF) {
		return 0;
	}
	usable = quota - fixed;

	/* Halve whichever buffer is currently larger, but never below the
	minimum it needs: the staging buffer must be able to hold a full chunk,
	and the read chunk must never become so small that it cannot be used. */
	while ((unsigned long long)c + (unsigned long long)s > usable) {
		if (c >= s) {
			if (c <= QS_CHUNK_MIN) {
				return 0;
			}
			c = c / 2u < QS_CHUNK_MIN ? QS_CHUNK_MIN : c / 2u;
		}
		else {
			if (s <= QOI_STREAM_MIN_BUF) {
				return 0;
			}
			s = s / 2u < QOI_STREAM_MIN_BUF ? QOI_STREAM_MIN_BUF : s / 2u;
		}
	}
	if (c > QS_CHUNK_MAX) {
		c = QS_CHUNK_MAX;
	}
	if (s < QOI_STREAM_MIN_BUF) {
		s = QOI_STREAM_MIN_BUF;
	}

	m->chunk = (unsigned char *)malloc(c);
	m->stage = (unsigned char *)malloc(s);
	if (!m->chunk || !m->stage) {
		free(m->chunk);
		free(m->stage);
		m->chunk = NULL;
		m->stage = NULL;
		return 0;
	}
	m->bytes = fixed + (unsigned long long)c + (unsigned long long)s;
	m->chunk_cap = c;
	m->stage_cap = s;
	*chunk = c;
	*stage = s;
	if (!quiet) {
		printf("budget: quota %llu B, fixed overhead %llu B, chunk %u B, "
			"stage %u B, total %llu B (%.1f%%)\n",
			quota, fixed, c, s, m->bytes,
			100.0 * (double)m->bytes / (double)quota);
	}
	return m->bytes;
}

/* Peak resident and peak virtual size of this process, in bytes, from
/proc/self/status. Returns 0 and zeroes the outputs where /proc is missing. */
static void qs_read_watermark(unsigned long long *hwm, unsigned long long *peak) {
	*hwm = 0;
	*peak = 0;
#ifdef __linux__
	{
		FILE *f = fopen("/proc/self/status", "r");
		char line[256];
		while (f && fgets(line, sizeof line, f)) {
			unsigned long long v;
			if (sscanf(line, "VmHWM: %llu kB", &v) == 1) {
				*hwm = v * 1024ull;
			}
			else if (sscanf(line, "VmPeak: %llu kB", &v) == 1) {
				*peak = v * 1024ull;
			}
		}
		if (f) {
			fclose(f);
		}
	}
#endif
}

static long long qs_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------------------
Segmentation

A fragment is cut after a fixed number of input bytes, chosen so that its
worst case output (5 bytes per pixel) stays around the target. The step is
relative to wherever the run starts reading, so a resumed run with a different
quota simply uses its own step from the resume point and still produces the
same byte stream. */

static unsigned long long qs_segment_step(const qs_options *o, unsigned long long px_len) {
	unsigned long long target = o->segment;
	unsigned long long step;

	if (target == 0) {
		target = o->quota / 3ull;
		if (target < 512ull * 1024) {
			target = 512ull * 1024;
		}
		if (target > 64ull * 1024 * 1024) {
			target = 64ull * 1024 * 1024;
		}
	}
	/* Clamp before multiplying: an absurd --segment must not overflow into a
	tiny step, which would produce one fragment per few pixels. */
	if (target > (unsigned long long)px_len + 1ull) {
		target = (unsigned long long)px_len + 1ull;
	}
	step = target * (unsigned long long)o->channels / 5ull;
	if (step < (unsigned long long)o->channels * 16ull) {
		step = (unsigned long long)o->channels * 16ull;
	}
	step -= step % (unsigned long long)o->channels;
	if (step > px_len) {
		step = px_len;
	}
	return step;
}

/* ---------------------------------------------------------------------------
Encoding with resume */

/* Decide whether the checkpoint on disk can be continued. Returns 1 and fills
run/offset/hash when it can. parts and parts_bytes describe the directory. */
static int qs_try_resume(
	const qs_options *o, qs_memory *mem, unsigned long long px_len,
	unsigned int parts, unsigned long long parts_bytes,
	qs_checkpoint *ck, qs_run *run, unsigned long long *offset,
	unsigned long long *hash
) {
	unsigned long long got;
	unsigned int i;
	unsigned long long prefix_bytes = 0;

	if (parts == 0 || !qs_manifest_matches(o, o->workdir) ||
		!qs_checkpoint_read(o->workdir, ck)) {
		return 0;
	}
	if (ck->parts == 0 || ck->parts > parts || ck->consumed > px_len ||
		ck->consumed % (unsigned long long)o->channels != 0) {
		return 0;
	}
	/* Exactly the first ck->parts fragments must exist and their total size
	must be the byte count the checkpoint recorded; a missing or truncated
	fragment invalidates the whole prefix. Fragments after it are an
	uncheckpointed tail and are simply dropped. */
	for (i = 0; i < ck->parts; i++) {
		char path[QS_PATH_MAX];
		long long sz;
		if (!qs_part_path(path, sizeof path, o->workdir, i) ||
			(sz = qs_file_size(path)) < 0) {
			return 0;
		}
		prefix_bytes += (unsigned long long)sz;
	}
	if (prefix_bytes != ck->out_bytes) {
		return 0;
	}
	/* Prove the input is still the one the fragments were produced from. */
	if (!qs_hash_file(o->input, ck->consumed, mem->chunk, mem->chunk_cap, &got) ||
		got != ck->input_hash) {
		return 0;
	}

	if (ck->finished && (ck->parts != parts || parts_bytes != ck->out_bytes)) {
		return 0;
	}
	run->assembled_only = ck->finished;
	run->resumed = 1;
	*offset = ck->consumed;
	*hash = got;
	return 1;
}

static int qs_encode(
	const qs_options *o, qs_memory *mem, qs_sink *sink, qs_run *run
) {
	qoi_desc desc;
	qs_checkpoint ck;
	unsigned int parts = 0;
	unsigned long long parts_bytes = 0;
	unsigned long long px_len = (unsigned long long)o->width * o->height * o->channels;
	unsigned long long step = qs_segment_step(o, px_len);
	unsigned long long offset = 0;
	unsigned long long next_cut;
	unsigned long long hash = QS_HASH_INIT;
	qoi_stream_encoder *enc = &mem->enc;
	FILE *in = NULL;
	unsigned int next_part = 0;
	int rc;

	memset(run, 0, sizeof(*run));
	memset(&ck, 0, sizeof ck);
	desc.width = o->width;
	desc.height = o->height;
	desc.channels = (unsigned char)o->channels;
	desc.colorspace = (unsigned char)o->colorspace;

	if ((unsigned long long)qs_file_size(o->input) != px_len) {
		char msg[160];
		sprintf(msg, "input is %lld bytes, %ux%u x %d needs %llu",
			qs_file_size(o->input), o->width, o->height, o->channels, px_len);
		return qs_fail_msg("input size mismatch", msg);
	}

	qs_scan_parts(o->workdir, &parts, &parts_bytes);

	if (qoi_stream_init(enc, &desc, mem->stage, (int)mem->stage_cap,
		qs_sink_write, sink) != 0) {
		return qs_fail_msg("stream init failed", NULL);
	}

	if (qs_try_resume(o, mem, px_len, parts, parts_bytes, &ck, run, &offset, &hash)) {
		if (run->assembled_only) {
			run->parts = ck.parts;
			run->out_bytes = ck.out_bytes;
			return 0;
		}
		/* Load the encoder state; it must land exactly on the recorded
		input offset. */
		if (qoi_stream_load_state(enc, ck.state, ck.state_len) != 0 ||
			(unsigned long long)qoi_stream_consumed(enc) != offset) {
			run->resumed = 0;
			run->assembled_only = 0;
			offset = 0;
			hash = QS_HASH_INIT;
		}
		else {
			next_part = ck.parts;
			sink->total = ck.out_bytes;
			/* Parts after the checkpoint were never checkpointed: redo. */
			qs_drop_parts(o->workdir, ck.parts);
			if (!o->quiet) {
				printf("resume: %llu/%llu input bytes, %u parts, %llu output bytes\n",
					offset, px_len, ck.parts, ck.out_bytes);
			}
		}
	}

	if (!run->resumed) {
		qs_reset_workdir(o->workdir);
		next_part = 0;
		sink->total = 0;
		offset = 0;
		hash = QS_HASH_INIT;
	}

	if (offset < px_len) {
		in = fopen(o->input, "rb");
		if (!in) {
			return qs_fail_msg("cannot read input", o->input);
		}
		if (offset > 0 && fseek(in, (long)offset, SEEK_SET) != 0) {
			fclose(in);
			return qs_fail_msg("cannot seek input to the resume offset", NULL);
		}
	}
	next_cut = offset + step;
	if (next_cut > px_len) {
		next_cut = px_len;
	}

	if (!qs_part_open(sink, next_part)) {
		if (in) {
			fclose(in);
		}
		return qs_fail_msg("cannot create part file in", o->workdir);
	}

	while (offset < px_len) {
		/* next_cut is an absolute anchor, not a per-iteration offset: the
		chunk buffer may be smaller than the step, so several reads fit in
		one fragment. The anchor is recomputed from `offset` only when a
		fragment closes. */
		unsigned long long limit;
		unsigned int want = mem->chunk_cap;
		int consumed = 0;

		limit = next_cut - offset;
		if ((unsigned long long)want > limit) {
			want = (unsigned int)limit;
		}
		if (fread(mem->chunk, 1, want, in) != want) {
			fclose(in);
			qs_part_close(sink);
			return qs_fail_msg("short read from input", o->input);
		}
		rc = qoi_stream_feed(enc, mem->chunk, (int)want, &consumed);
		/* The checkpoint hash covers exactly the input the encoder has
		consumed, so a resume can compare it against the same prefix. */
		hash = qs_hash_update(hash, mem->chunk, (size_t)consumed);
		offset += (unsigned long long)consumed;
		if (rc != 0) {
			fclose(in);
			qs_part_close(sink);
			return qs_fail_msg(rc == QOI_STREAM_SINK_FAILED
				? "write failed" : "encoder rejected input", NULL);
		}

		/* Close the fragment exactly at its planned boundary and checkpoint
		there. The last fragment stays open for the end marker. */
		if (offset >= px_len) {
			break;
		}
		if (offset < next_cut) {
			continue;
		}
		if (next_cut != offset) {
			fclose(in);
			qs_part_close(sink);
			return qs_fail_msg("internal: feed crossed a fragment boundary", NULL);
		}
		if (qoi_stream_flush(enc) != 0) {
			fclose(in);
			qs_part_close(sink);
			return qs_fail_msg("write failed", NULL);
		}
		if (!qs_part_close(sink)) {
			fclose(in);
			return qs_fail_msg("write failed", NULL);
		}
		ck.parts = next_part + 1;
		ck.finished = 0;
		ck.out_bytes = sink->total;
		ck.consumed = offset;
		ck.input_hash = hash;
		memset(ck.state, 0, sizeof ck.state);
		ck.state_len = qoi_stream_save_state(enc, ck.state, (int)sizeof ck.state);
		if (ck.state_len <= 0) {
			fclose(in);
			return qs_fail_msg("cannot serialize encoder state", NULL);
		}
		if (!qs_checkpoint_write(o->workdir, &ck)) {
			fclose(in);
			return qs_fail_msg("cannot write checkpoint in", o->workdir);
		}
		next_part++;
		if (!qs_part_open(sink, next_part)) {
			fclose(in);
			return qs_fail_msg("cannot create part file in", o->workdir);
		}
		next_cut = offset + step;
		if (next_cut > px_len) {
			next_cut = px_len;
		}
	}
	if (in) {
		fclose(in);
	}

	rc = qoi_stream_end(enc);
	if (rc != 0) {
		qs_part_close(sink);
		return qs_fail_msg(rc == QOI_STREAM_SINK_FAILED
			? "write failed" : "encoder could not finish the stream", NULL);
	}
	if (!qs_part_close(sink)) {
		return qs_fail_msg("write failed", NULL);
	}

	/* Record that the byte stream is complete: if the process dies during
	assembly, the next run skips encoding and only re-assembles. */
	ck.parts = next_part + 1;
	ck.finished = 1;
	ck.out_bytes = sink->total;
	ck.consumed = px_len;
	ck.input_hash = hash;
	ck.state_len = qoi_stream_save_state(enc, ck.state, (int)sizeof ck.state);
	if (ck.state_len <= 0) {
		return qs_fail_msg("cannot serialize encoder state", NULL);
	}
	if (!qs_checkpoint_write(o->workdir, &ck)) {
		return qs_fail_msg("cannot write checkpoint in", o->workdir);
	}

	run->parts = next_part + 1;
	run->out_bytes = sink->total;
	return 0;
}

/* ---------------------------------------------------------------------------
Streaming decode-and-compare, so --verify also fits the quota */

typedef struct {
	FILE *f;
	unsigned char *buf;
	size_t cap;
	size_t len;
	size_t pos;
	int eof;
} qs_reader;

static int qs_reader_fill(qs_reader *r) {
	if (r->pos < r->len) {
		return 1;
	}
	if (r->eof) {
		return 0;
	}
	r->len = fread(r->buf, 1, r->cap, r->f);
	r->pos = 0;
	if (r->len == 0) {
		r->eof = 1;
		return 0;
	}
	return 1;
}

static int qs_reader_byte(qs_reader *r, int *out) {
	if (!qs_reader_fill(r)) {
		return 0;
	}
	*out = r->buf[r->pos++];
	return 1;
}

/* Decode the QOI file and compare every pixel with the raw input, using only
the two buffers already charged to the budget. Returns 1 on success. */
static int qs_verify_stream(
	const qs_options *o, const char *qoi_path, qs_memory *mem
) {
	unsigned int index[64];
	unsigned char pxc[4] = {0, 0, 0, 255};
	int run = 0;
	unsigned long long n = (unsigned long long)o->width * o->height * o->channels;
	unsigned long long done = 0;
	FILE *in = NULL;
	qs_reader r;
	unsigned char header[14];
	int b1;
	int ok = 1;

	memset(index, 0, sizeof index);
	memset(&r, 0, sizeof r);

	r.f = fopen(qoi_path, "rb");
	if (!r.f) {
		return 0;
	}
	r.cap = mem->stage_cap;
	r.buf = mem->stage;
	if (fread(header, 1, 14, r.f) != 14) {
		fclose(r.f);
		return 0;
	}
	if (memcmp(header, "qoif", 4) != 0) {
		fclose(r.f);
		return 0;
	}
	{
		unsigned int w = (unsigned int)header[4] << 24 | (unsigned int)header[5] << 16 |
			(unsigned int)header[6] << 8 | header[7];
		unsigned int h = (unsigned int)header[8] << 24 | (unsigned int)header[9] << 16 |
			(unsigned int)header[10] << 8 | header[11];
		if (w != o->width || h != o->height ||
			header[12] != (unsigned char)o->channels || header[13] > 1) {
			fclose(r.f);
			return 0;
		}
	}

	in = fopen(o->input, "rb");
	if (!in) {
		fclose(r.f);
		return 0;
	}

	while (done < n && ok) {
		unsigned char *pix = mem->chunk;
		unsigned long long take = mem->chunk_cap;
		unsigned long long i;

		if (take > n - done) {
			take = n - done;
		}
		/* Compare whole pixels only; a chunk boundary may not split one. */
		take -= take % (unsigned long long)o->channels;
		if (take == 0) {
			ok = 0;
			break;
		}
		if (fread(pix, 1, (size_t)take, in) != (size_t)take) {
			ok = 0;
			break;
		}
		for (i = 0; i < take && ok; i += (unsigned int)o->channels) {
			if (run > 0) {
				run--;
			}
			else if (qs_reader_byte(&r, &b1)) {
				if (b1 == QOI_OP_RGB) {
					int cr, cg, cb;
					if (!qs_reader_byte(&r, &cr) || !qs_reader_byte(&r, &cg) ||
						!qs_reader_byte(&r, &cb)) {
						ok = 0;
						break;
					}
					pxc[0] = (unsigned char)cr;
					pxc[1] = (unsigned char)cg;
					pxc[2] = (unsigned char)cb;
				}
				else if (b1 == QOI_OP_RGBA) {
					int cr, cg, cb, ca;
					if (!qs_reader_byte(&r, &cr) || !qs_reader_byte(&r, &cg) ||
						!qs_reader_byte(&r, &cb) || !qs_reader_byte(&r, &ca)) {
						ok = 0;
						break;
					}
					pxc[0] = (unsigned char)cr;
					pxc[1] = (unsigned char)cg;
					pxc[2] = (unsigned char)cb;
					pxc[3] = (unsigned char)ca;
				}
				else if ((b1 & 0xc0) == 0x00) {
					unsigned int v = index[b1];
					pxc[0] = (unsigned char)(v & 0xff);
					pxc[1] = (unsigned char)((v >> 8) & 0xff);
					pxc[2] = (unsigned char)((v >> 16) & 0xff);
					pxc[3] = (unsigned char)((v >> 24) & 0xff);
				}
				else if ((b1 & 0xc0) == 0x40) {
					pxc[0] = (unsigned char)(pxc[0] + ((b1 >> 4) & 3) - 2);
					pxc[1] = (unsigned char)(pxc[1] + ((b1 >> 2) & 3) - 2);
					pxc[2] = (unsigned char)(pxc[2] + (b1 & 3) - 2);
				}
				else if ((b1 & 0xc0) == 0x80) {
					int b2, vg;
					if (!qs_reader_byte(&r, &b2)) {
						ok = 0;
						break;
					}
					vg = (b1 & 0x3f) - 32;
					pxc[0] = (unsigned char)(pxc[0] + vg - 8 + ((b2 >> 4) & 15));
					pxc[1] = (unsigned char)(pxc[1] + vg);
					pxc[2] = (unsigned char)(pxc[2] + vg - 8 + (b2 & 15));
				}
				else {
					run = b1 & 0x3f;
				}
				index[((unsigned int)pxc[0] * 3 + (unsigned int)pxc[1] * 5 +
					(unsigned int)pxc[2] * 7 + (unsigned int)pxc[3] * 11) & 63] =
					(unsigned int)pxc[0] | (unsigned int)pxc[1] << 8 |
					(unsigned int)pxc[2] << 16 | (unsigned int)pxc[3] << 24;
			}
			if (pix[i + 0] != pxc[0] || pix[i + 1] != pxc[1] ||
				pix[i + 2] != pxc[2] ||
				(o->channels == 4 && pix[i + 3] != pxc[3])) {
				ok = 0;
			}
		}
		done += take;
	}
	fclose(in);
	fclose(r.f);
	return ok;
}

/* ---------------------------------------------------------------------------
Assemble parts, in order, into the output file. The copy buffer is the chunk
buffer, so assembly stays inside the same budget. */

static int qs_assemble(
	const char *dir, unsigned int count, const char *output,
	unsigned char *copy, size_t cap
) {
	char tmp[QS_PATH_MAX];
	FILE *out;
	unsigned int i;
	int ok = 1;

	if (!qs_join(tmp, sizeof tmp, output, ".tmp")) {
		return 0;
	}
	out = fopen(tmp, "wb");
	if (!out) {
		return 0;
	}
	for (i = 0; i < count && ok; i++) {
		char part[QS_PATH_MAX];
		FILE *f;
		size_t got;
		if (!qs_part_path(part, sizeof part, dir, i) ||
			!(f = fopen(part, "rb"))) {
			ok = 0;
			break;
		}
		while ((got = fread(copy, 1, cap, f)) > 0) {
			if (fwrite(copy, 1, got, out) != got) {
				ok = 0;
				break;
			}
		}
		if (ferror(f)) {
			ok = 0;
		}
		fclose(f);
	}
	if (fflush(out) != 0 || ferror(out)) {
		ok = 0;
	}
	if (fclose(out) != 0) {
		ok = 0;
	}
	if (!ok) {
		remove(tmp);
		return 0;
	}
	(void)remove(output);
	if (rename(tmp, output) != 0) {
		remove(tmp);
		return 0;
	}
	return 1;
}

/* ---------------------------------------------------------------------------
Command line */

static void qs_usage(FILE *f) {
	fputs(
		"Usage: qoistream --input <raw> --width <w> --height <h> [options]\n"
		"  --output <file>     output QOI file (default: <input>.qoi)\n"
		"  --channels <3|4>    channels per pixel (default: 4)\n"
		"  --colorspace <0|1>  0 = sRGB, 1 = linear (default: 0)\n"
		"  --quota <bytes>     hard memory allowance (default: 512 MiB)\n"
		"  --quota-mb <n>      hard memory allowance in MiB\n"
		"  --chunk <bytes>     read chunk size (default: derived)\n"
		"  --buf <bytes>       QOI staging buffer size (default: derived)\n"
		"  --segment <bytes>   target output bytes per fragment (default: derived)\n"
		"  --workdir <dir>     fragment directory (default: <output>.parts)\n"
		"  --verify            stream-decode the result and compare to input\n"
		"  --keep              keep the fragment directory after assembly\n"
		"  --quiet             only print the summary line\n"
		"  --help              this text\n",
		f
	);
}

static int qs_parse_args(int argc, char **argv, qs_options *o) {
	static char out[QS_PATH_MAX];
	static char wd[QS_PATH_MAX];
	int i;
	unsigned long long v;

	memset(o, 0, sizeof(*o));
	o->channels = 4;
	o->colorspace = QOI_SRGB;
	o->quota = QS_QUOTA_DEFAULT;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			qs_usage(stdout);
			exit(0);
		}
		else if (!strcmp(a, "--verify")) { o->verify = 1; }
		else if (!strcmp(a, "--quiet")) { o->quiet = 1; }
		else if (!strcmp(a, "--keep")) { o->keep = 1; }
		else if (!strcmp(a, "--input") && i + 1 < argc) { o->input = argv[++i]; }
		else if (!strcmp(a, "--output") && i + 1 < argc) { o->output = argv[++i]; }
		else if (!strcmp(a, "--workdir") && i + 1 < argc) { o->workdir = argv[++i]; }
		else if (!strcmp(a, "--width") && i + 1 < argc) {
			if (!qs_parse_uint(argv[++i], &o->width)) return 0;
		}
		else if (!strcmp(a, "--height") && i + 1 < argc) {
			if (!qs_parse_uint(argv[++i], &o->height)) return 0;
		}
		else if (!strcmp(a, "--channels") && i + 1 < argc) {
			int c;
			if (!qs_parse_int(argv[++i], &c) || (c != 3 && c != 4)) return 0;
			o->channels = c;
		}
		else if (!strcmp(a, "--colorspace") && i + 1 < argc) {
			int c;
			if (!qs_parse_int(argv[++i], &c) || c < 0 || c > 1) return 0;
			o->colorspace = c;
		}
		else if (!strcmp(a, "--quota") && i + 1 < argc) {
			if (!qs_parse_ull(argv[++i], &o->quota)) return 0;
		}
		else if (!strcmp(a, "--quota-mb") && i + 1 < argc) {
			if (!qs_parse_ull(argv[++i], &v) || v > (0xffffffffull >> 20)) return 0;
			o->quota = v << 20;
		}
		else if (!strcmp(a, "--chunk") && i + 1 < argc) {
			if (!qs_parse_uint(argv[++i], &o->chunk) || o->chunk < QS_CHUNK_MIN) return 0;
		}
		else if (!strcmp(a, "--buf") && i + 1 < argc) {
			if (!qs_parse_uint(argv[++i], &o->stage) ||
				o->stage < QOI_STREAM_MIN_BUF) return 0;
		}
		else if (!strcmp(a, "--segment") && i + 1 < argc) {
			if (!qs_parse_ull(argv[++i], &o->segment) || o->segment == 0) return 0;
		}
		else {
			fprintf(stderr, "qoistream: unknown or incomplete option %s\n", a);
			return 0;
		}
	}

	if (!o->input || !o->width || !o->height) {
		fprintf(stderr, "qoistream: --input, --width and --height are required\n");
		return 0;
	}
	if (!o->output) {
		size_t n = strlen(o->input);
		if (n + 5 > sizeof out) return 0;
		memcpy(out, o->input, n);
		memcpy(out + n, ".qoi", 5);
		o->output = out;
	}
	if (!o->workdir) {
		size_t n = strlen(o->output);
		if (n + 7 > sizeof wd) return 0;
		memcpy(wd, o->output, n);
		memcpy(wd + n, ".parts", 7);
		o->workdir = wd;
	}
	return 1;
}

int main(int argc, char **argv) {
	qs_options o;
	qs_memory mem;
	qs_sink sink;
	qs_run run;
	unsigned long long px_len;
	unsigned long long measured;
	unsigned long long fixed;
	unsigned long long t0, t1;
	unsigned int chunk, stage;
	int rc;

	if (!qs_parse_args(argc, argv, &o)) {
		qs_usage(stderr);
		return 1;
	}

	if (o.height >= QOI_PIXELS_MAX / o.width ||
		(unsigned long long)o.width * o.height * o.channels > 0x7fffffffllu) {
		return qs_fail_msg("invalid image dimensions", NULL);
	}
	px_len = (unsigned long long)o.width * o.height * o.channels;

	/* Everything the process already holds (loader, stdio, stack) is measured
	now, before the two big buffers exist; QS_FIXED_MARGIN covers the small
	allocations that follow. Charging the sum against the quota makes the
	printed total an upper bound on VmHWM. */
	{
		unsigned long long base_hwm = 0, base_peak = 0;
		qs_read_watermark(&base_hwm, &base_peak);
		(void)base_peak;
		fixed = base_hwm + QS_FIXED_MARGIN;
	}

	/* Derive both buffers from the quota: the read chunk gets about a third
	of the usable budget, the staging buffer about a sixteenth, each capped so
	a huge quota does not lead to silly sizes. Explicit --chunk/--buf
	override this. */
	chunk = o.chunk;
	stage = o.stage;
	if (!chunk && !stage) {
		unsigned long long usable = o.quota > fixed ? o.quota - fixed : 0;
		unsigned long long c = usable / 3;
		unsigned long long s = usable / 16;
		if (c < QS_CHUNK_MIN) c = QS_CHUNK_MIN;
		if (c > QS_CHUNK_MAX) c = QS_CHUNK_MAX;
		if (s < QS_STAGE_DEFAULT) s = QS_STAGE_DEFAULT;
		if (s > QS_STAGE_MAX) s = QS_STAGE_MAX;
		chunk = (unsigned int)c;
		stage = (unsigned int)s;
	}
	else if (!chunk) {
		chunk = stage * 16 < QS_STAGE_DEFAULT ? QS_STAGE_DEFAULT : stage * 16;
	}
	else if (!stage) {
		stage = QS_STAGE_DEFAULT;
	}

	memset(&mem, 0, sizeof mem);
	memset(&sink, 0, sizeof sink);
	sink.dir = o.workdir;

	t0 = (unsigned long long)qs_now_ms();
	measured = qs_budget_alloc(&mem, &chunk, &stage, o.quota, fixed, o.quiet);
	if (measured == 0) {
		fprintf(stderr,
			"qoistream: a %llu byte quota cannot hold the encoder "
			"(more than %llu bytes are needed: %llu fixed, %u chunk, %u stage)\n",
			o.quota, fixed + QS_CHUNK_MIN + QOI_STREAM_MIN_BUF,
			fixed, QS_CHUNK_MIN, QOI_STREAM_MIN_BUF);
		return 2;
	}
	o.chunk = chunk;
	o.stage = stage;

	if (!qs_mkdir_p(o.workdir)) {
		free(mem.chunk);
		free(mem.stage);
		return qs_fail_msg("cannot create work dir", o.workdir);
	}

	{
		char mpath[QS_PATH_MAX];
		if (!qs_work_path(mpath, sizeof mpath, o.workdir, "/manifest")) {
			free(mem.chunk);
			free(mem.stage);
			return qs_fail_msg("work dir path too long", NULL);
		}
		if (qs_file_size(mpath) < 0) {
			if (!qs_manifest_write(&o, o.workdir)) {
				free(mem.chunk);
				free(mem.stage);
				return qs_fail_msg("cannot write manifest in", o.workdir);
			}
		}
		else if (!qs_manifest_matches(&o, o.workdir)) {
			unsigned int n;
			unsigned long long b;
			qs_scan_parts(o.workdir, &n, &b);
			fprintf(stderr,
				"qoistream: %s holds a checkpoint for a different image; "
				"discarding %u parts and starting over\n", o.workdir, n);
			qs_reset_workdir(o.workdir);
			if (!qs_manifest_write(&o, o.workdir)) {
				free(mem.chunk);
				free(mem.stage);
				return qs_fail_msg("cannot write manifest in", o.workdir);
			}
		}
	}

	rc = qs_encode(&o, &mem, &sink, &run);
	if (rc != 0) {
		free(mem.chunk);
		free(mem.stage);
		return rc;
	}

	{
		unsigned int count;
		unsigned long long bytes;
		qs_scan_parts(o.workdir, &count, &bytes);
		if (bytes != run.out_bytes || count != run.parts) {
			fprintf(stderr,
				"qoistream: parts hold %u files / %llu bytes, encoder wrote %u / %llu\n",
				count, bytes, run.parts, run.out_bytes);
			free(mem.chunk);
			free(mem.stage);
			return 1;
		}
		if (!qs_assemble(o.workdir, count, o.output, mem.chunk, mem.chunk_cap)) {
			free(mem.chunk);
			free(mem.stage);
			return qs_fail_msg("cannot assemble output", o.output);
		}
		if (o.keep) {
			/* Leave the fragments and the finished checkpoint in place. The
			next run with the same image re-assembles without encoding. */
			if (!o.quiet) {
				printf("kept: %s\n", o.workdir);
			}
		}
		else {
			qs_clear_workdir(o.workdir);
		}
	}

	t1 = (unsigned long long)qs_now_ms();
	{
		unsigned long long hwm = 0, vpeak = 0;
		qs_read_watermark(&hwm, &vpeak);
		printf("%s: %ux%u ch=%d, %llu raw -> %llu qoi (%.2f%%), %u parts, "
			"budget %llu B, VmHWM %llu B, VmPeak %llu B, quota %llu B, %.3f s%s\n",
			o.output, o.width, o.height, o.channels, px_len, run.out_bytes,
			px_len ? 100.0 * (double)run.out_bytes / (double)px_len : 0.0,
			run.parts, measured, hwm, vpeak, o.quota, (double)(t1 - t0) / 1000.0,
			run.resumed ? (run.assembled_only ? " (assembled)" : " (resumed)") : "");
	}

	if (o.verify) {
		if (!qs_verify_stream(&o, o.output, &mem)) {
			fprintf(stderr, "qoistream: verification FAILED\n");
			free(mem.chunk);
			free(mem.stage);
			return 1;
		}
		if (!o.quiet) {
			printf("verify: every decoded pixel matches %s\n", o.input);
		}
	}

	free(mem.chunk);
	free(mem.stage);
	return 0;
}
