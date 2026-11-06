# Architecture

Status: the diagram below is the target design. The table after it lists what is built;
everything else in the diagram is planned (see ROADMAP.md).

## Layers

```
 ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   
 │ tui/ (Rust)  │  │ cli/ xksm    │  │ src/api 127.0.0.1    │   
 └──────┬───────┘  └──────┬───────┘  └──────────┬───────────┘   
        └─────────────────┼─────────────────────┘               
                          │ C ABI: include/xksm.h               
 ┌────────────────────────▼─────────────────────────────────┐   
 │ libxksm (C17)                                            │   
 │  ingest ─▶ memory (classify, consolidate) ─▶ storage     │   
 │  retrieval (lexical, vector, temporal, structured, RRF)  │   
 │  context (token budget, citations)  scheduler (cost)     │   
 └───────┬──────────────────┬──────────────────┬────────────┘   
         ▼                  ▼                  ▼                
   backends/cpu      backends/knc       backends/cuda           
   (always)          (vector work)      (future)                
```

## Built modules

| Path | Role | Test |
|---|---|---|
| `include/xksm.h` | public C ABI | all tests link only through it, plus private headers where noted |
| `src/core/xksm.c` | handle lifecycle, errors, time, thread setting | `test_schema`, `test_search` |
| `src/core/sha256.c` | SHA-256, SHA-NI kernel selected by CPUID, scalar fallback | `test_sha256` |
| `src/core/pool.c` | persistent POSIX-thread worker pool (default 8, up to 128) | `test_search` (also under ThreadSanitizer) |
| `src/storage/schema.c` | schema v1 and ordered migrations | `test_schema` |
| `src/memory/memory.c` | add, batch add, get, delete; cached prepared statements | `test_memory` |
| `src/cache/cache.c` | Tier-1 RAM record cache, 5 to 50 % of the records | `test_cache` |
| `src/ingest/import.c` | explicit text and Markdown import, chunking, provenance | `test_import` |
| `src/vector/kernels.c` | f32 and square-lattice int8 dot products (AVX2, scalar); the Knights Corner MVEX port is planned (`docs/KNC_VECTOR_PLAN.md`) | `test_vector` |
| `src/vector/search.c` | parallel exact and lattice top-k, exact rerank | `test_search` |
| `src/vector/store.c` | vectors in SQLite (schema v2), RAM index, filtered vector search | `test_vecstore` |
| `src/retrieval/hybrid.c` | reciprocal rank fusion of lexical and vector results, per-hit explanation | `test_hybrid` |
| `backends/opencl/` | OpenCL GPU randomized SVD (kernels in `rsvd.cl`, embedded at build time) | `test_ocl` |
| `src/semantic/port.c`, `src/vector/backend.c` | semantic port (mmap, buckets, expansion) and the vector-backend interface (CPU reference) | `eval_retrieval` modes `port`, `porthybrid` |
| `src/semantic/` | count-based semantic model: PPMI, randomized SVD (`rsvd.c`), CSR (`sparse.c`), dense helpers (`linalg.c`), SIF memory vectors (`semantic.c`) | `test_linalg`, `test_semantic` |
| `src/retrieval/lexical.c` | FTS5 search, BM25, filters, snippets, two-stage | `test_lexical` |
| `cli/main.c` | `xksm init`, `info`, `import`, `search`, `version` | manual, `reports/insert-2026-11-06.md` |

## Invariants

1. SQLite is the only authoritative store. FTS5 tables, vector files and accelerator caches
   are derived and can be rebuilt from it at any time.
2. Original source text is never overwritten; derived memories point back to it.
3. Deletion is transactional in SQLite and is then propagated to every derived index and
   cache before the call returns.
4. Retrieved text is data. It is never interpreted as instructions by the engine and is
   marked as lower-trust in assembled context.
5. No code path opens a non-loopback socket unless the user configures one explicitly.
6. The CPU backend implements every operation. Accelerators are selected per query by a
   cost model (transfer + dispatch + execution + aggregation) and only where measured
   faster.

## Language split

- Core, API server, CLI, backends: C17 (CUDA C++ for `.cu` files, C for the KNC native
  side).
- TUI: Rust, linking `libxksm` through its C ABI.
