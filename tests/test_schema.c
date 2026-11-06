/* test_schema.c: fresh create, idempotent reopen, refusal of a newer schema,
 * constraint enforcement, FTS trigger sync. argv[1] = scratch database path. */
#include "test.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static void remove_db(const char *p)
{
    char buf[1024];
    remove(p);
    snprintf(buf, sizeof buf, "%s-wal", p);
    remove(buf);
    snprintf(buf, sizeof buf, "%s-shm", p);
    remove(buf);
}

static int exec_ok(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static int count(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s scratch.xksm\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    remove_db(path);

    xksm *x = NULL;
    CHECK(xksm_open(path, &x) == XKSM_OK);
    CHECK(xksm_schema_version(x) == XKSM_SCHEMA_VERSION);
    xksm_close(x);

    /* reopen: no migration re-run, same version */
    CHECK(xksm_open(path, &x) == XKSM_OK);
    CHECK(xksm_schema_version(x) == XKSM_SCHEMA_VERSION);
    xksm_close(x);

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(count(db, "SELECT count(*) FROM sqlite_schema WHERE name IN "
                    "('source','memory','memory_fts','memory_event_log')") == 4);

    /* constraints: bad category and short checksum are rejected */
    CHECK(!exec_ok(db, "INSERT INTO memory(category,text,created_us,modified_us,checksum) "
                       "VALUES (9,'x',0,0,zeroblob(32))"));
    CHECK(!exec_ok(db, "INSERT INTO memory(category,text,created_us,modified_us,checksum) "
                       "VALUES (2,'x',0,0,zeroblob(31))"));

    /* FTS sync through triggers: insert, update, delete */
    CHECK(exec_ok(db, "INSERT INTO memory(category,text,created_us,modified_us,checksum) "
                      "VALUES (2,'compile with sm_86',0,0,zeroblob(32))"));
    CHECK(count(db, "SELECT count(*) FROM memory_fts WHERE memory_fts MATCH 'sm_86'") == 1);
    CHECK(exec_ok(db, "UPDATE memory SET text='compile with sm_89' WHERE id=1"));
    CHECK(count(db, "SELECT count(*) FROM memory_fts WHERE memory_fts MATCH 'sm_86'") == 0);
    CHECK(count(db, "SELECT count(*) FROM memory_fts WHERE memory_fts MATCH 'sm_89'") == 1);
    CHECK(exec_ok(db, "DELETE FROM memory WHERE id=1"));
    CHECK(count(db, "SELECT count(*) FROM memory_fts WHERE memory_fts MATCH 'sm_89'") == 0);
    CHECK(exec_ok(db, "INSERT INTO memory_fts(memory_fts) VALUES('integrity-check')"));

    /* an older store is migrated forward: simulate v1 (before memory_vec and the semantic
     * tables) and reopen; every later migration must replay cleanly */
    CHECK(exec_ok(db, "DROP TABLE memory_vec; DROP TABLE semantic_vocab; DROP TABLE semantic_meta"));
    CHECK(exec_ok(db, "PRAGMA user_version = 1"));
    CHECK(exec_ok(db, "INSERT INTO memory(category,text,created_us,modified_us,checksum) "
                      "VALUES (2,'kept across migration',0,0,zeroblob(32))"));
    sqlite3_close(db);
    CHECK(xksm_open(path, &x) == XKSM_OK);
    CHECK(xksm_schema_version(x) == XKSM_SCHEMA_VERSION);
    xksm_close(x);
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(count(db, "SELECT count(*) FROM sqlite_schema WHERE name IN ('memory_vec', "
                    "'semantic_vocab', 'semantic_meta')") == 3);
    CHECK(count(db, "SELECT count(*) FROM memory WHERE text = 'kept across migration'") == 1);

    /* a store from the future is refused, not downgraded */
    CHECK(exec_ok(db, "PRAGMA user_version = 99"));
    sqlite3_close(db);
    CHECK(xksm_open(path, &x) == XKSM_ESCHEMA);
    CHECK(strstr(xksm_errmsg(x), "newer") != NULL);
    xksm_close(x);

    remove_db(path);
    return test_report("schema");
}
