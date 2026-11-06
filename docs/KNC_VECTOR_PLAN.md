# Plan: vector work on the Intel Xeon Phi 7120A (Knights Corner MVEX vector units)

Status: **plan only, nothing built or run on the card.** Project direction (2026-11-06): the
card's vector processing units perform the vector work (lattice scan, exact rerank) once
the CPU system is complete; the CUDA GPU backend is future work. The CPU path stays
complete and is the reference for every card result. Numbers marked "estimate" are
arithmetic and are replaced by measurements when the kernels exist.

## Why the card fits

| Property | Card | Host (measured, this project) |
|---|---|---|
| Memory bandwidth | 165 to 180 GB/s measured STREAM on this card family; 352 GB/s theoretical | lattice scan peaked at 57.7 to 58.0 GB/s with 4 threads |
| Vector width | 512 bits: 16 x int32 or 16 x f32 per instruction | 256-bit AVX2 |
| int8 codes | no int8 arithmetic, but memory up-convert loads sint8 directly into 16 int32 lanes (Intel 327364-001 section 2.2) | `vpmaddubsw` on int8 |
| Capacity | 16 GB GDDR5, 13.5 GB huge-page pool configured | host RAM, shared with everything else |

The lattice scan was measured to be bound by memory bandwidth on the host
(`reports/vector-lattice-2026-11-06.md`). The card has about 3x that bandwidth, and an
index resident in card memory needs only the query (768 bytes at 768 dimensions) sent in
and the candidate list sent back per search.

## Kernel design: 16-vector tiles, one vector per lane

Codes are stored dimension-major in tiles of 16 vectors: tile t holds, for every dimension
d, the 16 codes of vectors 16t..16t+15. Lane j of every register then accumulates vector j
of the tile, so no horizontal reduction is needed inside the scan.

Per tile, per dimension:

1. load 16 int8 codes with up-convert to 16 x int32 (one load);
2. multiply by the query's code for dimension d, broadcast to all lanes (32-bit multiply,
   instruction availability to verify in 327364-001);
3. add into the tile accumulator.

After all dimensions, multiply the 16 sums by the 16 scales and keep the best R in a
per-thread selection; merge on the host side of the result transfer.

## Cost estimate (arithmetic, not measured)

- Bytes per search at 100,000 x 768: 76.8 MB of codes. At 165 GB/s: about 0.47 ms.
- Operations: 2 vector operations per 16 codes, so 100,000 x 768 / 16 x 2 = 9.6 M vector
  operations; at 60 cores x 1.238 GHz x 1 operation per cycle (at least 2 threads per core
  needed for back-to-back issue): about 0.13 ms. The scan is bandwidth bound.
- Host comparison: lattice scan + rerank measured 1.331 to 1.338 ms with 4 threads
  (`reports/vector-lattice-2026-11-06.md`). Estimated card scan: about 0.47 ms plus the
  query and result transfer (a few microseconds of PCIe latency per message).
- Capacity with codes only on the card (f32 kept on the host for the exact rerank of the
  returned candidates): 13.5 GB / 772 bytes per 768-dimension vector = about 17 million
  vectors.

## Steps

1. Verify in 327364-001: sint8 up-convert load form, 32-bit integer multiply, broadcast
   form, the cheapest selection of the best R per thread.
2. Scalar reference on the card (native KNC C) for the tile layout; FIPS-style fixed
   vectors: digests of the scores compared with the host scalar reference.
3. MVEX kernel; compare every score with the host kernel for a fixed-seed batch.
4. Resident index: build tiles on the host, DMA to card memory once, append in tiles;
   deletion clears a lane's scale to remove it until compaction.
5. Measure: card scan with resident codes, query round trip, end to end; recall@10 equal
   to the host path. One short run per configuration (power budget on the desktop).
6. Wire into `backends/knc` behind `XKSM_KNC` (default OFF) and the cost model (phase 12);
   the card is used only where the measurement beats the host.

SHA-256 on the card is planned separately in `docs/KNC_SHA256_PLAN.md`.
