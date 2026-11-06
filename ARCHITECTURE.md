# Architecture

Status: design. Components are listed in build order; each section states what exists.

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
   backends/cpu      backends/cuda      backends/knc            
   (always)          (optional)         (optional, experimental)
```

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
