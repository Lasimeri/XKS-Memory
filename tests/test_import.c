/* test_import.c: chunking, source dedup, provenance byte ranges, UTF-8 safe splits,
 * all-or-nothing failure, file / directory / missing paths.
 * argv[1] = scratch database path; its directory receives one scratch text file. */
#include "test.h"

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

static int scalar(const char *path, const char *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int v = -1;
    if (sqlite3_open(path, &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

static char *text_of(const char *path, int64_t source_id, int nth)
{
    char sql[256];
    snprintf(sql, sizeof sql,
             "SELECT text FROM memory WHERE source_id = %lld ORDER BY source_off LIMIT 1 OFFSET %d",
             (long long)source_id, nth);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_open(path, &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        out = malloc(strlen(t) + 1);
        if (out)
            strcpy(out, t);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *path = argv[1];
    remove_db(path);
    xksm *x = NULL;
    CHECK(xksm_open(path, &x) == XKSM_OK);
    xksm_import_opts o = {0};
    o.category = XKSM_CAT_PROJECT;
    xksm_import_result r;

    /* Markdown: heading + its paragraphs form one chunk, the next heading starts another */
    const char *md = "# Title\n\npara one line1\r\nline2\n\n## Section\n\nsec text\n";
    CHECK(xksm_import_buffer(x, "notes.md", md, strlen(md), &o, &r) == XKSM_OK);
    CHECK(r.source_id > 0 && r.chunks == 2 && r.added == 2 && r.duplicates == 0);
    xksm_close(x);
    char *t0 = text_of(path, r.source_id, 0), *t1 = text_of(path, r.source_id, 1);
    CHECK(t0 && !strcmp(t0, "# Title\n\npara one line1\nline2"));
    CHECK(t1 && !strcmp(t1, "## Section\n\nsec text"));
    free(t0);
    free(t1);
    CHECK(xksm_open(path, &x) == XKSM_OK);

    /* same bytes as plain text under another name: different format, same SHA-256 */
    o.format = XKSM_FMT_TEXT;
    xksm_id first = r.source_id;
    CHECK(xksm_import_buffer(x, "other.txt", md, strlen(md), &o, &r) == XKSM_EXISTS);
    CHECK(r.source_id == first && r.added == 0);
    /* plain text: headings are not special, so everything packs into one chunk */
    const char *txt = "# not a heading\n\nfirst para\n\nsecond para\n";
    CHECK(xksm_import_buffer(x, "plain.txt", txt, strlen(txt), &o, &r) == XKSM_OK);
    CHECK(r.chunks == 1 && r.added == 1);

    /* long paragraph: pieces of at most 2 x 1024 bytes, cut at whitespace */
    char *big = malloc(5001);
    for (int i = 0; i < 5000; ++i)
        big[i] = (char)(i % 10 == 9 ? ' ' : 'a' + i % 26);
    big[5000] = 0;
    CHECK(xksm_import_buffer(x, "big.txt", big, 5000, &o, &r) == XKSM_OK);
    int64_t big_src = r.source_id;
    CHECK(r.chunks == 3);
    free(big);

    /* no whitespace, 2-byte code points: cuts land on code point boundaries only */
    char *utf = malloc(6001);
    for (int i = 0; i < 6000; i += 2) {
        utf[i] = (char)0xC3;
        utf[i + 1] = (char)0xA9;
    }
    utf[6000] = 0;
    CHECK(xksm_import_buffer(x, "e.txt", utf, 6000, &o, &r) == XKSM_OK);
    CHECK(r.chunks == 3 && r.added == 2 && r.duplicates == 1); /* pieces 1 and 2 are identical */
    free(utf);

    /* invalid UTF-8: refused, nothing written */
    xksm_close(x);
    int sources_before = scalar(path, "SELECT count(*) FROM source");
    CHECK(xksm_open(path, &x) == XKSM_OK);
    CHECK(xksm_import_buffer(x, "bad.txt", "ok\n\xC0\x80\n", 6, &o, &r) == XKSM_EINVAL);
    xksm_close(x);
    CHECK(scalar(path, "SELECT count(*) FROM source") == sources_before);
    CHECK(xksm_open(path, &x) == XKSM_OK);

    /* files: a real file, a directory, a missing path */
    char file[1024], dir[1024];
    snprintf(file, sizeof file, "%s.input.md", path);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(dir, ".");
    FILE *f = fopen(file, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("# From a file\n\nbody text\n", f);
        fclose(f);
    }
    o.format = XKSM_FMT_AUTO;
    CHECK(xksm_import_file(x, file, &o, &r) == XKSM_OK && r.chunks == 1);
    CHECK(xksm_import_file(x, dir, &o, &r) == XKSM_EIO);
    CHECK(xksm_import_file(x, "/nonexistent/xksm-test-file", &o, &r) == XKSM_EIO);
    remove(file);

    xksm_cache_info ci;
    xksm_cache_stats(x, &ci);
    xksm_close(x);

    /* provenance: every chunk's source byte range, normalized, equals its text */
    CHECK(scalar(path,
                 "SELECT count(*) FROM memory m JOIN source s ON m.source_id = s.id WHERE "
                 "trim(replace(CAST(substr(s.body, m.source_off + 1, m.source_len) AS TEXT), "
                 "char(13) || char(10), char(10)), ' ' || char(9) || char(10) || char(13)) "
                 "<> m.text") == 0);
    CHECK(scalar(path, "SELECT count(*) FROM memory WHERE source_id IS NOT NULL") == 9);
    char sql[160];
    snprintf(sql, sizeof sql, "SELECT max(length(CAST(text AS BLOB))) FROM memory WHERE source_id = %lld",
             (long long)big_src);
    CHECK(scalar(path, sql) <= 2048);
    CHECK(scalar(path, "SELECT count(*) FROM source") == 5); /* other.txt existed, bad.txt refused */
    CHECK(ci.records == scalar(path, "SELECT count(*) FROM memory"));

    remove_db(path);
    return test_report("import");
}
