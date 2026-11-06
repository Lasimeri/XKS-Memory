/* memory.c: add / get / delete of memory records. */
#include "internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define XKSM_MAX_TEXT ((size_t)16 << 20) /* 16 MiB per memory; larger input is chunked */

/* RFC 3629 UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. */
int xksm__utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t need;
        uint8_t lo = 0x80, hi = 0xBF; /* allowed range of the first continuation byte */
        if (c < 0xC2)
            return 0; /* stray continuation or overlong 2-byte */
        else if (c < 0xE0)
            need = 1;
        else if (c < 0xF0) {
            need = 2;
            if (c == 0xE0) lo = 0xA0; /* overlong 3-byte */
            if (c == 0xED) hi = 0x9F; /* surrogates */
        } else if (c < 0xF5) {
            need = 3;
            if (c == 0xF0) lo = 0x90; /* overlong 4-byte */
            if (c == 0xF4) hi = 0x8F; /* above U+10FFFF */
        } else
            return 0;
        if (n - i <= need)
            return 0; /* truncated */
        if (s[i + 1] < lo || s[i + 1] > hi)
            return 0;
        for (size_t k = 2; k <= need; ++k)
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
        i += need + 1;
    }
    return 1;
}

static int is_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Trim ASCII whitespace at both ends, CRLF -> LF. Returns a malloc'd NUL-terminated
 * copy and its length, or NULL on allocation failure. */
