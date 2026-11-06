# Design decisions

Each entry: source, concept, adaptation, implementation, limitations, alternatives, reason.
"Historical" marks a concept documented in S1/S2; everything else is a modern engineering
choice.

## D1. SQLite is the authoritative store

- Source: S3. Modern.
- Implementation: one database file per memory store, WAL journal, every mutation in a
  transaction, `PRAGMA user_version` for the schema version, ordered migrations in C.
- Limitations: one writer at a time; large vector data is kept outside it (D3).
- Alternatives: custom log-structured store (more code, no gain at this scale), LMDB (no
  full-text search).
- Reason: transactional, portable, public domain, FTS5 built in.

## D2. Lexical retrieval with FTS5 external content

- Source: S4. Modern.
- Implementation: `memory_fts` is an external-content FTS5 table over `memory.text`, kept in
  sync by triggers, tokenizer `unicode61` with `_` as a token character, ranked by `bm25()`.
  The query layer quotes non-bareword identifiers.
- Limitations: no substring search inside tokens (a trigram table can be added later).
- Alternatives: Tantivy (S7, Rust; the core is C), own inverted index.
- Reason: already inside the authoritative store, rebuildable with `'rebuild'`.

## D3. Vectors in a separate flat file, exact search first

- Modern.
- Implementation: f32 row-major file keyed by memory ID and embedding model ID; exact
  cosine and inner product with a CPU kernel; ANN (IVF) after exact search is measured.
- Limitations: exact search is O(N d) per query.
- Alternatives: vectors as SQLite BLOBs (slower scans), FAISS (S6, evaluated in phase 5).
- Reason: contiguous memory for SIMD, CUDA and KNC kernels; rebuildable from SQLite-held
  embeddings.

## D4. Working memory in front of persistent memory (historical)

- Source: S2, two retention tiers per site.
- Adaptation: working memory lives in RAM with a time bound and is never authoritative;
  persistent categories live in SQLite; archive is an optional third tier.

## D5. Processing stages as plugins (historical)

- Source: S2, plugin analysis engines.
- Implementation: ingestion and consolidation are ordered arrays of `xksm_stage`
  (name, function pointer, cost counter). New stages need no change to the pipeline code.

## D6. Federated retrieval and fusion (historical + S5)

- Source: S2, query fan-out and aggregation; S5 for the fusion rule.
- Implementation: lexical, vector, temporal and structured retrievers each return a ranked
  list; reciprocal rank fusion, score(d) = sum over lists of 1 / (k + rank(d)), default
  k = 60 (S5), per-list weights configurable. Each result keeps its per-list ranks so the
  interface can show why it was retrieved.

## D7. Cost-based stage and backend selection (historical)

- Source: S2, rule count versus throughput.
- Implementation: cost = transfer + dispatch + execution + aggregation, each coefficient
  measured on the running machine; a backend is used only when its total cost beats the
  CPU path.

## D8. Language split

- Core, CLI, API: C17. CUDA backend: CUDA C++. KNC backend: C for native KNC.
  Interface: Rust TUI (ratatui + crossterm) linking `libxksm` through `include/xksm.h`.
- Reason: project direction; a C ABI is the narrowest stable boundary for every consumer.

## D9. Deletion

- Source: S4 (`'secure-delete'`), S3 (`PRAGMA secure_delete`).
- Implementation: delete row and its FTS entry in one transaction with FTS5 secure-delete
  on, then remove its vector rows and evict caches before returning.
- Limitation: no guarantee of physical erasure on SSDs, snapshots or backups.

## D10. Embeddings are external and optional

- Modern. Embeddings come from a user-run local model (for example a llama.cpp server on
  127.0.0.1); with no model, semantic retrieval is reported as unavailable, never faked.

## D11. Token counts

- Use the model's tokenizer when an integration provides it; otherwise an estimate that is
  labelled as an estimate in every output.

## D12. Checksums are SHA-256

- Modern. `source.sha256` (dedup key) and `memory.checksum` are 32-byte SHA-256 digests.
- CPU: `src/core/sha256.c`, portable scalar C plus an x86 SHA-NI kernel
  (`sha256rnds2`/`sha256msg1`/`sha256msg2`) selected at run time by CPUID. On the Ryzen 7
  5800X: SHA-NI median 2.369 GB/s, scalar 0.395 GB/s, OpenSSL 2.356 GB/s
  (`reports/sha256-2026-11-06.md`). Every CPU without SHA-NI uses the scalar kernel.
- SHA-512 was built and measured first (`reports/sha512-2026-11-06.md`: CPU 0.577 GB/s, a
  batched CUDA kernel 10.77 GB/s end to end, bound by the host-to-device copy) and then
  replaced: the 5800X has no SHA-512 instructions, SHA-256 is 4.1x faster on its CPU, and
  32-byte digests are smaller (measured store sizes in the SHA-512 report).
- Future accelerator: the Knights Corner card, see `docs/KNC_SHA256_PLAN.md`; it is used
  only where a measurement shows it beating the host for the data at hand.
- The file-level source hash is a plain serial SHA-256 so it matches `sha256sum`.
