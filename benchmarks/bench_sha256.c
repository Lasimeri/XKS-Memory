/* bench_sha256.c: block-kernel throughput, scalar vs SHA-NI, interleaved repeats.
 * usage: bench_sha256 [MiB=256] [repeats=5] */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "sha256.h"

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

typedef void (*blocks_fn)(uint32_t[8], const uint8_t *, size_t);

int main(int argc, char **argv)
{
    size_t mib = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 256;
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    if (reps < 1 || reps > 64 || mib == 0)
        return 2;
    size_t n = mib << 20;
    uint8_t *buf = malloc(n);
    if (!buf)
        return 1;
    for (size_t i = 0; i < n; ++i)
        buf[i] = (uint8_t)(i * 2654435761u >> 24);

    struct { const char *name; blocks_fn f; double gbs[64]; } k[2] = {
        {"scalar", xksm__sha256_blocks_scalar, {0}},
#ifdef __x86_64__
        {"sha-ni", xksm__sha256_blocks_shani, {0}},
#endif
    };
    int nk = strcmp(xksm__sha256_kernel(), "sha-ni") ? 1 : 2;

    uint32_t sink = 0;
    for (int r = 0; r < reps; ++r)
        for (int j = 0; j < nk; ++j) { /* interleaved: scalar, sha-ni, scalar, ... */
            uint32_t st[8] = {0};
            double t0 = now_s();
            k[j].f(st, buf, n / 64);
            double dt = now_s() - t0;
            sink ^= st[0];
            k[j].gbs[r] = (double)n / dt / 1e9;
        }

    printf("buffer %zu MiB, %d interleaved repeats, sink %08x\n", mib, reps, sink);
    for (int j = 0; j < nk; ++j) {
        qsort(k[j].gbs, (size_t)reps, sizeof(double), cmp_d);
        printf("%-7s GB/s  min %.3f  median %.3f  max %.3f\n", k[j].name, k[j].gbs[0],
               k[j].gbs[reps / 2], k[j].gbs[reps - 1]);
    }
    free(buf);
    return 0;
}
