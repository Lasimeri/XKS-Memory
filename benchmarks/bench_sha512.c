/* bench_sha512.c: CPU SHA-512 block throughput, the baseline every other SHA-512 path
 * (CUDA batch kernel) is measured against.
 * usage: bench_sha512 [MiB=256] [repeats=5] */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "sha512.h"

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

    double gbs[64];
    uint64_t sink = 0;
    for (int r = 0; r < reps; ++r) {
        uint64_t st[8] = {0};
        double t0 = now_s();
        xksm__sha512_blocks(st, buf, n / 128);
        gbs[r] = (double)n / (now_s() - t0) / 1e9;
        sink ^= st[0];
    }
    qsort(gbs, (size_t)reps, sizeof(double), cmp_d);
    printf("sha512 cpu scalar, %zu MiB x %d: GB/s min %.3f median %.3f max %.3f (sink %016llx)\n",
           mib, reps, gbs[0], gbs[reps / 2], gbs[reps - 1], (unsigned long long)sink);
    free(buf);
    return 0;
}
