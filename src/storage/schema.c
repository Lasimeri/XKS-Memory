/* schema.c: ordered migrations. Migration i takes user_version i to i + 1.
 * Before the first tagged release migration 0 may still change in place;
 * after it, never edit a released migration; append a new one. */
#include "internal.h"

static const char *const k_migrations[] = {
    /* 0 -> 1: initial schema */
    "CREATE TABLE source ("
    "  id          INTEGER PRIMARY KEY,"
    "  kind        TEXT    NOT NULL,"           /* text, markdown, json, jsonl, csv, ... */
    "  uri         TEXT    NOT NULL,"           /* path or identifier as given by the user */
    "  sha512      BLOB    NOT NULL CHECK (length(sha512) = 64),"
    "  bytes       INTEGER NOT NULL CHECK (bytes >= 0),"
    "  imported_us INTEGER NOT NULL,"           /* unix microseconds, UTC */
    "  body        BLOB"                        /* original evidence; never rewritten */
    ");"
    "CREATE UNIQUE INDEX source_sha ON source(sha512);"

    "CREATE TABLE memory ("
    "  id            INTEGER PRIMARY KEY,"
    "  namespace     TEXT    NOT NULL DEFAULT 'default',"
    "  category      INTEGER NOT NULL CHECK (category BETWEEN 0 AND 5),"
    "  state         INTEGER NOT NULL DEFAULT 1 CHECK (state BETWEEN 0 AND 5),"
    "  source_id     INTEGER REFERENCES source(id) ON DELETE SET NULL,"
    "  source_off    INTEGER,"                  /* byte range of the evidence in source.body */
    "  source_len    INTEGER,"
    "  text          TEXT    NOT NULL,"         /* normalized, derived memory text */
    "  created_us    INTEGER NOT NULL,"
    "  modified_us   INTEGER NOT NULL,"
    "  event_us      INTEGER,"
    "  valid_from_us INTEGER,"
    "  valid_to_us   INTEGER,"
    "  source_conf   REAL    NOT NULL DEFAULT 1.0 CHECK (source_conf BETWEEN 0 AND 1),"
    "  extract_conf  REAL    NOT NULL DEFAULT 1.0 CHECK (extract_conf BETWEEN 0 AND 1),"
    "  embed_model   TEXT,"
    "  embed_version INTEGER,"
    "  tokens        INTEGER,"
    "  importance    REAL    NOT NULL DEFAULT 0.5 CHECK (importance BETWEEN 0 AND 1),"
    "  pinned        INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1)),"
    "  expire_us     INTEGER,"
    "  superseded_by INTEGER REFERENCES memory(id) ON DELETE SET NULL,"
    "  contradiction INTEGER NOT NULL DEFAULT 0,"
    "  checksum      BLOB    NOT NULL CHECK (length(checksum) = 64)"   /* SHA-512 of text */
    ");"
    "CREATE INDEX memory_ns_cat ON memory(namespace, category, state);"
    "CREATE INDEX memory_event  ON memory(event_us);"
    "CREATE INDEX memory_source ON memory(source_id);"

    "CREATE VIRTUAL TABLE memory_fts USING fts5("
    "  text, content='memory', content_rowid='id',"
    "  tokenize=\"unicode61 tokenchars '_'\");"
    "INSERT INTO memory_fts(memory_fts, rank) VALUES('secure-delete', 1);"
    "CREATE TRIGGER memory_ai AFTER INSERT ON memory BEGIN"
    "  INSERT INTO memory_fts(rowid, text) VALUES (new.id, new.text); END;"
    "CREATE TRIGGER memory_ad AFTER DELETE ON memory BEGIN"
    "  INSERT INTO memory_fts(memory_fts, rowid, text) VALUES ('delete', old.id, old.text); END;"
    "CREATE TRIGGER memory_au AFTER UPDATE OF text ON memory BEGIN"
    "  INSERT INTO memory_fts(memory_fts, rowid, text) VALUES ('delete', old.id, old.text);"
    "  INSERT INTO memory_fts(rowid, text) VALUES (new.id, new.text); END;"

    /* Change history: why a memory changed (consolidation, correction, supersession). */
    "CREATE TABLE memory_event_log ("
    "  id        INTEGER PRIMARY KEY,"
    "  memory_id INTEGER NOT NULL,"
    "  at_us     INTEGER NOT NULL,"
    "  action    TEXT    NOT NULL,"
    "  detail    TEXT"
    ");"
    "CREATE INDEX memory_event_log_mem ON memory_event_log(memory_id);",
};

/* Compile-time self-check: one migration per schema version, no more, no fewer. */
_Static_assert(sizeof k_migrations / sizeof k_migrations[0] == XKSM_SCHEMA_VERSION,
               "k_migrations must hold exactly XKSM_SCHEMA_VERSION entries");

static int user_version(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    int v = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

xksm_status xksm__migrate(xksm *x)
{
    int v = user_version(x->db);
    if (v < 0)
        return xksm__fail(x, XKSM_EIO, "cannot read user_version: %s", sqlite3_errmsg(x->db));
    if (v > XKSM_SCHEMA_VERSION)
        return xksm__fail(x, XKSM_ESCHEMA, "store schema %d is newer than library schema %d",
                          v, XKSM_SCHEMA_VERSION);

    for (; v < XKSM_SCHEMA_VERSION; ++v) {
        char pragma[48];
        char *err = NULL;
        snprintf(pragma, sizeof pragma, "PRAGMA user_version = %d;", v + 1);
        if (sqlite3_exec(x->db, "BEGIN IMMEDIATE;", NULL, NULL, &err) != SQLITE_OK ||
            sqlite3_exec(x->db, k_migrations[v], NULL, NULL, &err) != SQLITE_OK ||
            sqlite3_exec(x->db, pragma, NULL, NULL, &err) != SQLITE_OK ||
            sqlite3_exec(x->db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK) {
            xksm_status s = xksm__fail(x, XKSM_EIO, "migration %d -> %d failed: %s", v, v + 1,
                                       err ? err : sqlite3_errmsg(x->db));
            sqlite3_free(err);
            sqlite3_exec(x->db, "ROLLBACK;", NULL, NULL, NULL);
            return s;
        }
    }
    x->schema = v;
    return XKSM_OK;
}
