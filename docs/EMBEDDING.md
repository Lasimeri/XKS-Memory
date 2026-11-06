# The XKS-Memory embedding and recall system

This document describes, completely and from the code, how XKS-Memory turns text into
vectors and how it recalls memories. It is the reference for the default setup chosen on
2026-11-06: **hybrid + port v2, every content word expanded**.

Every number here was measured on the development machine (Ryzen 7 5800X, 8 pool workers,
RTX 3090 Ti) on the private evaluation store described in section 9. Commands are given
with each number; raw output stays in `benchmarks/out/` (not committed), curated results are
in `reports/semantic-2026-11-06.md`.

## Contents

1. Summary
2. Rules the design follows
3. Architecture
4. Stage A: the count-based semantic model (built per store, on the CPU)
5. Stage B: the semantic port (built once, offline, with an embedding model)
6. Stage C: recall (the query path, no model)
7. Vector storage and the square-lattice scan
8. Accelerator interface (Knights Corner, GPU)
9. Evaluation: method and every configuration measured
10. Verification that no model runs at query time
11. Operating it: commands
12. Parameter reference
13. API reference
14. Limits and open work
15. Decision history

## 1. Summary

| Question | Answer |
|---|---|
| What embeds memories? | A count-based semantic model learned from the store's own memories: PPMI over word co-occurrence, factored by randomized SVD into 128-dimension word vectors; a memory vector is the SIF-weighted average of its word vectors with the common component removed. No neural network. |
| Where does that run? | On the CPU (AVX2/FMA kernels, worker pool). 0.959 s for 2,856 memories. An OpenCL GPU path exists and is opt-in. |
| What does the embedding model do? | It is run once, offline, to produce the **semantic port**: a static table of 434,204 word vectors (1,339,261,952 bytes). After that the model is not needed. |
| What happens at query time? | Hybrid recall: BM25 over the query as typed, fused by reciprocal rank fusion with a vector search whose query vector adds, for every content word, its nearest store words in the port (same script or code bucket), weighted by cosine. |
| How good is it? | On the 50-query set: term-query MRR 0.860 (recall@10 1.000), paraphrase-query MRR 0.213 (recall@10 0.480). The full neural model would give 1.000 and 0.321, but it is excluded at query time by rule. |
| How fast? | `xksm recall`: 0.06 s per process, including mapping the port. |
| Does query time touch llama.cpp or the GGUF? | No. Verified with `strace` (section 10). |

## 2. Rules the design follows

- **No neural model at query time, ever.** Running the embedding model continuously is not an
  option for this project. The model may be used offline to produce data (the port); the
  library, the CLI and recall never load it.
- **CPU first.** Everything at run time runs on the CPU with AVX2/FMA where the CPU has it
  and plain C otherwise. Accelerators (the Xeon Phi 7120A MVEX units, a GPU) are added later
  behind interfaces that already exist, and are verified against the CPU reference.
- **Maximum retrieval precision; size is not a concern.** The port is 1.34 GB because more
  words cover more queries.
- **Multilingual and code support without cross-contamination.** Words relate only to words
  of the same Unicode script, and code identifiers only to code identifiers.
- **Offline, local, explicit.** Only data the owner imports is processed; no network access.
  The word list for the port comes from local documentation and man pages, nothing is
  downloaded.
- **Measured, not assumed.** Every option was accepted or rejected on measured recall
  (section 9), including the dead ends.

## 3. Architecture

