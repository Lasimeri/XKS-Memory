# Square-lattice (Z^n, int8) vector scan, 2026-11-06

Question: does quantizing vectors onto the square lattice Z^n (one int8 coordinate per dimension, one float scale per vector, codes in [-127, 127]) and scanning with AVX2 integer dot products improve retrieval speed over an exact f32 scan, and at what recall?

Host: Ryzen 7 5800X, single thread, GCC 16.2.1 -O3, kernels selected at run time (`avx2+fma`). Data: fixed-seed clustered placeholders, 1000 unit centers, vector = normalize(center + noise of norm about 0.5); queries drawn the same way; 100 queries per mode. Not real embeddings. Command: `build/bench_vector 100000 DIM 100 10 100 1000`, two runs per dimension. Mode B (exact f32 AVX2/FMA) is the ground truth for recall@10.

Kernel correctness (`tests/test_vector.c`, 7/7): f32 AVX2 vs double reference, worst relative error 3.87e-08 over lengths 0..1100; int8 AVX2 equal to the scalar reference for every length 0..1100 and exact in the saturation worst case (all codes +-127); lattice codes never -128, reconstruction error at most scale / 2.

```
=== run a
N 100000, dim 384, 100 queries, k 10, rerank 100, 1000 clusters, kernel avx2+fma
f32 data 153.6 MB, lattice codes 38.4 MB (+ 0.4 MB scales)
B f32 avx2+fma    median    3.757 ms/query    266.2 q/s   40.89 GB/s scanned
A f32 scalar      median   21.464 ms/query     46.6 q/s    7.16 GB/s scanned  recall@10 1.0000
C lattice i8      median    1.227 ms/query    814.7 q/s   31.61 GB/s scanned  recall@10 0.9730
D lattice+rerank  median    1.264 ms/query    791.4 q/s   30.71 GB/s scanned  recall@10 1.0000
=== run a
N 100000, dim 768, 100 queries, k 10, rerank 100, 1000 clusters, kernel avx2+fma
f32 data 307.2 MB, lattice codes 76.8 MB (+ 0.4 MB scales)
B f32 avx2+fma    median    7.377 ms/query    135.6 q/s   41.64 GB/s scanned
A f32 scalar      median   46.019 ms/query     21.7 q/s    6.68 GB/s scanned  recall@10 1.0000
C lattice i8      median    2.350 ms/query    425.6 q/s   32.85 GB/s scanned  recall@10 0.9700
D lattice+rerank  median    2.409 ms/query    415.1 q/s   32.05 GB/s scanned  recall@10 1.0000
=== run a
N 100000, dim 1024, 100 queries, k 10, rerank 100, 1000 clusters, kernel avx2+fma
f32 data 409.6 MB, lattice codes 102.4 MB (+ 0.4 MB scales)
B f32 avx2+fma    median    9.938 ms/query    100.6 q/s   41.22 GB/s scanned
A f32 scalar      median   62.226 ms/query     16.1 q/s    6.58 GB/s scanned  recall@10 1.0000
C lattice i8      median    3.164 ms/query    316.0 q/s   32.49 GB/s scanned  recall@10 0.9660
D lattice+rerank  median    3.269 ms/query    305.9 q/s   31.45 GB/s scanned  recall@10 1.0000
=== run b
N 100000, dim 384, 100 queries, k 10, rerank 100, 1000 clusters, kernel avx2+fma
f32 data 153.6 MB, lattice codes 38.4 MB (+ 0.4 MB scales)
B f32 avx2+fma    median    3.798 ms/query    263.3 q/s   40.44 GB/s scanned
A f32 scalar      median   21.540 ms/query     46.4 q/s    7.13 GB/s scanned  recall@10 1.0000
C lattice i8      median    1.233 ms/query    811.0 q/s   31.47 GB/s scanned  recall@10 0.9730
D lattice+rerank  median    1.309 ms/query    763.9 q/s   29.64 GB/s scanned  recall@10 1.0000
=== run b
N 100000, dim 768, 100 queries, k 10, rerank 100, 1000 clusters, kernel avx2+fma
f32 data 307.2 MB, lattice codes 76.8 MB (+ 0.4 MB scales)
B f32 avx2+fma    median    7.523 ms/query    132.9 q/s   40.84 GB/s scanned
A f32 scalar      median   46.087 ms/query     21.7 q/s    6.67 GB/s scanned  recall@10 1.0000
C lattice i8      median    2.402 ms/query    416.4 q/s   32.14 GB/s scanned  recall@10 0.9700
D lattice+rerank  median    2.518 ms/query    397.1 q/s   30.66 GB/s scanned  recall@10 1.0000
=== run b
N 100000, dim 1024, 100 queries, k 10, rerank 100, 1000 clusters, kernel avx2+fma
f32 data 409.6 MB, lattice codes 102.4 MB (+ 0.4 MB scales)
B f32 avx2+fma    median   10.038 ms/query     99.6 q/s   40.81 GB/s scanned
A f32 scalar      median   62.265 ms/query     16.1 q/s    6.58 GB/s scanned  recall@10 1.0000
C lattice i8      median    3.151 ms/query    317.4 q/s   32.63 GB/s scanned  recall@10 0.9660
D lattice+rerank  median    3.251 ms/query    307.6 q/s   31.62 GB/s scanned  recall@10 1.0000
```

