/* import.c: explicit import of text and Markdown: source row (original bytes) plus one
 * memory per chunk, in one transaction. */
#include "internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

typedef struct span {
    size_t off, len;
} span;

typedef struct spans {
    span *v;
    size_t n, cap;
} spans;

static int push(spans *s, size_t off, size_t len)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? 2 * s->cap : 64;
        span *v = realloc(s->v, cap * sizeof *v);
        if (!v)
            return 0;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = (span){off, len};
    return 1;
}

static int ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static int all_ws(const char *b, size_t s, size_t e)
{
    for (; s < e; ++s)
        if (!ws(b[s]))
            return 0;
    return 1;
}

/* Markdown ATX heading: up to 3 spaces, then '#' */
static int heading(const char *b, size_t s, size_t e)
{
    size_t i = s;
    while (i < e && i - s < 3 && b[i] == ' ')
        ++i;
    return i < e && b[i] == '#';
}

/* emit [s, e), cutting pieces of at most max bytes at whitespace (or a UTF-8 boundary) */
static int emit(spans *out, const char *b, size_t s, size_t e, size_t max)
{
    while (e - s > max) {
        size_t cut = s + max;
        size_t w = cut;
        while (w > s + max / 2 && !ws(b[w - 1]))
            --w;
        if (w > s + max / 2)
            cut = w;
        else
            while (cut > s + 1 && ((unsigned char)b[cut] & 0xC0) == 0x80)
                --cut; /* never split a code point */
        if (!all_ws(b, s, cut) && !push(out, s, cut - s))
            return 0;
        s = cut;
    }
    if (e > s && !all_ws(b, s, e) && !push(out, s, e - s))
        return 0;
    return 1;
}

/* Paragraphs (runs of non-blank lines) packed up to target bytes; a Markdown heading
 * always starts a new chunk; pieces never exceed 2 x target. */
static int chunk(const char *b, size_t n, size_t target, int markdown, spans *out)
{
    size_t max = 2 * target;
    size_t cs = 0, ce = 0;      /* current chunk */
    int have = 0;
    size_t ps = 0, pe = 0;      /* current paragraph */
    int in_para = 0, para_head = 0;

    for (size_t ls = 0; ls <= n;) {
        size_t le = ls;
        while (le < n && b[le] != '\n')
            ++le;
        int blank = ls >= n || all_ws(b, ls, le);
        int head = !blank && markdown && heading(b, ls, le);

        /* close the paragraph on a blank line, at the end, or before a heading */
        if (in_para && (blank || head)) {
            if (have && !para_head && pe - cs <= target) {
                ce = pe;
            } else {
                if (have && !emit(out, b, cs, ce, max))
                    return 0;
                cs = ps;
                ce = pe;
                have = 1;
            }
            in_para = 0;
        }
        if (!blank) {
            if (!in_para) {
                ps = ls;
                para_head = head;
                in_para = 1;
            }
            pe = le;
        }
        if (le >= n)
            break;
        ls = le + 1;
    }
    if (in_para) { /* file ended inside a paragraph */
        if (have && !para_head && pe - cs <= target)
            ce = pe;
        else {
            if (have && !emit(out, b, cs, ce, max))
                return 0;
            cs = ps;
            ce = pe;
            have = 1;
        }
    }
    return !have || emit(out, b, cs, ce, max);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a)
        return 0;
    for (size_t i = 0; i < b; ++i) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return 0;
    }
    return 1;
}