```
╔══════════════════════════════════════════════════════════════════════════════╗
║ OFFLINE, ONCE (RTX 3090 Ti; llama.cpp + nomic-embed-text-v2-moe F32 GGUF)    ║
║ ┌───────────────────────────┐     ┌──────────────────────────────────────┐   ║
║ │ word sources              │────▶│ tools/port_build                     │   ║
║ │ model whole words 142,311 │     │ embed each word alone, unit-normalize│   ║
║ │ store vocabulary   13,277 │     │ 434,204 keys in 49.0 s               │   ║
║ │ code identifiers   92,832 │     └──────────────────┬───────────────────┘   ║
║ │ doc + man words   185,784 │                        ▼                       ║
║ └───────────────────────────┘     ┌──────────────────────────────────────┐   ║
║                                   │ xksm-port-nomic-f32-v2.port          │   ║
║                                   │ XKSMPRT1, 768 x f32, 1,339,261,952 B │   ║
║                                   └──────────────────┬───────────────────┘   ║
╚══════════════════════════════════════════════════════╪═══════════════════════╝
                                                       ▼ mmap (xksm port)       
╔══════════════════════════════════════════════════════════════════════════════╗
║ RUN TIME, CPU ONLY (libxksm: SQLite + AVX2/FMA + pthread pool, no model)     ║
║ ┌───────────────────────────┐     ┌──────────────────────────────────────┐   ║
║ │ Stage A: xksm semantic    │     │ Stage C: xksm recall                 │   ║
║ │ tokens, co-occurrence,    │     │ lexical: BM25 on the typed query     │   ║
║ │ PPMI, rSVD (CholQR2 AVX2) │     │ vector: SIF query + port neighbours  │   ║
║ │ SIF memory vectors - PC   │     │ lattice scan, exact rerank           │   ║
║ │ 0.959 s / 2,856 memories  │     │ RRF k = 60, 0.06 s per process       │   ║
║ └─────────────┬─────────────┘     └──────────────────┬───────────────────┘   ║
║               ▼                                      ▼                       ║
║ ┌──────────────────────────────────────────────────────────────────────────┐ ║
║ │ store.xksm: memory, memory_fts, memory_vec (f32 + int8 code),            │ ║
║ │ semantic_vocab (words, counts, vectors), semantic_meta (dim, pc, port)   │ ║
║ └──────────────────────────────────────────────────────────────────────────┘ ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

Source files:

| File | Role |
|---|---|
| `src/semantic/semantic.c` | Stage A pipeline, persistence, query embedding, expansion, recall |
| `src/semantic/sparse.c`, `rsvd.c`, `linalg.c` | CSR matrices, randomized SVD, Gram / CholQR2 / GEMM (AVX2/FMA + plain C) |
| `backends/opencl/` | optional GPU randomized SVD (opt-in) |
| `src/semantic/port.c` | port file: mmap, key hash, buckets, stop words |
| `src/vector/` | vector store, square-lattice codes, AVX2 kernels, vector-backend interface |
| `src/retrieval/lexical.c`, `hybrid.c` | BM25 search, reciprocal rank fusion |
| `tools/port_build.c` | offline port builder (links llama.cpp; opt-in CMake target) |
| `tools/port_words.sh` | local word-list harvester for the port |

## 4. Stage A: the count-based semantic model

Built by `xksm_semantic_build` (CLI `xksm semantic STORE`) from all active memories of the
store. It learns which words occur near which other words in *this* store, so it adapts to
the owner's vocabulary (project names, code identifiers, jargon) with no training data.

### 4.1 Tokenization

- A token is a maximal run of bytes in `[A-Za-z0-9_]` or any byte >= 0x80 (so every UTF-8
  multi-byte character is part of a word and scripts other than Latin are kept whole).
- ASCII letters are lowercased; other bytes are kept as they are.
- Tokens longer than 63 bytes are truncated to 63 bytes.
- The same tokenizer is used for memories, for queries, and as the key form of the port.

### 4.2 Vocabulary

- Every token is counted; words with count >= `min_count` (default 2) are kept. Rarer words
  are dropped from the model (they still count for BM25).
- The build refuses a vocabulary of `dim + 16` words or fewer (the SVD needs rank).
- Evaluation store: 545,209 tokens, vocabulary 16,972.

### 4.3 Co-occurrence

- Within each memory, after removing dropped words, every pair of words at distance
  d = 1..`window` (default 5) adds weight `keep(a) * keep(b) / d` to both (a, b) and (b, a):
  a symmetric harmonic window.
- `keep(w)` is a deterministic subsampling weight: with f(w) = count(w) / kept tokens,
  `keep(w) = sqrt(t / f(w))` if f(w) > t, else 1, with t = `sub_t` (default 1e-4). Frequent
  words are damped by weight instead of by random dropping, so the build is reproducible
  bit for bit.
- Evaluation store: 2,139,188 distinct pairs.

### 4.4 PPMI with context distribution smoothing

For a pair (a, b) with weight n(a, b), row sums r(w) = sum over b of n(w, b), and
Ta = sum over w of r(w)^cds:

```
PMI(a, b) = log( n(a, b) * Ta / ( r(a) * r(b)^cds ) ) - log(shift)
PPMI(a, b) = max(PMI(a, b), 0)
```

cds = 0.75 (Levy, Goldberg and Dagan 2015) smooths the context distribution so rare
contexts do not dominate; shift defaults to 1 (no shift). Positive entries form a sparse CSR
matrix A (vocabulary x vocabulary) and its transpose. Evaluation store: 1,912,601 non-zeros.

### 4.5 Randomized truncated SVD

Halko, Martinsson and Tropp 2011 (Algorithm 4.4 with 5.1): rank k = `dim` (default 128),
oversampling p = 16 (l = 144 columns), q = `power_iters` (default 4) power iterations, a
fixed seed (0x5EED), so the result is deterministic.

```
Omega = Gaussian (n x l)
Y = A Omega;            Y = CholQR2(Y)
repeat q times:  Z = A^T Y; Z = CholQR2(Z);  Y = A Z; Y = CholQR2(Y)
Bt = A^T Y;  B B^T = Bt^T Bt (l x l);  eigen-decomposition by Jacobi rotations
sigma_i = sqrt(eigenvalue_i);  U = Y U_B;  V = Bt U_B / sigma
```

CholQR2 is two rounds of { G = Y^T Y; R = chol(G); Y = Y R^-1 }. On the CPU it is the
reformulation of 2026-11-06 (D22): the Gram matrix is accumulated over the worker pool with
f32 FMA partial sums per 256-row chunk, summed in double in a fixed order (deterministic for
a given thread count), and Y R^-1 is a parallel GEMM instead of a row-serial triangular
solve. The final projections use the same GEMM. Kernels are selected by CPUID (AVX2 + FMA,
else plain C); both are tested against double-precision references (`test_linalg`: Gram
relative error <= 1.17e-06, GEMM <= 2.50e-07, CholQR2 vs reference <= 1.12e-08,
orthogonality <= 2.11e-07).

| SVD path (`bench_semantic STORE 1`, one run each) | SVD | whole build |
|---|---|---|
| CPU before 2026-11-06 (serial CholQR2) | 3.320 s | 3.891 s |
| CPU, AVX2/FMA + pool (default) | 0.449 s | 1.008 s |
| OpenCL, RTX 3090 Ti (opt-in, `gpu` = 1 or 2) | 0.339 s | 0.882 s |

Retrieval with the CPU-built model is identical to the previous model on every metric.

### 4.6 Word vectors

```
word vector(w) = normalize( (U[w] + V[w]) * sqrt(sigma) )      (element-wise)
```

Adding the context vectors (U + V) and weighting by sigma^0.5 follow Levy, Goldberg and
Dagan 2015 (eigenvalue weighting p = 0.5, adding context vectors).

### 4.7 Memory vectors (SIF)

Arora, Liang and Ma 2017 (smooth inverse frequency):

```
sif(w) = a / (a + p(w)),  a = sif_a (default 1e-3),  p(w) = count(w) / kept tokens
m = (1 / K) * sum over the K known tokens w of the memory of sif(w) * vector(w)
pc = first singular vector of the memory matrix (power iteration, at most 100 rounds)
memory vector = m - (m . pc) pc
```

The memory vector is unit-normalized when stored (section 7). `pc` is persisted so query
vectors get the same removal.

### 4.8 Persistence

One transaction replaces the model: `semantic_vocab(id, word, count, vec)`,
`semantic_meta` (`dim`, `pc`, `params`; the `port` key survives rebuilds) and the
`memory_vec` rows under the model name `xksm-semantic`.

### 4.9 Build timings (`xksm semantic`, CPU, evaluation store)

| tokens | co-occurrence | PPMI | SVD | memory vectors | store | total |
|---|---|---|---|---|---|---|
| 0.021 s | 0.149 s | 0.330 s | 0.400 s | 0.010 s | 0.049 s | 0.959 s |

### 4.10 What Stage A cannot do alone

A word the store has never seen (or seen once) has no vector, and two words that never share
contexts in the store are unrelated, however close in meaning. On paraphrase queries the
diagnosis was direct: 15 of the 139 query words were out of vocabulary, and they were the
content words. Stage B exists to close that gap without a model at query time.

## 5. Stage B: the semantic port

The port carries the knowledge of the embedding model as data: a vector per word, computed
once. The run-time engine uses it only to find which **store words** are related to a query
word; it never mixes port vectors into the store's vector space.

### 5.1 The model used to build it

`nomic-embed-text-v2-moe`, F32 GGUF (1,907,985,280 bytes), through the llama.cpp build in
`~/llama-phi/llama.cpp`, on the RTX 3090 Ti. F32 was used for numerical accuracy (the Q6_K
rows had been measured at cosine 0.999865 to F32, so quantization was not the issue, but F32
removes the question).

### 5.2 Word sources (port v2)

| Source | New keys | How |
|---|---|---|
| Model vocabulary whole-word pieces | 142,311 | SentencePiece pieces starting with U+2581 and attribute NORMAL, 2 to 47 bytes, lowercased |
| Store vocabulary (`semantic_vocab`) | 13,277 | every model word of the store |
| Code identifiers | 92,832 | identifiers (contain `_`, camelCase, or letters with digits), seen at least twice, from the store's memories and from 2,237 source files (45.7 MB) of `~/XKS-Memory` and `~/llama-phi/llama.cpp` (`.c .h .cpp .hpp .cc .cu .cl .rs .py .sh .cmake`, `CMakeLists.txt`; hidden and `build*` directories skipped) |
| Local documentation words (`--words=`) | 185,784 | 208,081 words harvested from 779 text files of `/usr/share/doc` and 32,427 man pages (`tools/port_words.sh`) |
| **Total** | **434,204** | |

The documentation words were added because port v1 (248,876 keys, no word list) contained
only 2 of the 15 paraphrase words the store lacked: common English words such as
"attribute" or "deleting" are several pieces in the model vocabulary, so they are not
whole-word pieces. v2 contains 14 of the 15 (only "pictograms" is missing).

### 5.3 Buckets

Each key carries a bucket; relations form only inside one bucket.

| Bucket | Rule (first code point, unless code) | Keys in v2 |
|---|---|---|
| code | contains `_` inside, or letters mixed with digits, or seen camelCase | 97,975 |
| latin | a-z, U+00C0-U+024F, U+1E00-U+1EFF | 262,427 |
| cyrillic | U+0400-U+052F | 23,023 |
| greek | U+0370-U+03FF | 3,172 |
| arabic | U+0600-U+06FF, U+0750-U+077F | 10,724 |
| hebrew | U+0590-U+05FF | 2,831 |
| devanagari | U+0900-U+097F | 5,359 |
| hangul | U+AC00-U+D7AF | 2,451 |
| thai | U+0E00-U+0E7F | 394 |
| han | U+4E00-U+9FFF, U+3400-U+4DBF | 82 |
| kana | U+3040-U+30FF | 19 |
| other | everything else (punctuation-led pieces, other scripts) | 25,747 |

Why buckets: the model is multilingual, so its nearest neighbour of an English word is often
the same word in another language (the neighbour probe of 2026-11-06 showed exactly that for
distilled static vectors). Inside one bucket, "deleting" relates to "deletion", not to its
translation; code identifiers relate to code identifiers.

### 5.4 How each word is embedded

Each key is tokenized by the model's own tokenizer with BOS/EOS, encoded alone, pooled by the
model's pooling, and normalized to unit length. 256 sequences per batch on the GPU.

### 5.5 File format `XKSMPRT1`

```
offset 0   "XKSMPRT1"                         8 bytes
offset 8   u32 dim (768)                      little endian
offset 12  u32 n (434,204)
offset 16  n entries: u8 bucket, u8 len, len bytes of the lowercase key
           zero padding to a multiple of 64
           n x dim f32 unit vectors, row-major, in entry order
