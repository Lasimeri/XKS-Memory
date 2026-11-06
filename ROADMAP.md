# Roadmap

States: `todo`, `wip`, `done (verified: <how>)`.

| Phase | Deliverable | State |
|---|---|---|
| 0 | Repository, license, project documents | wip |
| 1 | Research notes, source matrix, design decisions | todo |
| 2 | CMake build, `libxksm` skeleton, versioned SQLite schema + migrations | todo |
| 3 | Persistent storage, explicit import (text, Markdown, JSON, JSONL, CSV), chunking, hashing, dedup | todo |
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
