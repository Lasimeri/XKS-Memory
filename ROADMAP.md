# Roadmap

States: `todo`, `wip`, `done (verified: <how>)`.

| Phase | Deliverable | State |
|---|---|---|
| 0 | Repository, license, project documents | done |
| 1 | Research notes, source matrix, design decisions | done (verified: FTS5 claims run on SQLite 3.54.0, docs/RESEARCH.md section 4); 7 sources still unread, listed in docs/RESEARCH.md section 3 |
| 2 | CMake build, `libxksm` skeleton, versioned SQLite schema + migrations | done (verified: ctest schema 19/19 checks; -Werror zero diagnostics; xksm refuses schema 7 store, exit 1) |
| 3 | Persistent storage, explicit import (text, Markdown, JSON, JSONL, CSV), chunking, hashing, dedup | wip: SHA-256 done (verified: FIPS vectors, scalar = SHA-NI on 0..4096, 18 files = openssl; SHA-NI 2.369 GB/s); memory add/get/delete done (verified: test_memory 41/41, deleted text 2 -> 0 occurrences in db file and WAL); importers todo |
| 4 | FTS5 lexical retrieval, structured filters | todo |
| 5 | Local embeddings (user-supplied model), exact + ANN vector retrieval | todo |
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
