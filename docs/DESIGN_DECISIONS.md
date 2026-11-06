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
  Interface: Rust TUI linking `libxksm` through `include/xksm.h`, std only (D29).
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
- Future accelerator: the Knights Corner card (`docs/KNC_SHA256_PLAN.md`; its main role is
  vector work, D18); it is used
  only where a measurement shows it beating the host for the data at hand.
- The file-level source hash is a plain serial SHA-256 so it matches `sha256sum`.

## D13. Tier-1 RAM record cache: 5 % to 50 % of the records, default 25 %

- Project direction: capacity = ceil(records * percent / 100), percent settable from 5 to
  50 (`xksm_cache_set_percent`, clamped), default 25; recomputed on every add and delete.
- Implementation: `src/cache/cache.c`. Read-through on `xksm_memory_get`, CLOCK eviction
  (entries enter unreferenced, so a one-time scan is evicted before re-read records),
  open-addressing index with Fibonacci hashing and backward-shift deletion, one allocation
  per entry; `xksm_memory_delete` evicts before returning. `xksm_cache_stats` exposes
  records, capacity, resident, hits, misses and evictions.
- Measured: hit p50 0.12 to 0.20 us against 3.6 to 3.7 us for a SQLite read
  (`reports/cache-2026-11-06.md`).
- Optional byte ceiling (`xksm_cache_set_byte_limit`, off by default, project decision):
  with a ceiling the cache holds min(25 % of records, what fits); entries are charged blob
  + entry + index slots, and `xksm_cache_stats` reports bytes and the ceiling.
- Limitation without a ceiling: RAM grows with the store. At about 400 bytes per cached record (estimate),
  25 % of 100 million records is about 10 GB.

## D14. Vector scan on the square lattice Z^n, exact rerank

- Project direction: evaluate an accelerated square-lattice representation for vector
  retrieval.
- Implementation: `src/vector/kernels.c`. Each unit vector is stored as int8 lattice
  coordinates (scale = max|x| / 127, codes in [-127, 127]) plus one float scale; the scan
  uses AVX2 `vpsignb` + `vpmaddubsw` + `vpmaddwd` (scalar fallback); the best 100
  candidates are rescored exactly in f32.
- Measured (`reports/vector-lattice-2026-11-06.md`, 100,000 clustered placeholder vectors,
  one thread): 2.90x to 3.09x faster than the exact f32 AVX2 scan at recall@10 = 1.0000;
  4x smaller scanned index.
- Open: real embeddings, threads, 4-bit codes, batched queries, NVMe-resident rerank rows.

## D15. CPU worker pool: default 8 threads, up to 128

- Project direction: 8 worker threads by default, settable 1 to 128 (`xksm_set_threads`,
  clamped); the Ryzen 7 5800X has 8 cores and 16 hardware threads.
- Implementation: `src/core/pool.c`, persistent POSIX threads; the caller is worker 0;
  one broadcast per dispatch, completion counted under the mutex. POSIX threads instead of
  C11 `<threads.h>`: with GCC 16 and glibc, ThreadSanitizer does not intercept
  `thrd_create`/`mtx_lock` (it crashed at the first instrumented function of each worker),
  so the C11 version could not be verified; the pthread version passes the whole suite
  under ThreadSanitizer.
- Parallel scans split the rows into one contiguous slice per worker, each with its own
  top-k heap, merged by the caller; results are identical for 1 to 128 workers (tested).
- Measured: 4 threads are the fastest for the lattice scan on this host; 8 are 15 to 24 %
  slower; the exact f32 scan does not scale (memory-bound).

## D16. Lexical search: quoted terms, BM25, two-stage filtering

- Source: S4. Modern.
- `xksm_search`: each whitespace-separated term is double-quoted (embedded quotes doubled),
  joined by OR (`XKSM_MATCH_ANY`, default) or AND (`XKSM_MATCH_ALL`); a trailing `*` is a
  prefix search; `XKSM_MATCH_RAW` passes FTS5 syntax through and reports syntax errors as
  `XKSM_EINVAL`. Only active memories are returned. Order: BM25, then id (deterministic).
- Default ANY because BM25 already ranks memories matching more terms first and recall
  matters for context assembly; ALL is the precise mode.
