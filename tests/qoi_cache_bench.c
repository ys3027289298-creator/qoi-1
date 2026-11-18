/* Measurements for the configurable QOI encoder cache.

Produces, for four synthetic materials (pixel art, gradient, photo, random):
  A. hit rate, size ratio and encode time for every (size, policy) configuration,
  B. a cache-size sweep curve (16/32/64) for both policies,
  C. the fixed memory bound, and
  D. parameter-validation behaviour.

Timing is wall-clock over repeated encodes of the same buffer; the fastest of
`REPS` batches is reported to reduce scheduler noise.

Build (from the repository root):
	cc -std=gnu99 -O2 -I. tests/qoi_cache_bench.c -o /tmp/qoi_cache_bench
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"
#include "materials.h"

#define W 128
#define H 128
#define CH 4
#define REPS 200

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Returns the fastest of REPS encodes; fills *st from the final encode. */
static double time_encode(unsigned char *px, qoi_desc *d, qoi_encode_cfg *cfg,
	qoi_encode_stats *st, int *out_len, void **out_enc)
{
	int rep, len = 0;
	double best = 1e30;
	void *enc = NULL;
	for (rep = 0; rep < REPS; rep++) {
		double t0 = now_ms();
		enc = qoi_encode_options(px, d, &len, cfg, st);
		{
			double dt = now_ms() - t0;
			if (dt < best) best = dt;
		}
		free(enc);
	}
	enc = qoi_encode_options(px, d, &len, cfg, st);
	*out_len = len;
	*out_enc = enc;
	return best;
}

/* Supplementary: an image built so that the fixed 16-slot assignment collides.
16 colors share hash & 63 classes only within four hash % 16 buckets, so
fixed/16 (and fixed/32) thrash while LRU keeps all 16. Included so the policy
dimension is measured where it actually matters; it is not one of the four
required materials. */
static unsigned char *make_collision_probe(int w, int h) {
	unsigned char pal[16][4];
	int n = 0, bi, off, i;
	unsigned char *p = (unsigned char *)malloc((size_t)w * h * 4);
	for (bi = 0; bi < 4; bi++) {
		for (off = 0; off < 4; off++) {
			unsigned int want = (unsigned int)(bi + 16 * off);
			int r, g, b, found = 0;
			for (r = 0; r < 256 && !found; r++) {
				for (g = 0; g < 256 && !found; g++) {
					for (b = 0; b < 256 && !found; b++) {
						int q, dup = 0;
						unsigned int hv = (unsigned int)r * 3u + (unsigned int)g * 5u +
							(unsigned int)b * 7u + 255u * 11u;
						if ((hv & 63u) != want) continue;
						for (q = 0; q < n; q++) {
							if (pal[q][0] == r && pal[q][1] == g && pal[q][2] == b) dup = 1;
						}
						if (dup) continue;
						pal[n][0] = (unsigned char)r;
						pal[n][1] = (unsigned char)g;
						pal[n][2] = (unsigned char)b;
						pal[n][3] = 255;
						n++;
						found = 1;
					}
				}
			}
		}
	}
	for (i = 0; i < w * h; i++) {
		int k = i % 16;
		memcpy(p + (size_t)i * 4, pal[k], 4);
	}
	return p;
}

