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

/* Load one memory. XKSM_ENOTFOUND if the id does not exist. */
xksm_status xksm_memory_get(xksm *db, xksm_id id, xksm_memory *out);
void xksm_memory_free(xksm_memory *m);

/* Remove a memory and its full-text entry in one transaction (FTS5 secure-delete).
 * The change log records the id and the action only, never the text. */
xksm_status xksm_memory_delete(xksm *db, xksm_id id);

#ifdef __cplusplus
}
#endif
#endif /* XKSM_H */