- Two stages: FTS5 ranks alone (top W = max(4 x limit, 64)), the W rows are filtered by
  point lookups, W grows fourfold if too few survive while more matches exist; snippets
  only for the final hits. Measured 2.0x faster than joining every match at p50 (ANY),
  identical results (`reports/search-2026-11-06.md`).

## D17. Vector store: one vector per (memory, model) in SQLite, derived RAM index

- Schema v2 (`memory_vec`, first migration on top of real v1 stores): unit-normalized f32
  for the exact rerank plus the int8 square-lattice code and scale; deleted with its memory
  (foreign key, cascade). A model keeps one dimension; mixing is refused.
- The RAM index (ids, codes, scales, vectors in contiguous arrays) is derived: loaded on the
  first search for a model, dropped on any put or delete.
- `xksm_vector_search`: lattice top-R (default 100) on the worker pool, exact rerank, then
  the active / namespace / category filter, widening R fourfold while too few survive.
- Embeddings are supplied by the caller; choosing and wiring a local embedding model is a
  separate, open decision.
- Concurrency fix found by ThreadSanitizer: the lazily resolved CPUID dispatch in
  `kernels.c` and `sha256.c` was a data race when first used inside a parallel scan; both
  are C11 atomics now (relaxed, a plain load on x86).

## D18. Vector acceleration on the Knights Corner card; GPU later

- Project direction (2026-11-06): the Xeon Phi 7120A MVEX vector units perform the vector
  work once the CPU system is complete; the CUDA GPU backend is future work. This replaces
  the original split (GPU for vectors, card for bitmap and integer filtering).
- Basis: the host lattice scan is memory-bandwidth bound (measured 57.7 to 58.0 GB/s at 4
  threads); the card streams 165 to 180 GB/s (measured STREAM on this card family), loads
  int8 codes with up-convert to 16 x int32, and holds a resident index in 16 GB GDDR5, so a
  search moves only the query and the candidates over PCIe. Estimates and steps:
  `docs/KNC_VECTOR_PLAN.md`.
- The CPU path stays complete; the card is selected by the cost model only where a
  measurement beats the host.

## D19. Hybrid retrieval by reciprocal rank fusion (implements D6)

- `xksm_hybrid_search`: lexical and vector lists, each to depth 50 by default, fused with
  score = sum of w / (k + rank), k = 60, weights 1 and 1 by default; a list with weight 0,
  or with no input, is not run. Ties ordered by id.
- Every hit keeps its lexical and vector rank and score (0 = absent from that list) and
  the lexical snippet: the per-result explanation the interface shows.
- Verified: identical to an independent recomputation from the two component searches on
  50 random queries (`tests/test_hybrid.c`).

## D20. Semantic vectors without a neural model

- Project direction: no embedding model; a heuristic semantic embedding written from
  scratch in C, f32, with the GPU through OpenCL.
- Method (research: Levy, Goldberg, Dagan 2015; Krebs, Paperno 2016; Arora, Liang, Ma 2017;
  Halko, Martinsson, Tropp 2011): PPMI with context distribution smoothing over a
  harmonic-window co-occurrence matrix built from the store's own memories, randomized
  truncated SVD, word vectors (U + V) Sigma^0.5, SIF memory vectors minus the first
  principal component. Random indexing (incremental, no SVD) was considered and not chosen:
  lower accuracy in the literature.
- Persisted (schema v3: `semantic_vocab`, `semantic_meta`) so queries embed into the same
  space; memory vectors are stored under `xksm-semantic` in `memory_vec`.
- Measured: `reports/semantic-2026-11-06.md`. Limits: words absent from the corpus carry no
  signal; morphology and synonyms need subword composition and general text respectively.
- GPU: the whole randomized SVD also runs in OpenCL (built when OpenCL is found; opt-in
  since 2026-11-06 with `opts.gpu` = 1 or 2); measured 7.8x to 11.3x over the first CPU
  code. OpenCL rather
  than CUDA for portability across vendors (project rule).

## D21. Semantic port: word relations computed once by a model, used without it

- Project direction: multilingual and code support, maximum retrieval precision, size no
  concern; the model only builds data offline, everything at query time runs on the CPU
  behind a vector-backend interface (`src/vector/backend.h`) so Knights Corner or GPU
  implementations can be added and verified against the CPU reference.