```

The size is checked exactly at open (header + keys + padding + n * dim * 4 must equal the
file size), so a truncated or foreign file is refused. The vectors are used in place from a
read-only `mmap`; at open only the keys are hashed (FNV-1a, open addressing, at most 50 %
load).

### 5.6 Building it

```
cmake -S . -B build -DXKSM_LLAMA_DIR=$HOME/llama-phi/llama.cpp \
      -DXKSM_LLAMA_BUILD=$HOME/llama-phi/llama.cpp/build-native-cuda
cmake --build build --target port_build
tools/port_words.sh words.txt /usr/share/doc /usr/share/man
build/port_build MODEL.f32.gguf STORE OUT.port --words=words.txt REPO_DIR...
```

v2 build: 49.024 s embedding on the RTX 3090 Ti, 51.606 s in total. The port depends on the
store only through the store-vocabulary source; a new store can use the same port (its own
words are then covered by the other sources where they exist).

## 6. Stage C: recall

`xksm_semantic_recall` (CLI `xksm recall STORE words...`) is the default retrieval path.

### 6.1 Lexical list

BM25 over FTS5 on the query exactly as typed: every term quoted (no FTS5 syntax injection),
match ANY, ranked by BM25 then id, two-stage (FTS ranks a window, filters by point lookup,
widens if short). Depth 50 hits. The port never changes this side, so exact names, error
strings and identifiers are found as they are.

### 6.2 Vector list: the expanded query vector

For the query, tokenized as in 4.1:

```
for every token w:
    if w is in the store vocabulary:
        out += sif(w) * vector(w);  K += 1                      (typed word, full weight)
    if w is a stop word or shorter than 3 bytes or all digits: continue
    if w is in the port (bucket b):
        candidates = store words of bucket b that are in the port and not stop words
        N = the k + 1 candidates with the highest dot product port(w) . port(t)
        for each t in N, best first, until k taken:
            stop if cosine < min_cos;  skip t == w
            out += alpha * cosine * sif(t) * vector(t)              (related word)
