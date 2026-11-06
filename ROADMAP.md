# Roadmap

States: `todo`, `wip`, `done (verified: <how>)`.

| Phase | Deliverable | State |
|---|---|---|
| 0 | Repository, license, project documents | done |
| 1 | Research notes, source matrix, design decisions | done (verified: FTS5 claims run on SQLite 3.54.0, docs/RESEARCH.md section 4); 7 sources still unread, listed in docs/RESEARCH.md section 3 |
| 2 | CMake build, `libxksm` skeleton, versioned SQLite schema + migrations | done (verified: ctest schema 19/19 checks; -Werror zero diagnostics; xksm refuses schema 7 store, exit 1) |
| 3 | Persistent storage, explicit import (text, Markdown, JSON, JSONL, CSV), chunking, hashing, dedup | wip: SHA-512 CPU done (verified: FIPS vectors, 18 files = openssl); CUDA batch SHA-512 measured (reports/sha512-2026-11-06.md); insert API and importers todo |
| 4 | FTS5 lexical retrieval, structured filters | todo |
| 5 | Local embeddings (user-supplied model), exact + ANN vector retrieval | todo |
| 6 | Hybrid retrieval, RRF, result explanations | todo |
| 7 | Consolidation, correction, supersession, forgetting | todo |
| 8 | Context assembly, local HTTP API, llama.cpp integration | todo |
| 9 | Rust TUI | todo |
| 10 | CUDA backend | todo |
| 11 | KNC backend (experimental) | todo |
| 12 | Cost scheduler, tiered caches | todo |
| 13 | Offline packaging, backup, restore | todo |
| 14 | Tests, benchmarks, retrieval-quality evaluation | todo |
| 15 | Documentation pass, release | todo |

PDF import is planned after Phase 3 and depends on choosing a permissively licensed
extractor.