xksm_status xksm_import_buffer(xksm *x, const char *uri, const void *data, size_t len,
                               const xksm_import_opts *opts, xksm_import_result *res)
{
    if (!x || !uri || (!data && len) || !opts || !res)
        return XKSM_EINVAL;
    memset(res, 0, sizeof *res);
    if (len > XKSM_MAX_SOURCE)
        return xksm__fail(x, XKSM_EINVAL, "%s: %zu bytes; limit %zu", uri, len, XKSM_MAX_SOURCE);
    if (!xksm__utf8_valid(data, len))
        return xksm__fail(x, XKSM_EINVAL, "%s: not valid UTF-8 text", uri);
    if ((unsigned)opts->category > XKSM_CAT_ARCHIVAL)
        return xksm__fail(x, XKSM_EINVAL, "category %d out of range", (int)opts->category);

    xksm_format fmt = opts->format;
    if (fmt == XKSM_FMT_AUTO)
        fmt = ends_with(uri, ".md") || ends_with(uri, ".markdown") ? XKSM_FMT_MARKDOWN
                                                                   : XKSM_FMT_TEXT;
    if (fmt != XKSM_FMT_TEXT && fmt != XKSM_FMT_MARKDOWN)
        return xksm__fail(x, XKSM_EINVAL, "format %d not supported", (int)fmt);
    size_t target = opts->chunk_target ? opts->chunk_target : 1024;
    if (target < 64 || target > XKSM_MAX_SOURCE)
        return xksm__fail(x, XKSM_EINVAL, "chunk_target %zu outside 64..%zu", target,
                          XKSM_MAX_SOURCE);

    uint8_t sum[32];
    xksm__sha256(data, len, sum);
    spans sp = {0};
    if (!chunk(data, len, target, fmt == XKSM_FMT_MARKDOWN, &sp)) {
        free(sp.v);
        return XKSM_ENOMEM;
    }

    xksm_status rc = XKSM_OK;
    sqlite3_stmt *st = NULL;
    int64_t now = xksm__now_us();
    if (sqlite3_exec(x->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "begin: %s", sqlite3_errmsg(x->db));
        goto out;
    }
    /* identical source already stored: report it, write nothing */
    if (sqlite3_prepare_v2(x->db, "SELECT id FROM source WHERE sha256 = ?1", -1, &st, NULL) !=
            SQLITE_OK ||
        sqlite3_bind_blob(st, 1, sum, 32, SQLITE_STATIC) != SQLITE_OK)
        goto sql_error;
    int step = sqlite3_step(st);
    if (step == SQLITE_ROW) {
        res->source_id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        st = NULL;
        sqlite3_exec(x->db, "COMMIT", NULL, NULL, NULL);
        rc = XKSM_EXISTS;
        goto out;
    }
    if (step != SQLITE_DONE)
        goto sql_error;
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(x->db,
                           "INSERT INTO source(kind, uri, sha256, bytes, imported_us, body) "
                           "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, fmt == XKSM_FMT_MARKDOWN ? "markdown" : "text", -1,
                          SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, uri, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_blob(st, 3, sum, 32, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(st, 4, (sqlite3_int64)len) != SQLITE_OK ||
        sqlite3_bind_int64(st, 5, now) != SQLITE_OK ||
        sqlite3_bind_blob64(st, 6, len ? data : "", len, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_DONE)
        goto sql_error;
    sqlite3_finalize(st);
    st = NULL;
    res->source_id = sqlite3_last_insert_rowid(x->db);

    for (size_t i = 0; i < sp.n; ++i) {
        xksm_memory_in in = {0};
        in.ns = opts->ns;
        in.category = opts->category;
        in.text = (const char *)data + sp.v[i].off;
        in.text_len = sp.v[i].len;
        in.source_id = res->source_id;
        in.source_off = (int64_t)sp.v[i].off;
        in.source_len = (int64_t)sp.v[i].len;
        xksm_id id;
        xksm_status s = xksm__memory_add_in_txn(x, &in, &id, now);
        if (s == XKSM_OK)
            ++res->added;
        else if (s == XKSM_EXISTS)
            ++res->duplicates;
        else { /* EINVAL here would be a chunker bug: refuse the whole import */
            sqlite3_exec(x->db, "ROLLBACK", NULL, NULL, NULL);
            rc = s;
            memset(res, 0, sizeof *res);
            goto out;
        }
    }
    res->chunks = sp.n;
    if (sqlite3_exec(x->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto sql_error;
    xksm__cache_records_delta(x, (int64_t)res->added);
    goto out;

sql_error:
    rc = xksm__fail(x, XKSM_EIO, "import %s: %s", uri, sqlite3_errmsg(x->db));
    sqlite3_finalize(st);
    st = NULL;
    sqlite3_exec(x->db, "ROLLBACK", NULL, NULL, NULL);
    memset(res, 0, sizeof *res);
out:
    sqlite3_finalize(st);
    free(sp.v);
    return rc;
}

xksm_status xksm_import_file(xksm *x, const char *path, const xksm_import_opts *opts,
                             xksm_import_result *res)
{
    if (!x || !path || !opts || !res)
        return XKSM_EINVAL;
    memset(res, 0, sizeof *res);
    FILE *f = fopen(path, "rb");
    if (!f)
        return xksm__fail(x, XKSM_EIO, "open %s failed", path);
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    xksm_status rc = XKSM_OK;
    while (buf) {
        if (n == cap) {
            if (cap > XKSM_MAX_SOURCE) { /* read one byte past the limit: refuse */
                rc = xksm__fail(x, XKSM_EINVAL, "%s: larger than %zu bytes", path,
                                XKSM_MAX_SOURCE);
                break;
            }
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) /* e.g. a directory: EISDIR */
                rc = xksm__fail(x, XKSM_EIO, "read %s failed", path);
            break;
        }
    }
    fclose(f);
    if (!buf)
        return XKSM_ENOMEM;
    if (rc == XKSM_OK)
        rc = xksm_import_buffer(x, path, buf, n, opts, res);
    free(buf);
    return rc;
}
