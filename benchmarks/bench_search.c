/* bench_search.c: xksm_search latency over a fixed query set, ANY and ALL modes.
 * usage: bench_search STORE [repeats=200] */
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

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    int reps = argc > 2 ? atoi(argv[2]) : 200;
    if (reps < 1)
        return 2;
    static const char *const queries[] = {
        "license",       "install",         "bug fix",       "memory leak",
        "configuration file", "kernel module", "unicode",   "thread safety",
        "compile error", "deprecated function",
    };
    const int nq = (int)(sizeof queries / sizeof *queries);
    xksm *x = NULL;
    if (xksm_open(argv[1], &x) != XKSM_OK) {
        fprintf(stderr, "open: %s\n", xksm_errmsg(x));
        return 1;
    }
    xksm_cache_info ci;
    xksm_cache_stats(x, &ci);
    printf("%lld memories, %d queries x %d repeats, limit 10\n", (long long)ci.records, nq,
           reps);
    double *t = malloc(sizeof(double) * (size_t)(nq * reps));
    for (int mode = 0; mode < 2; ++mode) {
        xksm_search_opts o = {NULL, 0, mode ? XKSM_MATCH_ALL : XKSM_MATCH_ANY, 10};
        size_t hits = 0;
        int n = 0;
        for (int r = 0; r < reps; ++r)
            for (int q = 0; q < nq; ++q) {
                xksm_search_result res;
                double a = now_s();
                if (xksm_search(x, queries[q], &o, &res) != XKSM_OK) {
                    fprintf(stderr, "search: %s\n", xksm_errmsg(x));
                    return 1;
                }
                t[n++] = now_s() - a;
                if (r == 0)
                    hits += res.count;
                xksm_search_free(&res);
            }
        qsort(t, (size_t)n, sizeof(double), cmp_d);
        printf("%-3s  p50 %8.1f us  p95 %8.1f us  p99 %8.1f us  max %8.1f us  (%zu hits over the set)\n",
               mode ? "ALL" : "ANY", t[n / 2] * 1e6, t[n * 95 / 100] * 1e6, t[n * 99 / 100] * 1e6,
               t[n - 1] * 1e6, hits);
    }
    free(t);
    xksm_close(x);
    return 0;
}
