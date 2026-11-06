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
#define XKSM_VERSION_MINOR 1
#define XKSM_VERSION_PATCH 0

/* Schema version written to PRAGMA user_version by the newest migration. */
#define XKSM_SCHEMA_VERSION 1

typedef struct xksm xksm;

typedef enum xksm_status {
    XKSM_OK = 0,
    XKSM_EINVAL = 1,   /* bad argument */
    XKSM_ENOMEM = 2,   /* allocation failed */
    XKSM_EIO = 3,      /* SQLite or filesystem error; see xksm_errmsg */
    XKSM_ESCHEMA = 4,  /* database schema newer than this library */
    XKSM_ENOTFOUND = 5,
    XKSM_EXISTS = 6    /* identical memory already stored; its id is returned */
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

/* Library version string, e.g. "0.1.0". */
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
 * Worker threads for parallel scans: default 8, settable 1..128 (clamped). Returns the
 * count applied; the pool is rebuilt at the new size on its next use. */
#define XKSM_THREADS_DEFAULT 8
#define XKSM_THREADS_MAX 128
int xksm_set_threads(xksm *db, int threads);
int xksm_get_threads(const xksm *db);

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
 * either the source and all its chunks are stored, or nothing is. */

typedef enum xksm_format {
    XKSM_FMT_AUTO = 0,     /* from the file extension; unknown extensions = text */
    XKSM_FMT_TEXT = 1,
    XKSM_FMT_MARKDOWN = 2  /* headings start a new chunk */
} xksm_format;

typedef struct xksm_import_opts {
    const char *ns;          /* namespace; NULL = "default" */
    xksm_category category;  /* category of every chunk */
    xksm_format format;
    size_t chunk_target;     /* bytes; 0 = 1024. Paragraphs are packed up to this size */
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