out = out / max(K, 1)
out = out - (out . pc) pc
```

Defaults (fixed before measuring, not tuned on the evaluation set): k = 5, min_cos = 0.6,
alpha = 0.5. Every content word is expanded, including words the store knows
(`unknown_only` = 0); this is the chosen setup. The per-bucket candidate matrices are built
once per handle on first use (store words with a port vector, copied contiguously).

The neighbour search is one call through the vector-backend interface
(`xksm_vbackend.topk_dot`, section 8), currently the CPU AVX2/FMA exact scan over the pool.

Example trace (`XKSM_EXP_DEBUG=1`, port v2): deleting -> deletion 0.873, deletions 0.790,
delete 0.676, deletes 0.653, deleted 0.650; recursively -> recursive 0.893, recursion
0.749; rotating -> rotated 0.901, rotation 0.853, rotates 0.820, rotations 0.821, rotate
0.790; attribute -> attributes 0.947, attributed 0.686, aspect 0.635, features 0.615,
feature 0.603.

### 6.3 Vector search

The query vector is normalized and searched over `memory_vec` (model `xksm-semantic`) by the
square-lattice scan with exact rerank (section 7). Depth 50 hits.

### 6.4 Fusion

Reciprocal rank fusion (Cormack, Clarke and Buettcher 2009):

```
score(memory) = w_lexical / (k + lexical_rank) + w_vector / (k + vector_rank)
```

k = 60, both weights 1, a list contributes only if the memory is in it. The 10 best fused
hits are returned with both ranks and both raw scores, so every result explains itself
(`xksm recall` prints `lexical rank`, `vector rank`).

### 6.5 Fallback

If no query word is known to the model and none has a related store word, the vector side is
empty and recall returns the lexical list alone (no error). If no port is recorded, recall
reports it (`XKSM_ENOTFOUND`).

### 6.6 Cost

`/usr/bin/time build/xksm recall STORE ...`: 0.06 s wall in two runs, RSS 537 MB (mostly the
shared read-only mapping of the port). That includes opening the store, mapping the port,
hashing 434,204 keys, building the bucket candidates, the expansion, BM25, the scan and
fusion. In a long-lived process (the planned MCP tool) the open costs are paid once.

## 7. Vector storage and the square-lattice scan

- Each memory vector is stored unit-normalized as f32 plus a square-lattice int8 code: scale
  = max|x| / 127, code = round(x / scale) clamped to +-127 (Z^n lattice, one scale per
  vector), in `memory_vec(memory_id, model, dim, vec, code, scale)`.
- A RAM index of the codes is scanned on the worker pool with AVX2 (`vpsignb`,
  `vpmaddubsw`, `vpmaddwd`; pair sums peak at 2 * 127 * 127 = 32,258 < 32,767, no
  saturation); the best 100 candidates are rescored exactly in f32 (AVX2/FMA, 4
  accumulators).
- Measured on placeholder vectors: lattice + rerank 2.90x to 3.09x faster than the exact f32
  scan at recall@10 1.0 (`reports/vector-lattice-2026-11-06.md`).

## 8. Accelerator interface

`src/vector/backend.h` defines the vector backend:

```c
typedef struct xksm_vbackend {
    const char *name;
    size_t (*topk_dot)(void *ctx, const float *mat, size_t rows, size_t dim,
                       const float *q, size_t k, xksm_hit *out);
    void *ctx;
} xksm_vbackend;
```

The CPU implementation (`xksm__vbackend_cpu`) is the reference. A Knights Corner backend
(MVEX units of the 7120A, plan in `docs/KNC_VECTOR_PLAN.md`) or a GPU backend implements the
same call and is accepted only if it returns the same hits as the CPU on the same inputs.
The Stage A SVD already has a GPU implementation (OpenCL) verified against the CPU (sigma
difference 2.52e-07) and is opt-in.

## 9. Evaluation

### 9.1 Method

- Corpus: the owner's own Claude Code memory notes (361 files, 2,856 memories after chunking),
  a store with known content, private (never committed).
- 50 queries, each with the memory files that answer it: 25 **term** queries (use words that
  occur in the answer) and 25 **paraphrase** queries (describe the answer in other words).
- A hit is correct if it comes from an answering file. Metrics: recall@1, @5, @10 and MRR
  (mean reciprocal rank of the first correct hit), per class.
- Tool: `benchmarks/eval_retrieval STORE QUERIES MODE` (modes `any`, `vec`, `hybrid`,
  `port`, `porthybrid`, and model comparison modes); `XKSM_PORT` selects the port for
  `port` and `porthybrid`.
- Resolution: one query is 0.04 of a recall value; differences of 1 to 3 paraphrase queries
  are inside what this set can resolve. A larger held-out set is open work.

### 9.2 Every configuration measured

| Configuration | model at query time | term MRR | term r@10 | para MRR | para r@10 |
|---|---|---|---|---|---|
| BM25 only | no | 0.923 | 1.000 | 0.224 | 0.440 |
| Stage A vectors only | no | 0.887 | 0.960 | 0.192 | 0.320 |
| Hybrid, no port | no | 0.920 | 1.000 | 0.206 | 0.400 |
| Port v1, every word, vectors only | no | 0.855 | 0.960 | 0.279 | 0.480 |
| Port v1, every word, hybrid | no | 0.860 | 1.000 | 0.239 | 0.480 |
| Port v2, unknown words only, vectors only | no | 0.887 | 0.960 | 0.148 | 0.280 |
| Port v2, unknown words only, hybrid | no | 0.920 | 1.000 | 0.192 | 0.440 |
| Port v2, every word, vectors only | no | 0.855 | 0.960 | 0.277 | 0.440 |
| **Port v2, every word, hybrid (default)** | **no** | **0.860** | **1.000** | **0.213** | **0.480** |
| nomic Q4_0 neural embeddings | yes | 1.000 | | 0.321 | 0.600 |
| nomic Q4_0 + BM25 hybrid | yes | 0.980 | | 0.404 | 0.560 |
| Model-driven expansion, store vocabulary (vectors) | yes | 0.754 | | 0.247 | 0.440 |
| Same, hybrid | yes | 0.819 | | 0.226 | 0.520 |
| Static nomic token vectors (pooled) | no | 0.403 | | 0.044 | 0.080 |
| Distilled static word vectors | no | 0.352 | | 0.023 | |
| Distilled + All-but-the-Top | no | 0.37 | | 0.010 | |

Blank cells were not recorded in the run. Full outputs: `reports/semantic-2026-11-06.md`.

### 9.3 Reading

- The default trades 0.060 of term MRR (0.920 -> 0.860; term recall@10 stays 1.000) for
  paraphrase recall@10 0.400 -> 0.480 and paraphrase MRR 0.206 -> 0.213 relative to the
  hybrid without a port. It was chosen as the best balance of term MRR and paraphrase recall
  among the configurations that need no model at query time.
- Port v1 measured a higher paraphrase MRR in the same hybrid (0.239 vs 0.213) with the same
  term MRR and paraphrase recall@10; the difference is inside the resolution of this query
  set, and v2 covers far more query words (14 of 15 unknown words vs 2 of 15), which matters
  for queries outside this set.
- Expanding only unknown words keeps term precision exactly but gives no paraphrase gain:
  the expansions are linguistically right, but the answering notes use other terms (a
  question about deleting folders is answered by a note with a shell command).
- Static model vectors without context (pooled tokens, distilled words) are far worse than
  the store's own count model: they encode cross-lingual identity, not topical relatedness.
  Using the model only to pick related store words, inside a bucket, is what works.

## 10. Verification that no model runs at query time

`strace -f -e trace=openat,open,execve,socket,connect build/xksm recall STORE ...`
(strace 7.3), 2026-11-06:

- Files opened: `/etc/ld.so.cache`, libc, libdl, libm, libpthread, libsqlite3,
  `/opt/cuda/lib64/libOpenCL.so.1` (the OpenCL loader, linked for the opt-in GPU SVD; recall
  queries no device), `/dev/urandom`, the `.port`, the store and its `-wal` / `-shm`.
- Attempts (including failed ones) on any `gguf`, `libllama`, `libggml` or `nvidia` path: 0.
- `execve`: 1 (the program itself). `socket` / `connect`: 0.
- `ldd build/xksm`: no llama or ggml library; 0 llama/ggml symbols.

An independent `LD_PRELOAD` open logger gave the same file list.

## 11. Operating it

```
build/xksm init notes.xksm
build/xksm import notes.xksm file.md ...         # explicit files only
build/xksm semantic notes.xksm                   # Stage A, after imports (about 1 s per 3k memories)
build/xksm port notes.xksm /path/xksm-port-nomic-f32-v2.port   # once per store
build/xksm recall notes.xksm how is old data evicted
build/xksm search notes.xksm cache eviction      # BM25 only
```

- Rebuild Stage A after significant imports; the port path is kept.
- The port path is recorded as given: if the file moves, run `xksm port` again.
- `XKSM_EXP_DEBUG=1` prints every expansion (query word -> store word, cosine) to stderr.

## 12. Parameter reference

| Parameter | Default | Where | Meaning |
|---|---|---|---|
| `dim` | 128 | `xksm_semantic_opts` | SVD rank, vector dimension |
| `window` | 5 | same | co-occurrence window (harmonic) |
| `min_count` | 2 | same | minimum word count to enter the model |
| `cds` | 0.75 | same | context distribution smoothing exponent |
| `shift` | 1 (none) | same | PMI shift (log shift subtracted) |
| `sub_t` | 1e-4 | same | subsampling threshold |
| `sif_a` | 1e-3 | same | SIF smoothing constant |
| `power_iters` | 4 | same | rSVD power iterations (oversampling fixed 16) |
| `gpu` | 0 (CPU) | same | 1 require OpenCL GPU, 2 GPU when available, -1 CPU |
| `k` | 5 | `xksm_expand_opts` | related store words per query word |
| `min_cos` | 0.6 | same | minimum port cosine |
| `alpha` | 0.5 | same | weight of a related word relative to a typed word |
| `unknown_only` | 0 | same | 1 = expand only words the store lacks |
| `limit` | 10 | `xksm_hybrid_opts` | fused hits returned |
| `depth` | 50 | same | hits taken from each list |
| `k` (RRF) | 60 | same | RRF constant |
| `w_lexical`, `w_vector` | 1, 1 | same | list weights |
| `match` | ANY | same | lexical match mode |
| threads | 8 (1..128) | `xksm_set_threads` | worker pool size |

## 13. API reference (include/xksm.h)

```c
xksm_status xksm_semantic_build(xksm *db, const xksm_semantic_opts *opts,
                                xksm_semantic_report *rep);