| dim | B exact f32 ms/query | C lattice ms/query | D lattice + rerank 100 ms/query | D speedup over B | recall@10 C / D |
|---|---|---|---|---|---|
| 384 | 3.757 / 3.798 | 1.227 / 1.233 | 1.264 / 1.309 | 2.97x / 2.90x | 0.9730 / 1.0000 |
| 768 | 7.377 / 7.523 | 2.350 / 2.402 | 2.409 / 2.518 | 3.06x / 2.99x | 0.9700 / 1.0000 |
| 1024 | 9.938 / 10.038 | 3.164 / 3.151 | 3.269 / 3.251 | 3.04x / 3.09x | 0.9660 / 1.0000 |

Reading: the exact scan streams 40.4 to 41.6 GB/s and is bound by memory bandwidth; the lattice scan reads 4x fewer bytes but streams only 31.5 to 32.9 GB/s of codes, so it is bound by instructions (sign, multiply-add, widen per 32 bytes, plus per-vector selection), which is why the gain is 3x and not 4x. The exact rerank of 100 candidates restores recall@10 to 1.0000 on this data for about 3 % of the query time. The rerank touches 100 f32 rows per query, so the scanned index can be codes only. Not yet measured: real embeddings, multiple threads, rerank from NVMe, 4-bit codes, query batching.

## Thread scaling (worker pool, 2026-11-06)

Command: `build/bench_threads 100000 768 100`, two runs, same clustered placeholder data, library parallel search (`src/vector/search.c`) on the persistent pthread pool (`src/core/pool.c`); recall@10 against the single-thread exact result.

```
=== run a
N 100000, dim 768, 100 queries, k 10, rerank 100, kernel avx2+fma
threads   1  exact f32        median   7.796 ms/query    128.3 q/s   39.41 GB/s  recall@10 1.0000
threads   1  lattice+rerank   median   2.585 ms/query    386.9 q/s   29.87 GB/s  recall@10 1.0000
threads   2  exact f32        median   7.108 ms/query    140.7 q/s   43.22 GB/s  recall@10 1.0000
threads   2  lattice+rerank   median   1.509 ms/query    662.7 q/s   51.16 GB/s  recall@10 1.0000
threads   4  exact f32        median   6.842 ms/query    146.1 q/s   44.90 GB/s  recall@10 1.0000
threads   4  lattice+rerank   median   1.338 ms/query    747.3 q/s   57.69 GB/s  recall@10 1.0000
threads   8  exact f32        median   7.740 ms/query    129.2 q/s   39.69 GB/s  recall@10 1.0000
threads   8  lattice+rerank   median   1.532 ms/query    652.9 q/s   50.41 GB/s  recall@10 1.0000
threads  16  exact f32        median   7.826 ms/query    127.8 q/s   39.26 GB/s  recall@10 1.0000
threads  16  lattice+rerank   median   1.520 ms/query    657.7 q/s   50.78 GB/s  recall@10 1.0000
threads  32  exact f32        median   7.961 ms/query    125.6 q/s   38.59 GB/s  recall@10 1.0000
threads  32  lattice+rerank   median   1.513 ms/query    660.9 q/s   51.03 GB/s  recall@10 1.0000
=== run b
N 100000, dim 768, 100 queries, k 10, rerank 100, kernel avx2+fma
threads   1  exact f32        median   7.905 ms/query    126.5 q/s   38.86 GB/s  recall@10 1.0000
threads   1  lattice+rerank   median   2.479 ms/query    403.5 q/s   31.15 GB/s  recall@10 1.0000
threads   2  exact f32        median   6.628 ms/query    150.9 q/s   46.35 GB/s  recall@10 1.0000
threads   2  lattice+rerank   median   1.518 ms/query    658.8 q/s   50.86 GB/s  recall@10 1.0000
threads   4  exact f32        median   6.842 ms/query    146.2 q/s   44.90 GB/s  recall@10 1.0000
threads   4  lattice+rerank   median   1.331 ms/query    751.5 q/s   58.02 GB/s  recall@10 1.0000
threads   8  exact f32        median   7.585 ms/query    131.8 q/s   40.50 GB/s  recall@10 1.0000
threads   8  lattice+rerank   median   1.648 ms/query    606.8 q/s   46.84 GB/s  recall@10 1.0000
threads  16  exact f32        median   7.996 ms/query    125.1 q/s   38.42 GB/s  recall@10 1.0000
threads  16  lattice+rerank   median   1.579 ms/query    633.3 q/s   48.89 GB/s  recall@10 1.0000
threads  32  exact f32        median   7.963 ms/query    125.6 q/s   38.58 GB/s  recall@10 1.0000
threads  32  lattice+rerank   median   1.521 ms/query    657.4 q/s   50.75 GB/s  recall@10 1.0000
```

- Exact f32 does not scale (best 1.19x at 2 threads, about 1.0x at 8 and above): memory-bound from one thread.
- Lattice + rerank: 1.86x to 1.94x at 4 threads (1.331 to 1.338 ms/query), 5.8x faster than the single-thread exact scan; 8 threads (the default) are 15 to 24 % slower than 4, and 16 or 32 do not help: past 4 workers the memory bus is saturated and dispatch plus merge overhead grows.
- At 4 threads the lattice scan streams 57.7 to 58.0 GB/s, above the 51.2 GB/s assumed for DDR4-3200; either the memory is faster than assumed or part of the 77 MB index stays in L3 between queries. Not yet determined.
- Recall@10 = 1.0000 at every thread count.
