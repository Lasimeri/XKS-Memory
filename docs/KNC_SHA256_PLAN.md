# Plan: SHA-256 on the Intel Xeon Phi 7120A (Knights Corner)

Status: **plan only, nothing built or run on the card.** Phase 11 work. Every number below
marked "estimate" is arithmetic and is replaced by a measurement when the kernel exists.

## Why it is plausible

SHA-256 works on 32-bit words. Knights Corner vectors hold sixteen 32-bit lanes (512 bits)
and the integer operations SHA-256 needs (add, shift, and, or, xor, and-not) all exist in
IMCI. The card has no SHA instructions, so the kernel is **multi-buffer**: lane *j* of
every vector register holds the state of message *j*, and one instruction stream hashes 16
independent messages at once. XKS-Memory produces exactly that shape: every memory chunk
has its own checksum.

## Hardware facts used

| Fact | Value | Source |
|---|---|---|
| Vector width | 64 bytes, dword and qword elements only | Intel 327364-001 section 2.1.1 |
| Masks | 8 registers, 16 bits, merge masking | 327364-001 section 2.1.2 |
| Cores | 61 (60 for compute, 1 for the card OS), 1.238 GHz | 7120A specification |
| Issue | in order; one thread cannot issue back to back, so at least 2 threads per core | Intel developer guide 328207-002 |
| Vector latency | 4 cycles | same |
| GDDR5 stream bandwidth | 165 to 180 GB/s measured with STREAM-style kernels | same, plus measurements on this card family |
| PCIe | Gen2 x8; 3.32 GB/s card-to-host DMA measured on the development host; 3.58 GB/s host-owned DMA measured on a 3120A | project measurements |

To verify in 327364-001 before writing code: whether IMCI has a vector rotate
(AVX-512F's `vprord`) or a three-input logic operation (`vpternlogd`). The plan assumes
**neither**, so a rotate costs 3 operations (two shifts and an or) and the byte swap of each
big-endian input word costs shifts and masks (there is no byte shuffle).

## Cost estimate (arithmetic, not measured)

Per 64-byte block per lane, assuming no rotate instruction:

| Part | Operations per 16 blocks |
|---|---|
| 64 rounds: Sigma1 and Sigma0 (3 rotates and 2 xors each), Ch, Maj, 7 adds | about 2,400 |
| 48 schedule words: sigma0, sigma1, 3 adds | about 900 |
| loads and byte swaps of 16 input words | about 110 |
| total | about 3,400 vector operations per 1,024 bytes |

At 1 vector operation per cycle per core, 60 cores and 1.238 GHz:
60 x 1.238e9 / 3,400 x 1,024 B = **about 22 GB/s** card-side compute (estimate).

## Where it can and cannot win

| Data location | Card bound | Host reference | Verdict |
|---|---|---|---|
| Host RAM, shipped to the card | PCIe, about 3.3 to 3.6 GB/s | SHA-NI 2.369 GB/s per thread measured; 8 threads not yet measured | at best about 1.4x one host thread; loses to an idle multi-core host (estimate) |
| Already in card GDDR (card-resident partitions, Tier 2) | compute, about 22 GB/s estimate | host would need the data copied back | card wins: no transfer at all |
| Host cores saturated by ingestion | PCIe | host has no idle cores | card adds about 3.3 GB/s of hashing without taking host cores |

The scheduler (phase 12) chooses per batch with the cost model
transfer + dispatch + execution + aggregation, using measured coefficients.

## Steps

1. Read the IMCI integer and logic instruction pages of 327364-001; record whether a rotate
   or ternary logic exists and the exact byte-swap sequence.
2. Scalar SHA-256 on the card (native KNC C, built with the project's KNC toolchain) as the
   on-card reference; check FIPS 180-2 vectors on the card.
3. 16-lane IMCI kernel, one message per lane; compare every digest with the host SHA-NI
   kernel for a fixed-seed batch of mixed lengths.
4. Measure: card kernel GB/s with data resident, host-to-card DMA GB/s, end to end; one
   short run per configuration on the desktop (power budget).
5. Lane balancing: sort messages by length (or group by block count) so 16 lanes finish
   together; measure lane utilization before and after.
6. Wire into the KNC backend behind `XKSM_KNC` (default OFF). The CPU path stays complete.
EOF
cp benchmarks/out/sha256_run2.txt /dev/null && { printf '# SHA-256 measurements, 2026-11-06\n\nHost: Ryzen 7 5800X (sha_ni, avx2, bmi2), GCC 16.2.1 -O3, OpenSSL 3.6.5. One run per configuration.\n\n## Correctness\n\n- `build/test_sha256`: FIPS 180-2 vectors (empty, "abc", 448-bit, 1,000,000 x "a") through the dispatched kernel; scalar vs SHA-NI on every length 0..4096: 0 mismatches; streaming in chunk sizes 1..130 equals one-shot. 7/7 checks.\n- `build/test_sha256 FILE...` on 18 random files of 0, 1, 111, 112, 113, 127, 128, 129, 239, 240, 255, 256, 257, 1000, 65535, 65536, 65537 and 1048577 bytes: identical to `openssl dgst -sha256`.\n\n## Throughput\n\nCommand: `build/bench_sha256 256 5` (256 MiB, 5 repeats, kernels interleaved), then `openssl speed -seconds 2 -bytes 16384 sha256`.\n\n```\n'; cat benchmarks/out/sha256_run2.txt; printf '```\n\nA first run the same day gave scalar median 0.391 and SHA-NI median 2.356 GB/s (min 2.274, max 2.365), OpenSSL 2.342 GB/s. SHA-NI is 6.0x the scalar kernel and on par with OpenSSL (1.005x). Multi-threaded throughput has not been measured.\n'; } > reports/sha256-2026-11-06.md && sed -i '1s/.*/# SHA-512 measurements, 2026-11-06 (superseded: the project uses SHA-256, see docs\/DESIGN_DECISIONS.md D12)/' reports/sha512-2026-11-06.md && sed -i 's#^Command: `bench_sha512_cuda 65536 8192 5`, binary built with `nvcc -O3 -arch=sm_86 -std=c++17 -Isrc/core benchmarks/bench_sha512_cuda.cu src/core/sha512.c` (same source and flags as the `XKSM_CUDA=ON` CMake target `build-cuda/bench_sha512_cuda`)#Command: `bench_sha512_cuda 65536 8192 5`, built with `nvcc -O3 -arch=sm_86 -std=c++17`. The SHA-512 sources were removed from the tree after this measurement; they are in commit a9f46d6.#' reports/sha512-2026-11-06.md && grep -c a9f46d6 reports/sha512-2026-11-06.md && sed -n 1,3p reports/sha512-2026-11-06.md