/* xksm.h: public C ABI of libxksm (XKS-Memory core).
 * Every consumer (CLI, HTTP API, Rust TUI) goes through this header only. */
#ifndef XKSM_H
#define XKSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XKSM_VERSION_MAJOR 0
#define XKSM_VERSION_MINOR 2
#define XKSM_VERSION_PATCH 0

/* Schema version written to PRAGMA user_version by the newest migration. */
#define XKSM_SCHEMA_VERSION 4

typedef struct xksm xksm;

typedef enum xksm_status {
    XKSM_OK = 0,
    XKSM_EINVAL = 1,   /* bad argument */
    XKSM_ENOMEM = 2,   /* allocation failed */
    XKSM_EIO = 3,      /* SQLite or filesystem error; see xksm_errmsg */
    XKSM_ESCHEMA = 4,  /* database schema newer than this library */
    XKSM_ENOTFOUND = 5,
    XKSM_EXISTS = 6,   /* identical memory already stored; its id is returned */
    XKSM_ECORRUPT = 7  /* stored bytes do not hash to their recorded SHA-256 */
} xksm_status;

/* Memory categories (stored as integers; values are part of the on-disk format). */
typedef enum xksm_category {
    XKSM_CAT_WORKING = 0,
    XKSM_CAT_EPISODIC = 1,
    XKSM_CAT_SEMANTIC = 2,
    XKSM_CAT_PROCEDURAL = 3,
    XKSM_CAT_PROJECT = 4,
    XKSM_CAT_ARCHIVAL = 5
} xksm_category;

/* Lifecycle states (on-disk format). */
typedef enum xksm_state {
    XKSM_STATE_CANDIDATE = 0,
    XKSM_STATE_ACTIVE = 1,
    XKSM_STATE_CONSOLIDATED = 2,
    XKSM_STATE_SUPERSEDED = 3,
    XKSM_STATE_ARCHIVED = 4,
    XKSM_STATE_EXPIRED = 5
    /* Deleted memories are removed, not stored with a state. */
} xksm_state;

/* Library version string, e.g. "0.2.0". */
const char *xksm_version(void);

/* Open or create a store at path and migrate it to XKSM_SCHEMA_VERSION.
 * On error *out is still set when allocation succeeded, so xksm_errmsg can be read;
 * the caller must xksm_close it in every case. */
xksm_status xksm_open(const char *path, xksm **out);
void xksm_close(xksm *db);

/* Schema version of the open store (PRAGMA user_version). */
int xksm_schema_version(const xksm *db);

/* Last error message for this handle; never NULL. */
const char *xksm_errmsg(const xksm *db);

/* ---- CPU threads ------------------------------------------------------------------
 * Two worker pools per handle, so a long embedding job never takes the cores recall needs:
 *   recall pool (xksm_set_threads): vector scans, port expansion; default 4
 *   embedding pool (xksm_set_embed_threads): word-vector table build (co-occurrence,
 *     PPMI, SVD, memory vectors); default 4
 * Settable 1..128 (clamped); returns the count applied; a pool is rebuilt at the new size
 * on its next use. xksm_open applies $XKSM_THREADS and $XKSM_EMBED_THREADS when set. */
#define XKSM_THREADS_DEFAULT 4
#define XKSM_EMBED_THREADS_DEFAULT 4
#define XKSM_THREADS_MAX 128
int xksm_set_threads(xksm *db, int threads);
int xksm_get_threads(const xksm *db);
int xksm_set_embed_threads(xksm *db, int threads);
int xksm_get_embed_threads(const xksm *db);

/* ---- memories ---------------------------------------------------------------- */

typedef int64_t xksm_id;            /* row id; 0 = none */
#define XKSM_TIME_NONE INT64_MIN    /* "no timestamp" for optional time fields */

/* Input for xksm_memory_add. Zero-initialise, then set what you need:
 * text/text_len are required; everything else has a default. */
typedef struct xksm_memory_in {
    const char *ns;         /* namespace; NULL = "default" */
    xksm_category category;
    const char *text;       /* UTF-8, need not be NUL-terminated */
    size_t text_len;
    int64_t event_us;       /* when the remembered event happened; 0 = XKSM_TIME_NONE */
    double importance;      /* 0..1; 0 = default 0.5 */
    xksm_id source_id;      /* evidence row in source; 0 = none */
    int64_t source_off;     /* byte range of the evidence in source.body (with source_id) */
    int64_t source_len;
} xksm_memory_in;

