# Changelog

## Unreleased

- Repository created: license, project documents, directory layout.
- Phase 1: research notes, source matrix, design decisions D1-D11.
- Phase 2: CMake build, `libxksm` C ABI (`include/xksm.h`), schema v1 (source, memory, external-content FTS5 with secure-delete, change log), ordered migrations with refusal of newer stores, `xksm init|info|version`, schema test.
- Checksums: SHA-256 (32-byte schema columns) with a run-time-selected SHA-NI kernel and a portable scalar kernel; FIPS and OpenSSL cross-checked; benchmark `bench_sha256`. SHA-512 (CPU and batched CUDA) was built, measured and replaced; measurements kept in `reports/sha512-2026-11-06.md`.
- Plan for SHA-256 on the Knights Corner card: `docs/KNC_SHA256_PLAN.md`.
- Memory API: `xksm_memory_add` (RFC 3629 UTF-8 validation, CRLF and whitespace normalization, SHA-256 checksum, per-namespace dedup returning `XKSM_EXISTS`), `xksm_memory_get`, `xksm_memory_free`, `xksm_memory_delete` (one transaction; change log keeps id and action only). Test proves deleted text leaves the database file and WAL.
- Cached prepared statements; `xksm_memory_add_batch` (one transaction, per-item status, all-or-nothing on I/O failure); `bench_insert`; measurements and FTS5 cost ablation in `reports/insert-2026-11-06.md`.
- Tier-1 RAM record cache: capacity 25 % of the records (ceil(records / 4)), read-through, CLOCK eviction, delete propagation, `xksm_cache_stats`; `bench_get`; `reports/cache-2026-11-06.md`.
- Explicit import: `xksm_import_file`, `xksm_import_buffer`, `xksm import <store> <file>...`; text and Markdown chunking (paragraph packing to 1024 bytes, headings start chunks, UTF-8 safe splits), source dedup by SHA-256, provenance byte ranges, one transaction per import; real-text measurements in `reports/insert-2026-11-06.md`.
- Optional RAM ceiling for the record cache: `xksm_cache_set_byte_limit` (off by default); stats report bytes in use.
- Record cache size settable from 5 % to 50 % of the records (`xksm_cache_set_percent`, default 25 %).
- Vector kernels (f32 AVX2/FMA, square-lattice int8 AVX2, scalar references) and `bench_vector`; lattice scan + exact rerank measured 2.90x to 3.09x faster than the exact scan at recall@10 1.0 on placeholder data.
- CPU worker pool (POSIX threads), default 8 threads, settable 1 to 128 (`xksm_set_threads`, `xksm_get_threads`); parallel exact and square-lattice top-k; `bench_threads`; suite clean under ThreadSanitizer.
- Lexical search: `xksm_search`, `xksm search <store> <words>...`; quoted terms, ANY/ALL/RAW modes, BM25 order with id tiebreak, active memories only, namespace and category filters, snippets; two-stage filtering (2.0x at p50); `bench_search`; `reports/search-2026-11-06.md`.
- Vector store: schema v2 (`memory_vec`, migrated from v1), `xksm_vector_put`, `xksm_vector_search` (lattice scan on the worker pool, exact rerank, filters, deletion propagates). Fixed a data race in lazy CPUID dispatch found by ThreadSanitizer.
- Plan change: the Knights Corner MVEX vector units are the vector accelerator (`docs/KNC_VECTOR_PLAN.md`, D18); the CUDA GPU backend becomes future work (roadmap phases 10 and 11 swapped).
- Roadmap phase 16: Claude Code integration (hook + MCP tool) as the final goal.
