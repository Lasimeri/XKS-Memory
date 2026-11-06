/* test_structured.c: JSON, JSONL and CSV import: rendering ("path: value"), unescaping
 * (BMP and surrogate pairs), provenance byte ranges, extension detection, strict syntax
 * errors with line and column, all-or-nothing failure, long-record splitting, dedup, and
 * random mutations of valid inputs (never a crash, never a partial import).
 * argv[1] = scratch database path. */
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

/* memories of source sid, in id order: text, and the source bytes of its provenance */
typedef struct row {
    char text[2048];
    char raw[2048];
} row;

static int rows_of(const char *db_path, long long sid, row *out, int max)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_open(db_path, &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
                           "SELECT m.text, substr(s.body, m.source_off + 1, m.source_len) "
                           "FROM memory m JOIN source s ON s.id = m.source_id "
                           "WHERE m.source_id = ?1 ORDER BY m.id",
                           -1, &st, NULL) == SQLITE_OK &&
        sqlite3_bind_int64(st, 1, sid) == SQLITE_OK)
        while (n < max && sqlite3_step(st) == SQLITE_ROW) {
            snprintf(out[n].text, sizeof out[n].text, "%s", (const char *)sqlite3_column_text(st, 0));
            int rl = sqlite3_column_bytes(st, 1);
            const void *rb = sqlite3_column_blob(st, 1);
            if (rl >= (int)sizeof out[n].raw)
                rl = (int)sizeof out[n].raw - 1;
            memcpy(out[n].raw, rb ? rb : "", (size_t)rl);
            out[n].raw[rl] = 0;
            ++n;
        }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