- Port file (`tools/port_build`): whole-word vectors from the FP32 embedding model with a
  bucket per word (Unicode script; code identifiers from the store and named repositories);
  relations are only formed inside one bucket, so a multilingual model cannot return
  translations, and code relates only to code.
- Default (project decision 2026-11-06, best balance of term MRR and paraphrase recall;
  no neural model at query time): hybrid, BM25 on the typed query + vector scan with every
  content word expanded (port v2 on the 50-query set: term MRR 0.860, paraphrase MRR 0.213,
  paraphrase recall@10 0.480; vector side alone: term 0.855, paraphrase
  MRR 0.277, recall@10 0.440); `unknown_only` expands only words the store lacks
  (term precision preserved, no paraphrase gain). `xksm_semantic_port_set` records the port
  path in the store; `xksm_semantic_recall` / `xksm recall` is the default retrieval path.
  Measured in `reports/semantic-2026-11-06.md`.
- Known gap: CJK words are not word-marked in the model vocabulary (82 Han, 19 kana keys);
  they need another word source.

## D22. Semantic build on the CPU by default (AVX2/FMA)

- Project direction: the memory embedding runs on the CPU for now; accelerators (Knights
  Corner MVEX, GPU) come later behind the same interfaces, verified against the CPU.
- CholQR2 (80 % of the SVD) is reformulated as on the GPU: Gram Y^T Y over the worker pool,
  f32 FMA partials per 256-row chunk summed in double in a fixed order (deterministic), then
  R^-1 and Y R^-1 as a parallel GEMM instead of a row-serial triangular solve. The final
  projections U = Q U_B and V = Bt U_B / sigma use the same GEMM. AVX2/FMA kernels are
  selected by CPUID; the plain C fallback runs the same arithmetic and is tested too.
- Measured on the Ryzen 7 5800X, 2,856-memory store (vocabulary 16,972, nnz 1,912,601):
  SVD 3.320 s -> 0.449 s (7.39x), whole build 3.891 s -> 1.008 s; the RTX 3090 Ti takes
  0.339 s. Retrieval metrics are identical to the previous model.

## D23. Structured import: JSON, JSONL, CSV as rendered records

- A record (JSON top-level array element or whole document; JSONL line; CSV data row under
  its header) becomes text lines `path: value` (`a.b[2].c: text`; CSV: header name, empty
  header = `column N`). Strings are unescaped (surrogate pairs to UTF-8, lone surrogates
  refused, `\u0000` to a space), numbers and booleans kept as written, null and empty
  values omitted. Indexing sees words, not syntax; provenance points at the raw record.
- Strict grammar (RFC 8259, RFC 4180), nesting limit 256; any error refuses the whole
  import with line and column, like every import (one transaction).
- Records longer than 2 x chunk_target are split like text; every piece keeps the record's
  provenance.
- Verified: test_structured 68 checks, including 3,000 random mutations of valid inputs
  (accepted 549, refused 2,378, 0 partial imports or unexpected statuses) under ASan/UBSan.

## D24. Agent harness memory: every turn, per harness, exact time

- Project direction: every turn of Claude Code (and later omp, other harnesses) is logged
  into XKS-Memory and embedded for retrieval, with its exact date and time, segregated per
  harness with session management, installed as its own application with the memories
  outside the repository.
- A turn is a source (`xksm-turn://HARNESS/SESSION/ROLE/TIME_US`) with one memory per chunk;
  dialogue and trace (tool calls, results, thinking) are separate namespaces; recall into a
  prompt uses the dialogue namespace of the same harness and skips the current session.
- Claude Code: plugin (hooks + MCP server); omp: extension; others: `xksm turn` / `xksm
  context`. Verified in Claude Code 2.1.316 and omp 18.17.0 (recall crossed sessions).
- Schema 4: `kv` (state such as transcript offsets) and `session` (registry).
- Reference: docs/AGENTS_INTEGRATION.md.

## D25. Derived indexes as memory-mapped snapshots

