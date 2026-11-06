/* test_memory.c: add / get / dedup / validation / persistence / delete, and a byte scan
 * of the database file proving a deleted memory's text is gone from it.
 * argv[1] = scratch database path. */
#include "test.h"

#include "sha256.h"

#include <sqlite3.h>
#include <stdlib.h>
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

static xksm_status add(xksm *x, const char *ns, const char *text, xksm_id *id)
{
    xksm_memory_in in = {0};
    in.ns = ns;
    in.category = XKSM_CAT_SEMANTIC;
    in.text = text;
    in.text_len = strlen(text);
    return xksm_memory_add(x, &in, id);
}

/* occurrences of needle in the file at path */
static long count_in_file(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    long hits = 0;
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
        size_t k = strlen(needle);
        for (long i = 0; i + (long)k <= n; ++i)
            hits += memcmp(b + i, needle, k) == 0;
    }
    free(b);
    fclose(f);
    return hits;
}

static int scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
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

    /* add + normalization + get */
    xksm_id a = 0, b = 0, c = 0;
    CHECK(add(x, NULL, "  \t build flags: sm_86\r\nuse -O3  \n", &a) == XKSM_OK);
    CHECK(a > 0);
    xksm_memory m;
    CHECK(xksm_memory_get(x, a, &m) == XKSM_OK);
    CHECK(m.text && !strcmp(m.text, "build flags: sm_86\nuse -O3"));
    CHECK(m.text_len == strlen("build flags: sm_86\nuse -O3"));
    CHECK(m.ns && !strcmp(m.ns, "default"));
    CHECK(m.category == XKSM_CAT_SEMANTIC && m.state == XKSM_STATE_ACTIVE);
    CHECK(m.importance == 0.5 && m.event_us == XKSM_TIME_NONE && m.pinned == 0);
    CHECK(m.created_us > 1700000000000000 && m.created_us == m.modified_us);
    uint8_t want[32];
    xksm__sha256(m.text, m.text_len, want);
    CHECK(!memcmp(m.checksum, want, 32));
    xksm_memory_free(&m);

    /* dedup within a namespace, not across namespaces */
    CHECK(add(x, "default", "build flags: sm_86\r\nuse -O3", &b) == XKSM_EXISTS);
    CHECK(b == a);
    CHECK(add(x, "project-a", "build flags: sm_86\nuse -O3", &c) == XKSM_OK);
    CHECK(c > 0 && c != a);

    /* validation */
    xksm_id z;
    const char *bad_utf8[] = {"\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82",
                              "\x80", "ok \xFF"};
    for (size_t i = 0; i < sizeof bad_utf8 / sizeof *bad_utf8; ++i)
        CHECK(add(x, NULL, bad_utf8[i], &z) == XKSM_EINVAL);
    CHECK(add(x, NULL, "euro \xE2\x82\xAC clef \xF0\x9D\x84\x9E e \xC3\xA9", &z) == XKSM_OK);
    CHECK(add(x, NULL, " \r\n\t ", &z) == XKSM_EINVAL);
    xksm_memory_in in = {0};
    in.text = "x";
    in.text_len = 1;
    in.category = (xksm_category)7;
    CHECK(xksm_memory_add(x, &in, &z) == XKSM_EINVAL);
    in.category = XKSM_CAT_EPISODIC;
    in.importance = 1.5;
    CHECK(xksm_memory_add(x, &in, &z) == XKSM_EINVAL);

    /* persistence across close / reopen */
    const char *marker = "zqxmarker7731 unique words";
    xksm_id d = 0;
    CHECK(add(x, "project-a", marker, &d) == XKSM_OK);
    xksm_close(x);
    CHECK(xksm_open(path, &x) == XKSM_OK);
    CHECK(xksm_memory_get(x, d, &m) == XKSM_OK);
    CHECK(m.text && !strcmp(m.text, marker));
    xksm_memory_free(&m);
    /* precondition: the close checkpointed the marker into the main file, so the zero
     * count after deletion below means it was erased, not that it was never written */
    long before = count_in_file(path, "zqxmarker7731");
    printf("marker occurrences before delete: db file %ld\n", before);
    CHECK(before > 0);

    /* delete: gone from table, index, and from the file bytes */
    CHECK(xksm_memory_delete(x, d) == XKSM_OK);
    CHECK(xksm_memory_get(x, d, &m) == XKSM_ENOTFOUND);
    CHECK(xksm_memory_delete(x, d) == XKSM_ENOTFOUND);
    xksm_close(x);

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(scalar(db, "SELECT count(*) FROM memory_fts WHERE memory_fts MATCH 'zqxmarker7731'") == 0);
    CHECK(scalar(db, "SELECT count(*) FROM memory_event_log WHERE action='delete'") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM memory_event_log WHERE detail IS NOT NULL") == 0);
    CHECK(sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    char wal[1024];
    snprintf(wal, sizeof wal, "%s-wal", path);
    long in_db = count_in_file(path, "zqxmarker7731");
    long in_wal = count_in_file(wal, "zqxmarker7731");
    printf("deleted marker occurrences: db file %ld, wal %ld\n", in_db, in_wal);
    CHECK(in_db == 0);
    CHECK(in_wal == 0);
    /* control: a live memory's text IS found by the same scan */
    CHECK(count_in_file(path, "build flags: sm_86") > 0);

    remove_db(path);
    return test_report("memory");
}