/* A stored memory as returned by xksm_memory_get. Strings are owned by the record;
 * release it with xksm_memory_free. */
typedef struct xksm_memory {
    xksm_id id;
    char *ns;
    xksm_category category;
    xksm_state state;
    char *text;             /* normalized text, NUL-terminated */
    size_t text_len;
    int64_t created_us, modified_us, event_us;
    double importance;
    int pinned;
    xksm_id source_id;
    uint8_t checksum[32];   /* SHA-256 of text */
} xksm_memory;

/* Store a memory. Text is validated (UTF-8), normalized (CRLF -> LF, outer whitespace
 * trimmed) and checksummed. If the namespace already holds a memory with the same
 * checksum, nothing is written, *id_out receives the existing id and XKSM_EXISTS is
 * returned. */
xksm_status xksm_memory_add(xksm *db, const xksm_memory_in *in, xksm_id *id_out);

/* Store n memories in one transaction. ids[i] and item_status[i] receive each item's
 * result (XKSM_OK, XKSM_EXISTS, or XKSM_EINVAL for an item that was skipped). Duplicates
 * inside the batch are detected too. Returns XKSM_OK when the transaction committed; on
 * XKSM_EIO or XKSM_ENOMEM nothing from the batch is stored and every ids[i] is 0. */
xksm_status xksm_memory_add_batch(xksm *db, const xksm_memory_in *in, size_t n, xksm_id *ids,
                                  xksm_status *item_status);

/* Load one memory. XKSM_ENOTFOUND if the id does not exist. */
xksm_status xksm_memory_get(xksm *db, xksm_id id, xksm_memory *out);
void xksm_memory_free(xksm_memory *m);

/* Remove a memory and its full-text entry in one transaction (FTS5 secure-delete).
 * The change log records the id and the action only, never the text. */
xksm_status xksm_memory_delete(xksm *db, xksm_id id);

/* ---- explicit import --------------------------------------------------------------
 * Only what the caller names is read: one file or one buffer per call, never a
 * directory walk. The original bytes are stored unchanged in the source table as
 * evidence; each chunk becomes a memory pointing at its byte range. One transaction:
 * either the source and all its chunks are stored, or nothing is.
 * JSON, JSONL and CSV records are stored as text lines "path: value" (JSON paths like
 * a.b[2].c; CSV: header name); strings are unescaped, numbers and booleans kept as
 * written, null and empty values omitted; a record longer than 2 x chunk_target is split
 * like text, every piece pointing at the record's raw bytes. Any syntax error refuses the
 * whole import with its line and column. */

typedef enum xksm_format {
    XKSM_FMT_AUTO = 0,     /* from the extension: .md .markdown .json .jsonl .ndjson .csv;
                            * anything else = text */
    XKSM_FMT_TEXT = 1,
    XKSM_FMT_MARKDOWN = 2, /* headings start a new chunk */
    XKSM_FMT_JSON = 3,     /* top-level array: one memory per element, else one memory */
    XKSM_FMT_JSONL = 4,    /* one memory per non-blank line (one JSON value each) */
    XKSM_FMT_CSV = 5       /* RFC 4180, first row = header; one memory per data row */
} xksm_format;

typedef struct xksm_import_opts {
    const char *ns;          /* namespace; NULL = "default" */
    xksm_category category;  /* category of every chunk */
    xksm_format format;
    size_t chunk_target;     /* bytes; 0 = 1024. Paragraphs are packed up to this size */
    int64_t event_us;        /* when the content happened (every chunk); 0 = none */
    const char *header;      /* NULL, or text put before every chunk's memory text (not into
                              * the stored source), e.g. a time + role line; at most 1 KiB */
} xksm_import_opts;

typedef struct xksm_import_result {
    xksm_id source_id;
    size_t chunks;           /* chunks produced */
    size_t added;            /* new memories */
    size_t duplicates;       /* chunks already stored in the namespace */
} xksm_import_result;

#define XKSM_MAX_SOURCE ((size_t)64 << 20) /* 64 MiB per imported file or buffer */

/* Import one regular file. XKSM_EXISTS: an identical source (same SHA-256) is already
 * stored; result->source_id names it and nothing is written. */
xksm_status xksm_import_file(xksm *db, const char *path, const xksm_import_opts *opts,
                             xksm_import_result *result);