- Each hook is a short process; rebuilding the vector index from 150,934 SQLite rows took
  0.211 s per call and the port targets 470 MB of copies. The index is now a mapped snapshot
  (`.vidx-<model hash>`) plus a RAM tail of rows appended after it, and the port targets are
  square-lattice int8 codes in a mapped `.ptgt` with exact f32 rerank on the port in place.
- Destructive changes bump a vector epoch and remove the snapshots at once (a deleted memory's
  vector must not survive on disk); appends never rewrite the snapshot.
- Measured: index load 0.031 s (6.8x); port expansion identical to f32 on all 642 expansions
  of the evaluation set; UserPromptSubmit 0.17 to 0.18 s from 0.63 to 0.66 s.

## D26. Function words dropped from ANY lexical queries

- In ANY mode a query of mostly function words matched nearly every memory (BM25 then ranks
  all of them): 0.212 s on 150,951 memories. A fixed English list (no length rule, so `rm`,
  `db`, `io` stay) is dropped when the query has other terms; ALL and RAW are unchanged;
  `XKSM_KEEP_STOPWORDS=1` restores the old behaviour.
- Measured: 0.039 s; term MRR unchanged in every mode; paraphrase MRR BM25 0.224 to 0.322,
  hybrid 0.206 to 0.272, default 0.213 to 0.251. The gain was not predicted: the data showed
  the function words were distorting BM25 rankings of paraphrase questions.

## D27. Terminal interface in Rust

- Crate `tui/` over the C ABI (first built on ratatui; replaced by D29); safe wrapper copies every C
  allocation into owned values. Views: sessions and turns, search (recall or words, three
  scopes), embedding state and coverage with rebuild, store and logging state.

## D28. Two 4-core pools: embedding and recall run on separate cores

- Project direction: multi-core embedding and recall, 4 cores each (the 5800X has 8).
- Each handle has a recall pool (vector scans, port expansion) and an embedding pool (table
  build), 4 workers each by default (`xksm_set_threads`, `xksm_set_embed_threads`,
  `XKSM_THREADS`, `XKSM_EMBED_THREADS`), so a build never takes recall's cores.
- Co-occurrence is partitioned by the first word of each pair (worker w owns a % nw == w):
  every pair is summed in the serial order, so pair weights are bit-identical with no locks;
  PPMI rows stay within one partition. The CSR build sorts triplets with a two-pass stable
  counting sort instead of qsort (16.95 M triplets).
- Recall runs BM25 in a thread on a second read-only connection while the vector branch runs
  on the calling thread; the fusion is unchanged.
- Measured (150,973 memories): build 17.18 s (8 threads, before) -> 11.02 s on 4 threads
  (cooc 4.65 -> 2.07 s, PPMI 3.68 -> 0.80 s), pairs 20,764,528 and non-zeros 16,951,298
  identical; 1-thread control 22.31 s. Recall 0.16 to 0.18 s -> 0.14 s, output identical.
  The recall pool's size made no difference (4 vs 8 threads: 0.147 to 0.151 vs 0.146 to
  0.153 s): its parallel part is milliseconds; the gain came from running the two branches
  concurrently. Evaluation metrics after a rebuild: identical.

## D29. The terminal interface uses no terminal crates

- Project rule: ratatui is never used, in any project. Its backend crate (crossterm) went
  with it: the crate now has no dependencies at all.
- `tui/src/term.rs` is the whole terminal layer: raw mode through `tcgetattr` /
  `cfmakeraw` / `tcsetattr` (restored on exit and on panic), the size through
  `ioctl(TIOCGWINSZ)` each frame, input through `poll` + `read` with an own decoder (CSI and
  SS3 keys, UTF-8; a lone ESC followed by a key in the same read is Esc then that key),
  output through a double-buffered cell grid (character, `wcwidth` display width, colour,
  bold) that writes only the changed cells, in one write per frame, and only when something
  changed.
- Measured, the same scripted session (search, scrolling, every view), interleaved, 2 runs
  each, 200 x 50 terminal, 150,982-memory store: CPU 0.61 / 0.61 s vs 0.94 / 0.95 s with
  ratatui (35 % less, library work included); peak RSS 188,668 to 189,048 KB vs 187,536 to
  187,848 KB (mappings of the store dominate); terminal output 89,916 / 89,923 bytes vs
  88,644 / 88,630 (1.4 % more: each style change writes a full SGR reset).
