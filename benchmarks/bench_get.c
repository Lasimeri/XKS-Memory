/* bench_get.c: xksm_memory_get latency, Tier-1 cache hits vs SQLite reads.
 * usage: bench_get STORE [gets=200000] [hot_pct=0]
 *   hot_pct = 0: uniform random ids; e.g. 80: 80 % of gets go to the first 20 % of ids. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "xksm.h"

#include <stdio.h>
#include <stdlib.h>
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

static void report(const char *name, double *v, long n)
{
    if (!n) {
        printf("%-5s: none\n", name);
        return;
    }
    qsort(v, (size_t)n, sizeof(double), cmp_d);
    printf("%-5s: %7ld gets  p50 %7.2f us  p95 %7.2f us  p99 %7.2f us\n", name, n,
           v[n / 2] * 1e6, v[n * 95 / 100] * 1e6, v[n * 99 / 100] * 1e6);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    long n = argc > 2 ? strtol(argv[2], NULL, 10) : 200000;
    long hot = argc > 3 ? strtol(argv[3], NULL, 10) : 0;
    if (n < 1 || hot < 0 || hot > 100)
        return 2;
    xksm *x = NULL;
    if (xksm_open(argv[1], &x) != XKSM_OK) {
        fprintf(stderr, "open: %s\n", xksm_errmsg(x));
        return 1;
    }
    xksm_cache_info ci;
    xksm_cache_stats(x, &ci);
    long records = (long)ci.records; /* ids 1..records in a fresh bench_insert store */
    double *hit = malloc(sizeof(double) * (size_t)n), *miss = malloc(sizeof(double) * (size_t)n);
    long nh = 0, nm = 0;
    uint64_t r = 0x9e3779b97f4a7c15u;
    for (long i = 0; i < n; ++i) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        long range = (hot && (long)(r % 100) < hot) ? records / 5 : records;
        xksm_id id = 1 + (xksm_id)((r >> 20) % (uint64_t)range);
        uint64_t h0 = ci.hits;
        xksm_memory m;
        double a = now_s();
        xksm_status s = xksm_memory_get(x, id, &m);
        double dt = now_s() - a;
        if (s != XKSM_OK) {
            fprintf(stderr, "get %lld: %s\n", (long long)id, xksm_errmsg(x));
            return 1;
        }
        xksm_memory_free(&m);
        xksm_cache_stats(x, &ci);
        if (ci.hits > h0)
            hit[nh++] = dt;
        else
            miss[nm++] = dt;
    }
    printf("%ld records, capacity %u, pattern %s: hit rate %.2f %%\n", records, ci.capacity,
           hot ? "skewed" : "uniform", 100.0 * (double)nh / (double)n);
    report("hit", hit, nh);
    report("miss", miss, nm);
    free(hit);
    free(miss);
    xksm_close(x);
    return 0;
}