/* Same for bytes already in memory; uri is recorded as the source's name. */
xksm_status xksm_import_buffer(xksm *db, const char *uri, const void *data, size_t len,
                               const xksm_import_opts *opts, xksm_import_result *result);

/* Name and import time of a source. uri receives at most cap - 1 bytes, NUL-terminated. */
xksm_status xksm_source_info(xksm *db, xksm_id source, char *uri, size_t cap,
                             int64_t *imported_us);

/* ---- store key-value state ---------------------------------------------------------
 * Small settings and integration state kept in the store (table kv, schema 4). */
xksm_status xksm_kv_put(xksm *db, const char *key, const void *value, size_t len);
/* XKSM_ENOTFOUND if absent; *len receives the full length (value truncated to cap). */
xksm_status xksm_kv_get(xksm *db, const char *key, void *value, size_t cap, size_t *len);
/* Integer counter: adds delta (absent = 0) and returns the new value in *out. */
xksm_status xksm_kv_add(xksm *db, const char *key, int64_t delta, int64_t *out);
xksm_status xksm_kv_delete(xksm *db, const char *key);

/* ---- agent sessions per harness --------------------------------------------------------
 * Turns of an agent harness (Claude Code, omp, ...) are stored as sources with uri
 * xksm-turn://HARNESS/SESSION/ROLE/TIME_US and memories in the namespaces HARNESS
 * (dialogue) and HARNESS-trace (tool calls, results, thinking), so harnesses never mix.
 * The session table indexes them. */
typedef struct xksm_session_info {
    char *harness, *id, *cwd; /* cwd may be NULL */
    int64_t first_us, last_us;
    int64_t turns;
} xksm_session_info;

/* Record one turn of a session at event_us (creates the row on first use). */
xksm_status xksm_session_touch(xksm *db, const char *harness, const char *id, const char *cwd,
                               int64_t event_us);
/* Sessions of a harness (NULL = every harness), most recent first, at most limit (0 = all). */
xksm_status xksm_session_list(xksm *db, const char *harness, size_t limit,
                              xksm_session_info **out, size_t *count);
void xksm_session_list_free(xksm_session_info *list, size_t count);
/* Delete every memory and source of a session (secure delete) and its row; *deleted (may
 * be NULL) receives the number of memories removed. */
xksm_status xksm_session_forget(xksm *db, const char *harness, const char *id, size_t *deleted);
/* Memory ids of a session in time order (event time, then id); free with free(). */
xksm_status xksm_session_memories(xksm *db, const char *harness, const char *id,
                                  xksm_id **ids, size_t *count);

/* Per namespace: active memories and how many have a vector under model (NULL =
 * XKSM_SEMANTIC_MODEL). */
typedef struct xksm_ns_count {
    char *ns;
    int64_t memories, embedded;
} xksm_ns_count;
xksm_status xksm_ns_counts(xksm *db, const char *model, xksm_ns_count **out, size_t *count);
void xksm_ns_counts_free(xksm_ns_count *list, size_t count);

/* Active memories of a namespace (NULL = all), counted now in the store (other processes'
 * inserts included, unlike the per-handle cache counter); -1 on error. */
int64_t xksm_memory_count(xksm *db, const char *ns);

/* A value that changes whenever the store changed: a commit by this handle or by any other
 * connection or process since the previous call (PRAGMA data_version + this handle's change
 * count); compare successive values, the number itself has no meaning; -1 on error. Costs
 * one prepared statement, so a viewer can poll it several times a second. */
int64_t xksm_change_counter(xksm *db);

/* ---- readable session files ---------------------------------------------------------------
 * ROOT/<harness>/<YYYY-MM-DD_HHMMSS>_<session>/ holds session.md and one file per turn,
 * <YYYY-MM-DD_HH-MM-SS.mmm>_<role>_<sha8>.md, containing the stored turn (header line + text).
 * Local times; files 0600, folders 0700. The index stays authoritative for search; each path
 * written is recorded in the store, so forgetting a session (xksm_session_forget) removes
 * its files and folder. */
/* Write (or rewrite) the file of one agent-turn source; XKSM_EINVAL if it is not a turn. */
xksm_status xksm_mirror_turn(xksm *db, const char *root, xksm_id source);
/* Write the files of every agent turn that still has an active memory. */
xksm_status xksm_mirror_rebuild(xksm *db, const char *root, size_t *written);
/* After deleting a memory: when no active memory of its turn remains, remove the turn file
 * and the turn source. */