xksm_status xksm_semantic_embed(xksm *db, const char *text, size_t len, float *out, size_t dim);
size_t      xksm_semantic_dim(xksm *db);
xksm_status xksm_semantic_port_load(xksm *db, const char *path);   /* this handle only */
xksm_status xksm_semantic_port_set(xksm *db, const char *path);    /* load + record in store */
xksm_status xksm_semantic_embed_expanded(xksm *db, const char *text, size_t len,
                                         const xksm_expand_opts *opts, float *out, size_t dim,
                                         size_t *expanded);
xksm_status xksm_semantic_recall(xksm *db, const char *text, const xksm_expand_opts *xo,
                                 const xksm_hybrid_opts *ho, xksm_hybrid_result *out);
void        xksm_hybrid_free(xksm_hybrid_result *r);
```

Tests: `test_semantic` (946 checks: topic recovery, persistence, port expansion counts,
recall of a store-unknown word, port survives reopen and rebuild, recall equals hybrid search
hit for hit, lexical fallback, bad port refused), `test_linalg` (29), `test_ocl`,
`test_hybrid`, `test_vecstore`, `test_vector`.

## 14. Limits and open work

- **Query-set resolution**: 25 paraphrase queries; a larger, held-out set is needed before
  any further tuning of k, min_cos or alpha.
- **CJK**: the model vocabulary does not mark word starts for Chinese and Japanese (82 Han,
  19 kana keys); those languages need a word source (a segmenter or a word list).
- **Paraphrase ceiling**: related word forms do not reach notes written in entirely different
  terms; the neural model reaches paraphrase MRR 0.321 and is excluded at query time.
- **Stage A updates**: the model is rebuilt as a whole; incremental fold-in of new memories
  is open work.
- **Accelerators**: Knights Corner and GPU backends of `topk_dot` and of the lattice scan are
  planned (phase 10, phase 11).
- **OpenCL link**: `libxksm` links the OpenCL loader when OpenCL is found at build time; a
  build without it is `-DXKSM_OPENCL=OFF` and loses only the opt-in GPU SVD.

## 15. Decision history

| Date | Decision | Evidence |
|---|---|---|
| 2026-11-06 | No neural embedding model in the engine; a heuristic semantic system written in C, GPU work in OpenCL | D20 |
| 2026-11-06 | nomic-embed-text-v2-moe measured as the reference (Q4_0, then F32) | section 9 |
| 2026-11-06 | Static and distilled model vectors rejected (para MRR 0.044, 0.023) | section 9.3 |
| 2026-11-06 | Model used only offline to build the port; script and code buckets; multilingual and code support | D21 |
| 2026-11-06 | Stage A on the CPU with AVX2/FMA by default; GPU opt-in | D22, section 4.5 |
| 2026-11-06 | Default recall: hybrid + port v2, every content word; never a model at query time | sections 6, 9 |