static char *normalize(const char *text, size_t n, size_t *out_len)
{
    const uint8_t *s = (const uint8_t *)text;
    size_t a = 0, b = n;
    while (a < b && is_space(s[a]))
        ++a;
    while (b > a && is_space(s[b - 1]))
        --b;
    char *out = malloc(b - a + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = a; i < b; ++i) {
        if (s[i] == '\r' && i + 1 < b && s[i + 1] == '\n')
            continue;
        out[o++] = (char)s[i];
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

static xksm_status sql_fail(xksm *x, const char *what)
{
    xksm_status s = xksm__fail(x, XKSM_EIO, "%s: %s", what, sqlite3_errmsg(x->db));
    sqlite3_exec(x->db, "ROLLBACK", NULL, NULL, NULL);
    return s;
}

static int log_event(xksm *x, xksm_id id, const char *action, int64_t at)
{
    sqlite3_stmt *st = NULL;
    int ok = sqlite3_prepare_v2(x->db,
                                "INSERT INTO memory_event_log(memory_id, at_us, action) "
                                "VALUES (?1, ?2, ?3)",
                                -1, &st, NULL) == SQLITE_OK &&
             sqlite3_bind_int64(st, 1, id) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, at) == SQLITE_OK &&
             sqlite3_bind_text(st, 3, action, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

xksm_status xksm_memory_add(xksm *x, const xksm_memory_in *in, xksm_id *id_out)
{
    if (!x || !in || !id_out || !in->text)
        return XKSM_EINVAL;
    *id_out = 0;
    if ((unsigned)in->category > XKSM_CAT_ARCHIVAL)
        return xksm__fail(x, XKSM_EINVAL, "category %d out of range", (int)in->category);
    double imp = in->importance == 0.0 ? 0.5 : in->importance;
    if (!(imp >= 0.0 && imp <= 1.0))
        return xksm__fail(x, XKSM_EINVAL, "importance must be in 0..1");
    if (in->text_len > XKSM_MAX_TEXT)
        return xksm__fail(x, XKSM_EINVAL, "text is %zu bytes; limit %zu", in->text_len,
                          XKSM_MAX_TEXT);
    if (!xksm__utf8_valid((const uint8_t *)in->text, in->text_len))
        return xksm__fail(x, XKSM_EINVAL, "text is not valid UTF-8");

    size_t len;
    char *text = normalize(in->text, in->text_len, &len);
    if (!text)
        return XKSM_ENOMEM;
    if (len == 0) {
        free(text);
        return xksm__fail(x, XKSM_EINVAL, "text is empty after trimming whitespace");
    }
    uint8_t sum[32];
    xksm__sha256(text, len, sum);
    const char *ns = in->ns ? in->ns : "default";
    int64_t now = xksm__now_us();
    xksm_status rc = XKSM_OK;
    sqlite3_stmt *st = NULL;

    if (sqlite3_exec(x->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "begin: %s", sqlite3_errmsg(x->db));
        goto out;
    }

    /* dedup: same namespace + same checksum = same memory */
    if (sqlite3_prepare_v2(x->db,
                           "SELECT id FROM memory WHERE namespace = ?1 AND checksum = ?2 LIMIT 1",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_blob(st, 2, sum, 32, SQLITE_STATIC) != SQLITE_OK) {
        rc = sql_fail(x, "dedup lookup");
        goto out;
    }
    int step = sqlite3_step(st);
    if (step == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        st = NULL;
        sqlite3_exec(x->db, "COMMIT", NULL, NULL, NULL);
        rc = XKSM_EXISTS;
        goto out;
    }
    if (step != SQLITE_DONE) {
        rc = sql_fail(x, "dedup lookup");
        goto out;
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(x->db,
                           "INSERT INTO memory(namespace, category, state, source_id, text, "
                           "created_us, modified_us, event_us, importance, checksum) "
                           "VALUES (?1, ?2, 1, ?3, ?4, ?5, ?5, ?6, ?7, ?8)",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int(st, 2, (int)in->category) != SQLITE_OK ||
        (in->source_id ? sqlite3_bind_int64(st, 3, in->source_id)
                       : sqlite3_bind_null(st, 3)) != SQLITE_OK ||
        sqlite3_bind_text64(st, 4, text, len, SQLITE_STATIC, SQLITE_UTF8) != SQLITE_OK ||
        sqlite3_bind_int64(st, 5, now) != SQLITE_OK ||
        (in->event_us != 0 && in->event_us != XKSM_TIME_NONE
             ? sqlite3_bind_int64(st, 6, in->event_us)
             : sqlite3_bind_null(st, 6)) != SQLITE_OK ||
        sqlite3_bind_double(st, 7, imp) != SQLITE_OK ||
        sqlite3_bind_blob(st, 8, sum, 32, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_DONE) {
        rc = sql_fail(x, "insert memory");
        goto out;
    }
    *id_out = sqlite3_last_insert_rowid(x->db);
    if (!log_event(x, *id_out, "add", now)) {
        rc = sql_fail(x, "log add");
        *id_out = 0;
        goto out;
    }
    if (sqlite3_exec(x->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        rc = sql_fail(x, "commit");
        *id_out = 0;
    }
out:
    sqlite3_finalize(st);
    free(text);
    return rc;
}

static char *dup_column(sqlite3_stmt *st, int col, size_t *len)
{
    const unsigned char *p = sqlite3_column_text(st, col);
    size_t n = (size_t)sqlite3_column_bytes(st, col);
    char *s = malloc(n + 1);
    if (s) {
        if (n)
            memcpy(s, p, n);
        s[n] = 0;
        if (len)
            *len = n;
    }
    return s;
}

xksm_status xksm_memory_get(xksm *x, xksm_id id, xksm_memory *m)
{
    if (!x || !m)
        return XKSM_EINVAL;
    memset(m, 0, sizeof *m);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT namespace, category, state, text, created_us, modified_us, "
                           "event_us, importance, pinned, source_id, checksum "
                           "FROM memory WHERE id = ?1",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(st, 1, id) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "get: %s", sqlite3_errmsg(x->db));
    }
    int step = sqlite3_step(st);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(st);
        return step == SQLITE_DONE
                   ? xksm__fail(x, XKSM_ENOTFOUND, "memory %lld not found", (long long)id)
                   : xksm__fail(x, XKSM_EIO, "get: %s", sqlite3_errmsg(x->db));
    }
    m->id = id;
    m->ns = dup_column(st, 0, NULL);
    m->category = (xksm_category)sqlite3_column_int(st, 1);
    m->state = (xksm_state)sqlite3_column_int(st, 2);
    m->text = dup_column(st, 3, &m->text_len);
    m->created_us = sqlite3_column_int64(st, 4);
    m->modified_us = sqlite3_column_int64(st, 5);
    m->event_us = sqlite3_column_type(st, 6) == SQLITE_NULL ? XKSM_TIME_NONE
                                                             : sqlite3_column_int64(st, 6);
    m->importance = sqlite3_column_double(st, 7);
    m->pinned = sqlite3_column_int(st, 8);
    m->source_id = sqlite3_column_int64(st, 9);
    if (sqlite3_column_bytes(st, 10) == 32)
        memcpy(m->checksum, sqlite3_column_blob(st, 10), 32);
    sqlite3_finalize(st);
    if (!m->ns || !m->text) {
        xksm_memory_free(m);
        return XKSM_ENOMEM;
    }
    return XKSM_OK;
}

void xksm_memory_free(xksm_memory *m)
{
    if (!m)
        return;
    free(m->ns);
    free(m->text);
    m->ns = m->text = NULL;
}

xksm_status xksm_memory_delete(xksm *x, xksm_id id)
{
    if (!x)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_exec(x->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return xksm__fail(x, XKSM_EIO, "begin: %s", sqlite3_errmsg(x->db));
    if (sqlite3_prepare_v2(x->db, "DELETE FROM memory WHERE id = ?1", -1, &st, NULL) !=
            SQLITE_OK ||
        sqlite3_bind_int64(st, 1, id) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return sql_fail(x, "delete");
    }
    sqlite3_finalize(st);
    if (sqlite3_changes(x->db) == 0) {
        sqlite3_exec(x->db, "ROLLBACK", NULL, NULL, NULL);
        return xksm__fail(x, XKSM_ENOTFOUND, "memory %lld not found", (long long)id);
    }
    if (!log_event(x, id, "delete", xksm__now_us()))
        return sql_fail(x, "log delete");
    if (sqlite3_exec(x->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        return sql_fail(x, "commit");
    return XKSM_OK;
}