xksm_status xksm_mirror_prune(xksm *db, xksm_id source);

/* ---- integrity ---------------------------------------------------------------------------
 * Every memory carries the SHA-256 of its text (xksm_memory.checksum), every source the
 * SHA-256 of its body, every session file is the body of its source. */

/* SHA-256 (SHA-NI when the CPU has it, else the scalar reference). */
void xksm_sha256_digest(const void *data, size_t n, uint8_t out[32]);

/* The recorded SHA-256 and size of a source; XKSM_ENOTFOUND if it does not exist. */
xksm_status xksm_source_sha256(xksm *db, xksm_id source, uint8_t out[32], int64_t *bytes);

/* XKSM_OK when the file's bytes hash to sha, XKSM_ECORRUPT when they do not,
 * XKSM_ENOTFOUND when it cannot be read. */
xksm_status xksm_file_matches(const char *path, const uint8_t sha[32]);

typedef struct xksm_verify_report {
    size_t memories, memory_bad;             /* checksum != SHA-256(text) */
    size_t sources, source_bad;              /* sha256 != SHA-256(body) or bytes != length */
    size_t sources_without_body;             /* nothing stored to recompute from */
    size_t files, file_bad, file_missing;    /* session files: content or name differ, unreadable */
    xksm_id first_bad_memory, first_bad_source, first_bad_file_source; /* 0 = none */
    double seconds;
} xksm_verify_report;

/* Recompute every hash of the store in one read snapshot (writers keep running): memories,
 * sources, and the session files under root (NULL = the root recorded by the mirror, none
 * when nothing was mirrored). XKSM_OK means the check ran; the counts say what it found. */
xksm_status xksm_verify(xksm *db, const char *root, xksm_verify_report *out);

/* Active memory ids of a namespace (NULL = all), newest first (event time, else creation),
 * limit (0 = all) from offset; free with free(). */
xksm_status xksm_memory_ids(xksm *db, const char *ns, size_t offset, size_t limit, xksm_id **ids,
                            size_t *count);

/* ---- lexical retrieval (FTS5, BM25) -------------------------------------------------
 * Only active memories are searched (superseded, expired, archived and candidate ones are
 * never returned as current). User text is never interpreted as FTS5 syntax except in
 * XKSM_MATCH_RAW: each whitespace-separated term is quoted; a trailing '*' on a term makes
 * it a prefix search. Hits are ordered by BM25, ties by id, so results are deterministic. */

typedef enum xksm_match {
    XKSM_MATCH_ANY = 0, /* any term (OR), ranked by BM25: default */
    XKSM_MATCH_ALL = 1, /* every term (AND) */
    XKSM_MATCH_RAW = 2  /* the query is FTS5 syntax as written; errors -> XKSM_EINVAL */
} xksm_match;

typedef struct xksm_search_opts {
    const char *ns;          /* only this namespace; NULL = all */
    uint32_t categories;     /* bit (1 << category); 0 = all */
    xksm_match match;
    size_t limit;            /* 0 = 10 */
} xksm_search_opts;

typedef struct xksm_search_hit {
    xksm_id id;
    double score;            /* -bm25: higher is better */
    char *snippet;           /* matched terms in [brackets]; owned by the result */
} xksm_search_hit;

typedef struct xksm_search_result {
    xksm_search_hit *hits;
    size_t count;
} xksm_search_result;

#define XKSM_MAX_QUERY 4096  /* bytes */
#define XKSM_MAX_TERMS 64

xksm_status xksm_search(xksm *db, const char *query, const xksm_search_opts *opts,
                        xksm_search_result *out);
void xksm_search_free(xksm_search_result *r);

/* ---- vectors ------------------------------------------------------------------------
 * Embeddings are produced by whatever the caller uses (a local model); the store keeps
 * one vector per (memory, model), unit-normalized, as f32 plus a square-lattice int8 code.
 * Search scans the codes on the worker pool, rescoring the best candidates exactly. */

typedef struct xksm_vector_opts {
    const char *ns;          /* only this namespace; NULL = all */
    uint32_t categories;     /* bit (1 << category); 0 = all */
    size_t limit;            /* 0 = 10 */
    size_t rerank;           /* lattice candidates rescored exactly; 0 = 100 */
} xksm_vector_opts;

/* Store or replace the vector of a memory under a model name. XKSM_ENOTFOUND if the memory
 * does not exist; XKSM_EINVAL if dim differs from the model's existing vectors. */
