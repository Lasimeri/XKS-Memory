/* bench_sha512_cuda.cu: batched SHA-512 on the GPU vs the CPU baseline, same messages.
 * Times each cost-model term separately (H2D, kernel, D2H) and checks every digest.
 * usage: bench_sha512_cuda [messages=65536] [maxlen=8192] [repeats=5] */
#include "../backends/cuda/sha512_batch.cuh"
#include "sha512.h"

#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>

#define CK(x)                                                                             \
    do {                                                                                  \
        cudaError_t e_ = (x);                                                             \
        if (e_ != cudaSuccess) {                                                          \
            fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #x, cudaGetErrorString(e_)); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

static double now_s()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    uint32_t n = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 65536;
    uint32_t maxlen = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 8192;
    int reps = argc > 3 ? atoi(argv[3]) : 5;
    if (!n || !maxlen || reps < 1 || reps > 32)
        return 2;

    uint64_t *off = (uint64_t *)malloc(sizeof(uint64_t) * n);
    uint32_t *len = (uint32_t *)malloc(sizeof(uint32_t) * n);
    uint64_t rng = 0x9e3779b97f4a7c15ull, total = 0, msg_bytes = 0;
    for (uint32_t i = 0; i < n; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        len[i] = (uint32_t)(rng % maxlen);
        off[i] = total;
        total += (len[i] + 7u) & ~7ull;
        msg_bytes += len[i];
    }
    size_t buf_bytes = total + 8; /* slack for the last partial word */

    uint8_t *h_data, *h_dig;
    CK(cudaMallocHost(&h_data, buf_bytes));
    CK(cudaMallocHost(&h_dig, (size_t)n * 64));
    for (size_t i = 0; i < buf_bytes; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        h_data[i] = (uint8_t)(rng >> 56);
    }

    /* CPU reference, single thread, same messages */
    uint8_t *ref = (uint8_t *)malloc((size_t)n * 64);
    double t0 = now_s();
    for (uint32_t i = 0; i < n; ++i)
        xksm__sha512(h_data + off[i], len[i], ref + (size_t)i * 64);
    double cpu_s = now_s() - t0;

    cudaDeviceProp prop;
    CK(cudaGetDeviceProperties(&prop, 0));
    uint8_t *d_data; uint64_t *d_off, *d_out; uint32_t *d_len;
    CK(cudaMalloc(&d_data, buf_bytes));
    CK(cudaMalloc(&d_off, sizeof(uint64_t) * n));
    CK(cudaMalloc(&d_len, sizeof(uint32_t) * n));
    CK(cudaMalloc(&d_out, (size_t)n * 64));
    cudaEvent_t e[4];
    for (auto &x : e) CK(cudaEventCreate(&x));

    const int tpb = 128;
    const uint32_t grid = (n + tpb - 1) / tpb;
    float h2d[32], ker[32], d2h[32];
    for (int r = 0; r < reps; ++r) {
        CK(cudaEventRecord(e[0]));
        CK(cudaMemcpyAsync(d_data, h_data, buf_bytes, cudaMemcpyHostToDevice));
        CK(cudaMemcpyAsync(d_off, off, sizeof(uint64_t) * n, cudaMemcpyHostToDevice));
        CK(cudaMemcpyAsync(d_len, len, sizeof(uint32_t) * n, cudaMemcpyHostToDevice));
        CK(cudaEventRecord(e[1]));
        xksm_sha512_batch_kernel<<<grid, tpb>>>(d_data, d_off, d_len, n, d_out);
        CK(cudaGetLastError());
        CK(cudaEventRecord(e[2]));
        CK(cudaMemcpyAsync(h_dig, d_out, (size_t)n * 64, cudaMemcpyDeviceToHost));
        CK(cudaEventRecord(e[3]));
        CK(cudaEventSynchronize(e[3]));
        CK(cudaEventElapsedTime(&h2d[r], e[0], e[1]));
        CK(cudaEventElapsedTime(&ker[r], e[1], e[2]));
        CK(cudaEventElapsedTime(&d2h[r], e[2], e[3]));
    }

    /* device words -> big-endian digest bytes, compare all */
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t d[64];
        const uint64_t *w = (const uint64_t *)(h_dig + (size_t)i * 64);
        for (int k = 0; k < 8; ++k)
            for (int j = 0; j < 8; ++j)
                d[8 * k + j] = (uint8_t)(w[k] >> (56 - 8 * j));
        bad += memcmp(d, ref + (size_t)i * 64, 64) != 0;
    }

    std::sort(h2d, h2d + reps); std::sort(ker, ker + reps); std::sort(d2h, d2h + reps);
    double mb = (double)msg_bytes;
    printf("%s, %u messages, lengths 0..%u, %.1f MiB of message bytes, %d repeats\n",
           prop.name, n, maxlen - 1, mb / 1048576.0, reps);
    printf("digest mismatches GPU vs CPU: %u / %u\n", bad, n);
    printf("cpu 1 thread    : %8.2f ms  %7.3f GB/s\n", cpu_s * 1e3, mb / cpu_s / 1e9);
    printf("gpu kernel      : min %8.3f  median %8.3f  max %8.3f ms  -> median %7.2f GB/s\n",
           ker[0], ker[reps / 2], ker[reps - 1], mb / (ker[reps / 2] * 1e-3) / 1e9);
    printf("h2d (pinned)    : min %8.3f  median %8.3f  max %8.3f ms  -> median %7.2f GB/s\n",
           h2d[0], h2d[reps / 2], h2d[reps - 1], (double)buf_bytes / (h2d[reps / 2] * 1e-3) / 1e9);
    printf("d2h digests     : median %8.3f ms\n", d2h[reps / 2]);
    double e2e = (h2d[reps / 2] + ker[reps / 2] + d2h[reps / 2]) * 1e-3;
    printf("end to end      : median %8.3f ms  -> %7.2f GB/s, %.2fx the 1-thread CPU\n", e2e * 1e3,
           mb / e2e / 1e9, cpu_s / e2e);
    return bad != 0;
}
