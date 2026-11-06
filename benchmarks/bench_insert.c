/* bench_insert.c: xksm_memory_add throughput and per-call latency.
 * Text = placeholder tokens (hex words), about 256 bytes per memory, unique per record.
 * usage: bench_insert STORE [count=100000] [batch=1]   (batch > 1 uses xksm_memory_add_batch) */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "xksm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    long n = argc > 2 ? strtol(argv[2], NULL, 10) : 100000;
    long batch = argc > 3 ? strtol(argv[3], NULL, 10) : 1;
    if (n < 1 || batch < 1 || batch > 65536)
        return 2;
    xksm *x = NULL;
    if (xksm_open(argv[1], &x) != XKSM_OK) {
        fprintf(stderr, "open: %s\n", xksm_errmsg(x));
        return 1;
    }
    long calls = (n + batch - 1) / batch;
    double *lat = malloc(sizeof(double) * (size_t)calls);
    char (*text)[320] = malloc(sizeof *text * (size_t)batch);
    xksm_memory_in *in = calloc((size_t)batch, sizeof *in);
    xksm_id *ids = malloc(sizeof *ids * (size_t)batch);
    xksm_status *st = malloc(sizeof *st * (size_t)batch);
    if (!lat || !text || !in || !ids || !st)
        return 1;
    uint64_t r = 0x9e3779b97f4a7c15u;
    size_t bytes = 0;
    long dups = 0, c = 0;
    double t0 = now_s();
    for (long i = 0; i < n; c++) {
        long k = 0;
        for (; k < batch && i < n; ++k, ++i) {
            int o = snprintf(text[k], sizeof text[k], "rec%08ld", i);
            while (o < 250) {
                r ^= r << 13; r ^= r >> 7; r ^= r << 17;
                o += snprintf(text[k] + o, sizeof text[k] - (size_t)o, " w%05x",
                              (unsigned)(r >> 44) & 0xFFFFF);
            }
            in[k].category = XKSM_CAT_EPISODIC;
            in[k].text = text[k];
            in[k].text_len = (size_t)o;
            bytes += (size_t)o;
        }
        double a = now_s();
        xksm_status s = XKSM_OK;
        if (batch == 1)
            st[0] = xksm_memory_add(x, in, ids);
        else
            s = xksm_memory_add_batch(x, in, (size_t)k, ids, st);
        lat[c] = now_s() - a;
        if (s != XKSM_OK) {
            fprintf(stderr, "batch %ld: %s\n", c, xksm_errmsg(x));
            return 1;
        }
        for (long j = 0; j < k; ++j) {
            if (st[j] == XKSM_EXISTS)
                ++dups;
            else if (st[j] != XKSM_OK) {
                fprintf(stderr, "item %ld: %s\n", j, xksm_errmsg(x));
                return 1;
            }
        }
    }
    double total = now_s() - t0;
    qsort(lat, (size_t)calls, sizeof(double), cmp_d);
    printf("%ld adds in %ld calls of %ld (%ld dedup hits), %.1f MiB text, %.3f s: %.0f records/s\n",
           n, calls, batch, dups, (double)bytes / 1048576.0, total, (double)n / total);
    printf("per call us: p50 %.1f  p95 %.1f  p99 %.1f  max %.1f\n", lat[calls / 2] * 1e6,
           lat[calls * 95 / 100] * 1e6, lat[calls * 99 / 100] * 1e6, lat[calls - 1] * 1e6);
    free(lat); free(text); free(in); free(ids); free(st);
    xksm_close(x);
    return 0;
}
