# XKS-Memory

An offline, open-source, hardware-accelerated long-term memory engine for large language models.

**Status: pre-alpha.** The storage core, explicit import, lexical, vector and hybrid search, the RAM record cache, the CPU
worker pool and the vector kernels are built and tested; embeddings, the
terminal UI and the accelerator backends are not. [ROADMAP.md](ROADMAP.md) lists every
phase with its state; a feature is marked done only after it was built, run and its output
checked. Measurements are in [reports/](reports/).

## What it is

XKS-Memory stores what you explicitly give it (notes, documents, conversation exports, model
summaries), indexes it lexically, semantically and by time, and assembles the most relevant
pieces into a token-budgeted context block for a local language model. It runs entirely on
your machine: no telemetry, no cloud service, no account, no network access at runtime.

- **Core:** C17 library `libxksm` with a C ABI (`include/xksm.h`).
- **Storage:** SQLite (transactional, authoritative); every other index is rebuildable from it.
- **Interface:** the `xksm` command-line tool now; a terminal UI written in Rust and a
  loopback-only local HTTP API are planned.
- **Acceleration (optional, planned):** the Intel Xeon Phi 7120A (Knights Corner) MVEX vector
  units perform the vector work (`docs/KNC_VECTOR_PLAN.md`); a CUDA GPU backend is future
  work. The CPU path is always complete.

## Working today

| Area | State |
|---|---|
| Store | SQLite schema v1 with migrations; refuses stores newer than the library |
| Memories | add, batch add (one transaction), get, delete; UTF-8 validation, normalization, SHA-256 checksums, per-namespace dedup |
| Forgetting | delete removes the row and its full-text entry in one transaction; a test shows the deleted text is gone from the database file and its WAL |
| Import | named text and Markdown files only (no directory walks): paragraph chunks of about 1 KiB, headings start chunks, original bytes kept as evidence, each chunk points at its byte range |
| RAM cache | holds 5 to 50 % of the records (default 25 %), optional byte ceiling, CLOCK eviction |
| CPU threads | worker pool, 8 threads by default, up to 128 |
| Hybrid search | reciprocal rank fusion of lexical and vector results; each hit shows its rank in each list |
| Lexical search | `xksm search`: quoted terms (no FTS5 injection), BM25 ranking, namespace and category filters, snippets; 1.8 ms median on 24,088 real memories |
| Vectors | vector store per (memory, model) in SQLite; square-lattice (int8) scan with exact rerank, filters, AVX2 with scalar fallback |
| Recall (default; full reference: [docs/EMBEDDING.md](docs/EMBEDDING.md)) | `xksm recall`: hybrid of BM25 on the typed query and a lattice vector scan whose query expands every content word through a semantic port (word relations computed once offline by an embedding model, grouped by script and code) over a count-based model built from the store on the CPU (AVX2/FMA, about 1 s for 2,856 memories); no model at query time, 0.06 s per CLI call |

Selected measurements on a Ryzen 7 5800X (details and commands in `reports/`):

| Measurement | Result |
|---|---|
| SHA-256 (SHA-NI) | 2.369 GB/s, 6.0x the portable kernel |
| Import of 779 real text files, 22.4 MB | 2.2 to 2.3 s |
| Cached read vs SQLite read | 0.12 to 0.20 us vs 3.6 to 3.7 us |
| Lattice scan + rerank vs exact scan, 100k x 768 | 2.9 to 3.1x faster at recall@10 = 1.0 (one thread), 5.8x with 4 threads |

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Requirements: a C17 compiler, CMake >= 3.20, SQLite >= 3.35 with FTS5, POSIX threads.
Optional: Rust (terminal UI, planned), a Knights Corner toolchain (planned vector backend),
CUDA (future GPU backend).

## Use

```
build/xksm init notes.xksm
build/xksm import notes.xksm README.md docs/DESIGN_DECISIONS.md
build/xksm info notes.xksm
build/xksm search notes.xksm cache eviction
build/xksm semantic notes.xksm
build/xksm port notes.xksm /path/to/model.port     # once; made by tools/port_build
build/xksm recall notes.xksm how is old data evicted
```

## Scope

XKS-Memory is an independent memory-management project. Its layering (local indexing,
short-lived caches in front of longer-lived searchable records, plugin processing stages,
federated queries over several indexes, result aggregation) draws on general architecture
described in publicly released historical documents about the XKeyscore system. It has no
affiliation with or endorsement from any government agency and contains no code or
non-public material from any such system.

It contains **no** packet capture, network inspection, communications interception,
monitoring, user tracking or identity correlation. It processes only data its owner
deliberately imports.

## License

MIT, see [LICENSE](LICENSE).