xksm_status xksm_vector_put(xksm *db, xksm_id memory, const char *model, const float *v,
                            size_t dim);

/* Nearest active memories by cosine similarity (score in -1..1, exact). Hits carry no
 * snippet. A model with no vectors gives zero hits. */
xksm_status xksm_vector_search(xksm *db, const char *model, const float *q, size_t dim,
                               const xksm_vector_opts *opts, xksm_search_result *out);

/* ---- semantic model (no neural network) ---------------------------------------------
 * Built from the store's own active memories: co-occurrence (harmonic window, frequency
 * subsampling) -> PPMI with context distribution smoothing -> randomized truncated SVD ->
 * word vectors; a memory's vector is the SIF-weighted average of its word vectors with
 * the common component removed (Levy et al. 2015, Arora et al. 2017, Halko et al. 2011).
 * Vectors are stored under the model name XKSM_SEMANTIC_MODEL for xksm_vector_search and
 * xksm_hybrid_search; xksm_semantic_embed turns query text into the same space. */
#define XKSM_SEMANTIC_MODEL "xksm-semantic"

typedef struct xksm_semantic_opts {
    size_t dim;            /* 0 = 128 */
    size_t window;         /* 0 = 5 */
    uint32_t min_count;    /* 0 = 2 */
    double cds;            /* context distribution smoothing exponent; 0 = 0.75 */
    double shift;          /* PPMI shift k (PMI - log k); 0 = 1, no shift */
    double sub_t;          /* subsampling threshold; 0 = 1e-4 */
    double sif_a;          /* SIF weight a / (a + p(w)); 0 = 1e-3 */
    int power_iters;       /* 0 = 4 */
    int gpu;               /* 0 = CPU (default: AVX2/FMA when present), 1 = require an OpenCL
                            * GPU, 2 = OpenCL GPU when available, else CPU; -1 = CPU */
} xksm_semantic_opts;

typedef struct xksm_semantic_report {
    size_t memories, embedded, tokens, vocab;
    uint64_t pairs, nnz;
    double t_tokens, t_cooc, t_ppmi, t_svd, t_embed, t_store, t_total; /* seconds */
    int gpu_used;          /* 1 if the SVD ran on the GPU */
    char device[64];       /* GPU name when gpu_used */
} xksm_semantic_report;

xksm_status xksm_semantic_build(xksm *db, const xksm_semantic_opts *opts,
                                xksm_semantic_report *report);

/* Embed text with the stored model into out[dim]. XKSM_ENOTFOUND if no model is built;
 * XKSM_EINVAL if dim differs or the text has no known word. */
xksm_status xksm_semantic_embed(xksm *db, const char *text, size_t len, float *out, size_t dim);

/* Embed every active memory that has no XKSM_SEMANTIC_MODEL vector yet with the current
 * model (fold-in of new memories; words new to the model carry no signal until the next
 * build). *embedded (may be NULL) receives the count. XKSM_ENOTFOUND if no model. */
xksm_status xksm_semantic_update(xksm *db, size_t *embedded);

/* Number of memories the current model was built from; 0 if none. */
size_t xksm_semantic_built_memories(xksm *db);

/* Model dimension, 0 if no model is built. */
size_t xksm_semantic_dim(xksm *db);

/* ---- semantic port: model-made word relations, used without the model ----------------
 * A port file (tools/port_build) holds a static vector per word, computed once by an
 * embedding model, each tagged with a bucket (Unicode script, or code). Query expansion
 * relates a query word only to store words of the same bucket, and adds them with weight
 * alpha * cosine on the semantic side; typed words always count fully. */
typedef struct xksm_expand_opts {
    size_t k;          /* related words per query word; 0 = 5 */
    float min_cos;     /* minimum cosine; 0 = 0.6 */
    float alpha;       /* weight of a related word relative to a typed word; 0 = 0.5 */
    int unknown_only;  /* 0 (default): expand every content word; 1: only words the store
                        * does not contain (term precision kept, paraphrase recall lower) */
} xksm_expand_opts;

/* Load a port for this handle only. */
xksm_status xksm_semantic_port_load(xksm *db, const char *path);

/* Load a port and record its path in the store, so every later handle loads it on first
 * use (survives xksm_semantic_build). */
xksm_status xksm_semantic_port_set(xksm *db, const char *path);

