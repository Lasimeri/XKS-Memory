# Roadmap

States: `todo`, `wip`, `done (verified: <how>)`.

| Phase | Deliverable | State |
|---|---|---|
| 0 | Repository, license, project documents | done |
| 1 | Research notes, source matrix, design decisions | done (verified: FTS5 claims run on SQLite 3.54.0, docs/RESEARCH.md section 4); 7 sources still unread, listed in docs/RESEARCH.md section 3 |
| 2 | CMake build, `libxksm` skeleton, versioned SQLite schema + migrations | done (verified: ctest schema 19/19 checks; -Werror zero diagnostics; xksm refuses schema 7 store, exit 1) |
| 3 | Persistent storage, explicit import (text, Markdown, JSON, JSONL, CSV), chunking, hashing, dedup | wip: SHA-256 done (verified: FIPS vectors, scalar = SHA-NI on 0..4096, 18 files = openssl; SHA-NI 2.369 GB/s); memory add/get/delete + batch done (verified: test_memory 48/48, deleted text 2 -> 0 occurrences in db file and WAL; insert 15.1-15.7 k rec/s batched, reports/insert-2026-11-06.md); RAM record cache (5-50 % of records, default 25 %, optional byte ceiling) done (verified: test_cache 175/175, ASan+UBSan clean, hit p50 0.12-0.20 us vs 3.6-3.7 us SQLite); text + Markdown import done (verified: test_import 27/27, ASan clean, 779 real files 22.4 MB in 2.2-2.3 s); JSON, JSONL, CSV, PDF and opt-in Latin-1 transcoding todo |
| 4 | FTS5 lexical retrieval, structured filters | done (verified: test_lexical 34/34, two-stage = full join on 300 queries, ASan + TSan clean; 24,088 real memories ANY p50 1.82-1.85 ms, ALL 0.97-0.98 ms) |
| 5 | Local embeddings (user-supplied model), exact + ANN vector retrieval | wip: kernels done (verified: test_vector 7/7); square-lattice scan + rerank 2.90-3.09x faster than exact at recall@10 1.0 on placeholders (reports/vector-lattice-2026-11-06.md); vector store done (verified: test_vecstore 684/684, recall@10 1.0 vs brute force, schema v1 -> v2 migration on a real 24,088-memory store); embedding model choice and wiring todo |
| 6 | Hybrid retrieval, RRF, result explanations | done (verified: test_hybrid 763/763, fusion identical to an independent RRF recomputation on 50 queries; ASan + TSan clean) |
| 7 | Consolidation, correction, supersession, forgetting | todo |
| 8 | Context assembly, local HTTP API, llama.cpp integration | todo |
| 9 | Rust TUI | todo |
| 10 | KNC backend: the 7120A MVEX vector units perform the vector work (square-lattice scan of a card-resident index, exact rerank), plan in docs/KNC_VECTOR_PLAN.md; also SHA-256, docs/KNC_SHA256_PLAN.md | todo |
| 11 | CUDA GPU backend: future implementation, after the KNC backend | todo (future) |
| 12 | Cost scheduler, tiered caches | wip: worker pool done (default 8, up to 128; verified: test_search 19/19 under TSan, 1..128 workers identical); cost model todo |
| 13 | Offline packaging, backup, restore | todo |
| 14 | Tests, benchmarks, retrieval-quality evaluation | todo |
| 15 | Documentation pass, release | todo |
| 16 | Claude Code integration (final goal, after phases 0-15 are production ready): a hook stores every conversation turn as a memory with a timestamp, a SHA-256 content tag and an embedding (int8 square-lattice code + scale kept in RAM, full vector and text on disk); an MCP tool retrieves past turns by date, context and semantic similarity through the lattice scan and loads them into the model context as lower-trust reference text. Conversation logs stay in the local store only. | todo |

PDF import is planned after Phase 3 and depends on choosing a permissively licensed
extractor.