static long long count(const char *db_path, const char *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    long long v = -1;
    if (sqlite3_open(db_path, &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

static xksm_status imp(xksm *x, const char *uri, const char *s, xksm_import_result *r)
{
    xksm_import_opts o = {0};
    o.category = XKSM_CAT_PROJECT;
    return xksm_import_buffer(x, uri, s, strlen(s), &o, r);
}

static uint64_t rng = 0x2545F4914F6CDD1Du;
static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *P = argv[1];
    remove_db(P);
    xksm *x = NULL;
    CHECK(xksm_open(P, &x) == XKSM_OK);
    xksm_import_result r;
    row R[16];

    /* JSON: top-level array, one memory per element; nested paths; escapes; nulls, empty
     * strings and empty containers omitted; provenance = the element's raw bytes */
    const char *j1 = "[\n"
                     "  {\"name\": \"alpha\", \"n\": -1.5e3, \"ok\": true,\n"
                     "   \"tags\": [\"x\", \"y\"], \"sub\": {\"k\": \"caf\\u00e9 \\\"q\\\"\\n2\"}},\n"
                     "  {\"name\": \"beta\", \"gone\": null, \"blank\": \"\", \"e\": {}, \"f\": []},\n"
                     "  {\"only\": null},\n"
                     "  \"plain \\ud800\\udf48 text\"\n"
                     "]\n";
    CHECK(imp(x, "a.json", j1, &r) == XKSM_OK);
    CHECK(r.chunks == 3 && r.added == 3);
    int n = rows_of(P, r.source_id, R, 16);
    CHECK(n == 3);
    CHECK(!strcmp(R[0].text, "name: alpha\nn: -1.5e3\nok: true\ntags[0]: x\ntags[1]: y\n"
                             "sub.k: caf\xC3\xA9 \"q\"\n2"));
    CHECK(!strncmp(R[0].raw, "{\"name\": \"alpha\"", 16) && R[0].raw[strlen(R[0].raw) - 1] == '}');
    CHECK(!strcmp(R[1].text, "name: beta"));
    CHECK(!strcmp(R[1].raw, "{\"name\": \"beta\", \"gone\": null, \"blank\": \"\", \"e\": {}, \"f\": []}"));
    CHECK(!strcmp(R[2].text, "plain \xF0\x90\x8D\x88 text")); /* U+10348 from a surrogate pair */
    CHECK(!strcmp(R[2].raw, "\"plain \\ud800\\udf48 text\""));
    CHECK(count(P, "SELECT count(*) FROM source WHERE kind = 'json'") == 1);

    /* JSON: a single top-level object is one memory; a UTF-8 BOM is skipped */
    CHECK(imp(x, "b.json", "\xEF\xBB\xBF{\"a\": {\"b\": [1, {\"c\": false}]}}", &r) == XKSM_OK);
    CHECK(rows_of(P, r.source_id, R, 16) == 1);
    CHECK(!strcmp(R[0].text, "a.b[0]: 1\na.b[1].c: false"));
    CHECK(!strcmp(R[0].raw, "{\"a\": {\"b\": [1, {\"c\": false}]}}"));

    /* JSON syntax errors: refused, with line and column; nothing written */
    long long mem0 = count(P, "SELECT count(*) FROM memory"), src0 = count(P, "SELECT count(*) FROM source");
    static const char *const bad[] = {
        "[1, 2,]",                 /* trailing comma */
        "{\"a\": 1} x",            /* data after the value */
        "{\"a\" 1}",               /* missing colon */
        "[\"\\ud800\"]",           /* lone high surrogate */
        "[\"\\udc00\"]",           /* lone low surrogate */
        "[\"a\\x\"]",              /* unknown escape */
        "[01]",                    /* leading zero */
        "[1.]",                    /* empty fraction */
        "[\"tab\there\"]",         /* raw control character */
        "{\"a\": tru}",            /* bad literal */
        "",                        /* empty document */
        "[\"unterminated]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i)
        CHECK(imp(x, "bad.json", bad[i], &r) == XKSM_EINVAL);
    {
        char deep[700];
        size_t k = 0;
        for (int d = 0; d < 300; ++d)
            deep[k++] = '[';
        deep[k++] = '1';
        for (int d = 0; d < 300; ++d)
            deep[k++] = ']';
        deep[k] = 0;
        CHECK(imp(x, "deep.json", deep, &r) == XKSM_EINVAL);
        CHECK(strstr(xksm_errmsg(x), "nesting") != NULL);
        k = 0; /* depth 256 is accepted */
        for (int d = 0; d < 256; ++d)
            deep[k++] = '[';
        deep[k++] = '7';
        for (int d = 0; d < 256; ++d)
            deep[k++] = ']';
        deep[k] = 0;
        CHECK(imp(x, "deep256.json", deep, &r) == XKSM_OK && r.added == 1);
        mem0 += 1;
        src0 += 1;
    }
    CHECK(imp(x, "pos.json", "[1,\n 2,\n   x]", &r) == XKSM_EINVAL);
    CHECK(strstr(xksm_errmsg(x), "line 3, column 4") != NULL);
    printf("error message: %s\n", xksm_errmsg(x));
    CHECK(count(P, "SELECT count(*) FROM memory") == mem0);
    CHECK(count(P, "SELECT count(*) FROM source") == src0);

    /* JSONL: one memory per non-blank line, CRLF tolerated; provenance = the line */
    const char *l1 = "{\"role\": \"one\", \"v\": 1}\r\n\n   \n[\"two\", 2]\n\"three\"\n{\"z\": null}\n";
    CHECK(imp(x, "c.jsonl", l1, &r) == XKSM_OK);
    CHECK(r.chunks == 3);
    n = rows_of(P, r.source_id, R, 16);
    CHECK(n == 3);
    CHECK(!strcmp(R[0].text, "role: one\nv: 1") && !strcmp(R[0].raw, "{\"role\": \"one\", \"v\": 1}"));
    CHECK(!strcmp(R[1].text, "[0]: two\n[1]: 2") && !strcmp(R[1].raw, "[\"two\", 2]"));
    CHECK(!strcmp(R[2].text, "three"));
    CHECK(count(P, "SELECT count(*) FROM source WHERE kind = 'jsonl'") == 1);
    mem0 = count(P, "SELECT count(*) FROM memory");
    CHECK(imp(x, "d.ndjson", "{\"a\": 1}\n{\"b\": 2}\n{\"c\": }\n", &r) == XKSM_EINVAL);
    CHECK(strstr(xksm_errmsg(x), "line 3") != NULL);
    CHECK(imp(x, "e.jsonl", "{\"a\": 1} {\"b\": 2}\n", &r) == XKSM_EINVAL); /* two values on a line */
    CHECK(count(P, "SELECT count(*) FROM memory") == mem0);

    /* CSV: header names, quoted fields with commas, "" and newlines, CRLF, empty values
     * omitted, empty header name -> "column N"; provenance = the row */
    const char *c1 = "id,note,,extra\r\n"
                     "1,\"comma, inside\",v3,\r\n"
                     "2,\"say \"\"hi\"\"\nnext line\",,x\n"
                     "\n"
                     "3,,,\n";
    CHECK(imp(x, "f.csv", c1, &r) == XKSM_OK);
    n = rows_of(P, r.source_id, R, 16);
    CHECK(n == 3 && r.chunks == 3);
    CHECK(!strcmp(R[0].text, "id: 1\nnote: comma, inside\ncolumn 3: v3"));
    CHECK(!strcmp(R[0].raw, "1,\"comma, inside\",v3,"));
    CHECK(!strcmp(R[1].text, "id: 2\nnote: say \"hi\"\nnext line\nextra: x"));
    CHECK(!strcmp(R[2].text, "id: 3"));
    mem0 = count(P, "SELECT count(*) FROM memory");
    CHECK(imp(x, "g.csv", "a,b\n1,2\n3\n", &r) == XKSM_EINVAL); /* ragged row */
    CHECK(strstr(xksm_errmsg(x), "line 3") != NULL);
    CHECK(imp(x, "h.csv", "a,b\n1,\"open\n", &r) == XKSM_EINVAL);
    CHECK(imp(x, "i.csv", "a,b\n1,x\"y\n", &r) == XKSM_EINVAL);
    CHECK(imp(x, "j.csv", "a,b\n1,\"q\"z\n", &r) == XKSM_EINVAL);
    CHECK(count(P, "SELECT count(*) FROM memory") == mem0);
    CHECK(imp(x, "k.csv", "only,header\n", &r) == XKSM_OK && r.chunks == 0);

    /* explicit format overrides the extension; unknown extension = text */
    {
        xksm_import_opts o = {0};
        o.category = XKSM_CAT_PROJECT;
        o.format = XKSM_FMT_JSONL;
        const char *s = "{\"q\": \"forced\"}\n";
        CHECK(xksm_import_buffer(x, "noext", s, strlen(s), &o, &r) == XKSM_OK);
        CHECK(rows_of(P, r.source_id, R, 16) == 1 && !strcmp(R[0].text, "q: forced"));
        o.format = (xksm_format)9;
        CHECK(xksm_import_buffer(x, "noext2", s, strlen(s), &o, &r) == XKSM_EINVAL);
        CHECK(imp(x, "l.txt", "[\"not parsed\"]", &r) == XKSM_OK);
        CHECK(rows_of(P, r.source_id, R, 16) == 1 && !strcmp(R[0].text, "[\"not parsed\"]"));
    }

    /* a record longer than 2 x chunk_target is split; every piece points at the record */
    {
        static char big[8192];
        size_t k = (size_t)snprintf(big, sizeof big, "[{\"text\": \"");
        for (int w = 0; w < 600; ++w)
            k += (size_t)snprintf(big + k, sizeof big - k, "word%03d ", w);
        k += (size_t)snprintf(big + k, sizeof big - k, "\"}]");
        xksm_import_opts o = {0};
        o.category = XKSM_CAT_PROJECT;
        o.chunk_target = 256;
        CHECK(xksm_import_buffer(x, "big.json", big, k, &o, &r) == XKSM_OK);
        n = rows_of(P, r.source_id, R, 16);
        int same = n > 1;
        for (int i = 0; i < n; ++i)
            same &= !strcmp(R[i].raw, R[0].raw) && strlen(R[i].text) <= 512;
        printf("long record: %d pieces of <= 512 bytes\n", n);
        CHECK(same && (size_t)n == r.chunks && !strncmp(R[0].text, "text: word000", 13));
    }

    /* dedup: the same record in another file is a duplicate */
    CHECK(imp(x, "m.jsonl", "{\"name\": \"beta\"}\n{\"name\": \"gamma\"}\n", &r) == XKSM_OK);
    CHECK(r.added == 1 && r.duplicates == 1);

    /* random mutations of valid inputs: OK or EINVAL (or EXISTS), never a crash, and a
     * refused import leaves the store unchanged */
    {
        static const char *const seeds[] = {
            "[{\"a\": [1, 2.5e-3, \"s\\u00e9\"], \"b\": {\"c\": null, \"d\": true}}, \"x\"]",
            "{\"k\": \"v\"}\n[1, {\"n\": -0.0}]\n\"t\\ud800\\udf48\"\n",
            "h1,h2,h3\n1,\"a,\"\"b\"\"\",c\n2,,\"multi\nline\"\n",
        };
        static const char *const uri[] = {"fz.json", "fz.jsonl", "fz.csv"};
        static const char alphabet[] = "[]{}\",:\\u0123456789abcdefnrtx.-+eE \n\r";
        int ok = 0, inval = 0, bad_state = 0;
        for (int it = 0; it < 3000; ++it) {
            int s = it % 3;
            char buf[256];
            size_t len = strlen(seeds[s]);
            memcpy(buf, seeds[s], len + 1);
            int edits = 1 + (int)(next() % 4);
            for (int e = 0; e < edits; ++e) {
                size_t at = (size_t)(next() % len);
                switch (next() % 3) {
                case 0: /* replace */
                    buf[at] = alphabet[next() % (sizeof alphabet - 1)];
                    break;
                case 1: /* delete */
                    memmove(buf + at, buf + at + 1, len - at);
                    --len;
                    break;
                default: /* insert */
                    if (len + 2 < sizeof buf) {
                        memmove(buf + at + 1, buf + at, len - at + 1);
                        buf[at] = alphabet[next() % (sizeof alphabet - 1)];
                        ++len;
                    }
                }
                if (!len)
                    break;
            }
            long long m0 = count(P, "SELECT count(*) FROM memory");
            xksm_import_opts o = {0};
            o.category = XKSM_CAT_PROJECT;
            xksm_status st = xksm_import_buffer(x, uri[s], buf, len, &o, &r);
            if (st == XKSM_OK)
                ++ok;
            else if (st == XKSM_EINVAL) {
                ++inval;
                bad_state += count(P, "SELECT count(*) FROM memory") != m0;
            } else if (st != XKSM_EXISTS)
                ++bad_state;
        }
        printf("mutations: 3000, accepted %d, refused %d, unexpected %d\n", ok, inval, bad_state);
        CHECK(bad_state == 0 && ok > 0 && inval > 0);
    }

    xksm_close(x);
    remove_db(P);
    return test_report("structured");
}