/* The port path recorded in the store, NUL-terminated into buf (at most cap - 1 bytes);
 * returns its full length, 0 if none is recorded. */
size_t xksm_semantic_port_path(xksm *db, char *buf, size_t cap);

/* As xksm_semantic_embed, plus expansion through the loaded port. *expanded (may be NULL)
 * receives the number of related words added. */
xksm_status xksm_semantic_embed_expanded(xksm *db, const char *text, size_t len,
                                         const xksm_expand_opts *opts, float *out, size_t dim,
                                         size_t *expanded);


/* ---- hybrid retrieval: reciprocal rank fusion ----------------------------------------
 * Lexical and vector results fused by RRF: score = sum over lists of w / (k + rank),
 * rank counted from 1 (Cormack, Clarke, Buettcher 2009; k = 60 by default). Each hit
 * keeps its rank and score in each list, which is why it was retrieved. */

typedef struct xksm_hybrid_opts {
    const char *ns;          /* only this namespace; NULL = all */
    uint32_t categories;     /* bit (1 << category); 0 = all */
    xksm_match match;        /* lexical match mode */
    size_t limit;            /* fused hits; 0 = 10 */
    size_t depth;            /* hits taken from each list; 0 = 50 */
    double k;                /* RRF constant; 0 = 60 */
    double w_lexical, w_vector; /* list weights; both 0 = 1 and 1 */
} xksm_hybrid_opts;

typedef struct xksm_hybrid_hit {
    xksm_id id;
    double score;            /* fused */
    size_t lexical_rank;     /* 1-based; 0 = not in the lexical list */
    size_t vector_rank;      /* 1-based; 0 = not in the vector list */
    double lexical_score;    /* -bm25 when lexical_rank > 0 */
    double vector_score;     /* cosine when vector_rank > 0 */
    char *snippet;           /* from the lexical list, or NULL */
} xksm_hybrid_hit;

typedef struct xksm_hybrid_result {
    xksm_hybrid_hit *hits;
    size_t count;
} xksm_hybrid_result;

/* text or (model, q, dim) may be absent (NULL / 0), or its weight 0: the other list alone
 * is ranked. */
xksm_status xksm_hybrid_search(xksm *db, const char *text, const char *model, const float *q,
                               size_t dim, const xksm_hybrid_opts *opts,
                               xksm_hybrid_result *out);
void xksm_hybrid_free(xksm_hybrid_result *r);

/* Recall: the default retrieval setup (project decision 2026-11-06). Hybrid search: BM25
 * over the query as typed, fused by RRF with the lattice vector scan over
 * XKSM_SEMANTIC_MODEL, whose query vector expands every content word through the store's
 * semantic port (xksm_semantic_port_set; xo->unknown_only narrows it; without a port the
 * query vector is the plain model embedding). text is
 * NUL-terminated; xo, ho may be NULL (defaults). Free out with xksm_hybrid_free. */
xksm_status xksm_semantic_recall(xksm *db, const char *text, const xksm_expand_opts *xo,
                                 const xksm_hybrid_opts *ho, xksm_hybrid_result *out);

/* ---- Tier-1 RAM record cache ------------------------------------------------------
 * Holds a percentage of the store's records, capacity = ceil(records * percent / 100),
 * percent 5..50 (default 25); filled on read, CLOCK eviction, evicted on delete. */
#define XKSM_CACHE_PCT_MIN 5
#define XKSM_CACHE_PCT_MAX 50
#define XKSM_CACHE_PCT_DEFAULT 25

typedef struct xksm_cache_info {
    int64_t records;     /* memories in the store */
    uint32_t capacity;   /* ceil(records * percent / 100) */
    uint32_t resident;   /* records currently cached */
    uint64_t hits, misses, evictions;
    size_t bytes;        /* RAM held by cached records */
    size_t byte_limit;   /* 0 = no ceiling (default) */
    int percent;         /* 5..50 */
} xksm_cache_info;

void xksm_cache_stats(const xksm *db, xksm_cache_info *out);

/* Cache size as a percentage of the records, clamped to 5..50. Returns the percentage
 * applied. Shrinking evicts immediately; growing lets later reads fill the cache. */
int xksm_cache_set_percent(xksm *db, int percent);

/* Optional RAM ceiling for the cache in bytes; 0 (the default) = none. With a ceiling the
 * cache holds min(percent of the records, what fits in the ceiling). */
void xksm_cache_set_byte_limit(xksm *db, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif /* XKSM_H */
