# Roadmap

States: `todo`, `wip`, `done (verified: <how>)`.

| Phase | Deliverable | State |
|---|---|---|
| 0 | Repository, license, project documents | done |
| 1 | Research notes, source matrix, design decisions | done (verified: FTS5 claims run on SQLite 3.54.0, docs/RESEARCH.md section 4); 7 sources still unread, listed in docs/RESEARCH.md section 3 |
| 2 | CMake build, `libxksm` skeleton, versioned SQLite schema + migrations | done (verified: ctest schema 19/19 checks; -Werror zero diagnostics; xksm refuses schema 7 store, exit 1) |
| 3 | Persistent storage, explicit import (text, Markdown, JSON, JSONL, CSV), chunking, hashing, dedup | wip: SHA-256 done (verified: FIPS vectors, scalar = SHA-NI on 0..4096, 18 files = openssl; SHA-NI 2.369 GB/s); memory add/get/delete + batch done (verified: test_memory 48/48, deleted text 2 -> 0 occurrences in db file and WAL; insert 15.1-15.7 k rec/s batched, reports/insert-2026-11-06.md); RAM record cache (5-50 % of records, default 25 %, optional byte ceiling) done (verified: test_cache 175/175, ASan+UBSan clean, hit p50 0.12-0.20 us vs 3.6-3.7 us SQLite); text + Markdown import done (verified: test_import 27/27, ASan clean, 779 real files 22.4 MB in 2.2-2.3 s); JSON, JSONL, CSV, PDF and opt-in Latin-1 transcoding todo |
| 4 | FTS5 lexical retrieval, structured filters | todo |
| 5 | Local embeddings (user-supplied model), exact + ANN vector retrieval | wip: kernels done (verified: test_vector 7/7); square-lattice scan + rerank 2.90-3.09x faster than exact at recall@10 1.0 on placeholders (reports/vector-lattice-2026-11-06.md); vector store, embeddings, threads todo |
| 6 | Hybrid retrieval, RRF, result explanations | todo |
| 7 | Consolidation, correction, supersession, forgetting | todo |
| 8 | Context assembly, local HTTP API, llama.cpp integration | todo |
| 9 | Rust TUI | todo |
| 10 | CUDA backend | todo |
| 11 | KNC backend (experimental); first kernel: 16-lane SHA-256, plan in docs/KNC_SHA256_PLAN.md | todo |
| 12 | Cost scheduler, tiered caches | todo |
| 13 | Offline packaging, backup, restore | todo |
| 14 | Tests, benchmarks, retrieval-quality evaluation | todo |
| 15 | Documentation pass, release | todo |

PDF import is planned after Phase 3 and depends on choosing a permissively licensed
extractor.