int main(void) {
	const size_t raw = (size_t)W * H * CH;
	int m;

	printf("QOI configurable cache measurements: %dx%d RGBA (raw %lu bytes), %d reps\n",
		W, H, (unsigned long)raw, REPS);

	/* --- D: illegal parameters ------------------------------------------- */
	printf("\n=== illegal configuration handling ===\n");
	{
		static const int bad_size[] = {-1, 0x7fffffff, 1, 15, 17, 48, 63, 65, 100};
		static const int bad_policy[] = {-1, 2, 100};
		static const int bad_report[] = {-1, 2, 100};
		int i;
		for (i = 0; i < (int)(sizeof(bad_size) / sizeof(bad_size[0])); i++) {
			qoi_encode_cfg c;
			qoi_encode_stats st;
			int len = 4321;
			void *e;
			c.cache_size = bad_size[i];
			c.cache_policy = QOI_CACHE_FIXED;
			c.report = 0;
			memset(&st, 0xff, sizeof(st));
			e = qoi_encode_options(NULL, NULL, &len, &c, &st);
			printf("  size=%-11d valid=%d encode=%s stats_zeroed=%s out_len_kept=%s\n",
				bad_size[i], qoi_encode_cfg_valid(&c),
				e ? "accepted" : "NULL",
				(st.pixels == 0 && st.cache_bytes == 0) ? "yes" : "no",
				(len == 4321) ? "yes" : "no");
		}
		for (i = 0; i < (int)(sizeof(bad_policy) / sizeof(bad_policy[0])); i++) {
			qoi_encode_cfg c;
			void *e;
			c.cache_size = 32;
			c.cache_policy = bad_policy[i];
			c.report = 0;
			e = qoi_encode_options(NULL, NULL, NULL, &c, NULL);
			printf("  policy=%-9d valid=%d encode=%s\n", bad_policy[i],
				qoi_encode_cfg_valid(&c), e ? "accepted" : "NULL");
		}
		for (i = 0; i < (int)(sizeof(bad_report) / sizeof(bad_report[0])); i++) {
			qoi_encode_cfg c;
			c.cache_size = 32;
			c.cache_policy = QOI_CACHE_LRU;
			c.report = bad_report[i];
			printf("  report=%-9d valid=%d\n", bad_report[i], qoi_encode_cfg_valid(&c));
		}
	}
	printf("  NULL cfg (defaults: 64/fixed/no report) valid=%d\n",
		qoi_encode_cfg_valid(NULL));

	/* Cache memory is fixed and independent of image and configuration. */
	{
		qoi_encode_cfg c;
		qoi_encode_stats st;
		unsigned char one[4] = {0, 0, 0, 255};
		qoi_desc d;
		int len;
		void *e;
		c.cache_size = 16;
		c.cache_policy = QOI_CACHE_LRU;
		c.report = 1;
		d.width = 1;
		d.height = 1;
		d.channels = 4;
		d.colorspace = QOI_SRGB;
		e = qoi_encode_options(one, &d, &len, &c, &st);
		printf("  fixed cache state: %u bytes (cap %d), 1x1 image reported %u bytes\n",
			st.cache_bytes, QOI_CACHE_BYTES_MAX, st.cache_bytes);
		free(e);
	}

	/* --- A: full matrix, per material ------------------------------------ */
	{
		static const int sizes[3] = {16, 32, 64};
		static const int policies[2] = {QOI_CACHE_FIXED, QOI_CACHE_LRU};
		static const char *pname[2] = {"fixed", "lru"};

		printf("\n=== A. per-material matrix (hit rate = OP_INDEX / cache lookups) ===\n");
		printf("%-9s %-6s %-5s | %8s | %10s | %10s | %10s\n",
			"material", "policy", "size", "hit rate", "ratio", "bytes", "enc us");
		printf("----------------------------------------------------------------------------\n");

		for (m = 0; m < 4; m++) {
			unsigned char *px = qoi_test_gen(m)(W, H, CH);
			qoi_desc d;
			int si, pi;
			d.width = W;
			d.height = H;
			d.channels = CH;
			d.colorspace = QOI_SRGB;

			for (pi = 0; pi < 2; pi++) {
				for (si = 0; si < 3; si++) {
					qoi_encode_cfg c;
					qoi_encode_stats st;
					int len;
					void *enc;
					double ms;
					c.cache_size = sizes[si];
					c.cache_policy = policies[pi];
					c.report = 1;
					ms = time_encode(px, &d, &c, &st, &len, &enc);
					printf("%-9s %-6s %-5d | %7.2f%% | %9.4f%% | %10d | %10.2f\n",
						qoi_test_name(m), pname[pi], sizes[si],
						st.index_hit_rate * 100.0,
						100.0 * len / (double)raw, len, ms * 1000.0);
					free(enc);
				}
			}
			printf("\n");
			free(px);
		}
	}

	/* --- B: cache-size sweep curve --------------------------------------- */
	printf("=== B. cache-size sweep (hit rate %%, ratio %%, enc us) ===\n");
	printf("%-9s %-6s %3s | %8s %9s %9s\n",
		"material", "policy", "n", "hit rate", "ratio", "enc us");
	printf("-----------------------------------------------------\n");
	for (m = 0; m < 4; m++) {
		unsigned char *px = qoi_test_gen(m)(W, H, CH);
		qoi_desc d;
		int si, pi;
		d.width = W;
		d.height = H;
		d.channels = CH;
		d.colorspace = QOI_SRGB;
		for (pi = 0; pi < 2; pi++) {
			for (si = 0; si < 3; si++) {
				static const int sizes[3] = {16, 32, 64};
				qoi_encode_cfg c;
				qoi_encode_stats st;
				int len;
				void *enc;
				double ms;
				c.cache_size = sizes[si];
				c.cache_policy = pi ? QOI_CACHE_LRU : QOI_CACHE_FIXED;
				c.report = 1;
				ms = time_encode(px, &d, &c, &st, &len, &enc);
				printf("%-9s %-6s %3d | %7.2f%% %8.4f%% %9.2f\n",
					qoi_test_name(m), pi ? "lru" : "fixed", sizes[si],
					st.index_hit_rate * 100.0, 100.0 * len / (double)raw, ms * 1000.0);
				free(enc);
			}
		}
		printf("\n");
		free(px);
	}

	/* --- C: supplementary collision probe --------------------------------- */
	printf("=== C. hash-collision probe (policy matters here; not a required material) ===\n");
	printf("%-9s %-6s %3s | %8s %9s %9s\n",
		"material", "policy", "n", "hit rate", "ratio", "enc us");
	printf("-----------------------------------------------------\n");
	{
		unsigned char *px = make_collision_probe(W, H);
		qoi_desc d;
		int si, pi;
		d.width = W;
		d.height = H;
		d.channels = 4;
		d.colorspace = QOI_SRGB;
		for (pi = 0; pi < 2; pi++) {
			for (si = 0; si < 3; si++) {
				static const int sizes[3] = {16, 32, 64};
				qoi_encode_cfg c;
				qoi_encode_stats st;
				int len;
				void *enc;
				double ms;
				c.cache_size = sizes[si];
				c.cache_policy = pi ? QOI_CACHE_LRU : QOI_CACHE_FIXED;
				c.report = 1;
				ms = time_encode(px, &d, &c, &st, &len, &enc);
				printf("%-9s %-6s %3d | %7.2f%% %8.4f%% %9.2f\n",
					"collide", pi ? "lru" : "fixed", sizes[si],
					st.index_hit_rate * 100.0, 100.0 * len / (double)raw, ms * 1000.0);
				free(enc);
			}
		}
		free(px);
	}

	return 0;
}
