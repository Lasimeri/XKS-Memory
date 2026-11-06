/* verify.c: integrity of a store, recomputed from the bytes.
 *   memory.checksum  = SHA-256(text)                (every memory, any state)
 *   source.sha256    = SHA-256(body), bytes = length (every source that keeps its body)
 *   session files    = the bytes of ROOT/<kv mirror:file:N> hash to source N's sha256, and
 *                      the file name ends in _<first 8 hex digits of it>.md
 * Everything is read inside one read transaction (a WAL snapshot), so writers running at the
 * same time neither block it nor show up half-written. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void xksm_sha256_digest(const void *data, size_t n, uint8_t out[32])
{
    xksm__sha256(data ? data : "", data ? n : 0, out);
}

xksm_status xksm_source_sha256(xksm *x, xksm_id source, uint8_t out[32], int64_t *bytes)
{
    if (!x || !out)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT sha256, bytes FROM source WHERE id = ?1", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(st, 1, source) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "source %lld: %s", (long long)source, sqlite3_errmsg(x->db));
    }
    xksm_status rc = XKSM_ENOTFOUND;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32) {
        memcpy(out, sqlite3_column_blob(st, 0), 32);
        if (bytes)
            *bytes = sqlite3_column_int64(st, 1);
        rc = XKSM_OK;
    }
    sqlite3_finalize(st);
    return rc;
}

/* SHA-256 of a file's bytes in 64 KiB reads; 0 when it cannot be read */
static int hash_file(const char *path, uint8_t out[32])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    static _Thread_local uint8_t buf[1 << 16];
    xksm_sha256 c;
    xksm__sha256_init(&c);
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        xksm__sha256_update(&c, buf, n);
    int ok = !ferror(f);
    fclose(f);
    xksm__sha256_final(&c, out);
    return ok;
}

xksm_status xksm_file_matches(const char *path, const uint8_t sha[32])
{
    uint8_t h[32];
    if (!path || !sha)
        return XKSM_EINVAL;
    if (!hash_file(path, h))
        return XKSM_ENOTFOUND;
    return memcmp(h, sha, 32) ? XKSM_ECORRUPT : XKSM_OK;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

xksm_status xksm_verify(xksm *x, const char *root, xksm_verify_report *r)
{
    if (!x || !r)
        return XKSM_EINVAL;
    memset(r, 0, sizeof *r);
    double t0 = now_s();
    char rootbuf[4096] = {0};
    if (!root) { /* the tree the turns were mirrored into */
        size_t n = 0;
        if (xksm_kv_get(x, "mirror:root", rootbuf, sizeof rootbuf - 1, &n) == XKSM_OK) {
            rootbuf[n < sizeof rootbuf ? n : sizeof rootbuf - 1] = 0;
            root = rootbuf;
        }
    }
    if (sqlite3_exec(x->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK)
        return xksm__fail(x, XKSM_EIO, "verify: %s", sqlite3_errmsg(x->db));
    xksm_status rc = XKSM_OK;
    sqlite3_stmt *st = NULL;
    uint8_t h[32];
    /* memories */
    if (sqlite3_prepare_v2(x->db, "SELECT id, text, checksum FROM memory ORDER BY id", -1, &st, NULL) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "verify: %s", sqlite3_errmsg(x->db));
        goto out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const void *t = sqlite3_column_text(st, 1);
        size_t n = (size_t)sqlite3_column_bytes(st, 1);
        xksm__sha256(t ? t : "", n, h);
        r->memories++;
        if (sqlite3_column_bytes(st, 2) != 32 || memcmp(h, sqlite3_column_blob(st, 2), 32)) {
            if (!r->memory_bad++)
                r->first_bad_memory = sqlite3_column_int64(st, 0);
        }
    }
    sqlite3_finalize(st);
    /* sources */
    if (sqlite3_prepare_v2(x->db, "SELECT id, sha256, bytes, body FROM source ORDER BY id", -1, &st, NULL) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "verify: %s", sqlite3_errmsg(x->db));
        goto out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        r->sources++;
        if (sqlite3_column_type(st, 3) == SQLITE_NULL) {
            r->sources_without_body++; /* imported by path: the file itself is the source */
            continue;
        }
        const void *b = sqlite3_column_blob(st, 3);
        size_t n = (size_t)sqlite3_column_bytes(st, 3);
        xksm__sha256(b ? b : "", n, h);
        if (sqlite3_column_bytes(st, 1) != 32 || memcmp(h, sqlite3_column_blob(st, 1), 32) ||
            sqlite3_column_int64(st, 2) != (int64_t)n) {
            if (!r->source_bad++)
                r->first_bad_source = sqlite3_column_int64(st, 0);
        }
    }
    sqlite3_finalize(st);
    /* session files */
    if (root && *root) {
        if (sqlite3_prepare_v2(x->db,
                               "SELECT s.id, s.sha256, CAST(k.value AS TEXT) FROM kv k JOIN source s "
                               "ON s.id = CAST(substr(k.key, 13) AS INTEGER) "
                               "WHERE substr(k.key, 1, 12) = 'mirror:file:' ORDER BY s.id",
                               -1, &st, NULL) != SQLITE_OK) {
            rc = xksm__fail(x, XKSM_EIO, "verify: %s", sqlite3_errmsg(x->db));
            goto out;
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            const uint8_t *s = sqlite3_column_blob(st, 1);
            const char *rel = (const char *)sqlite3_column_text(st, 2);
            if (!s || sqlite3_column_bytes(st, 1) != 32 || !rel)
                continue;
            r->files++;
            char path[8192], tail[16];
            snprintf(path, sizeof path, "%s/%s", root, rel);
            snprintf(tail, sizeof tail, "_%02x%02x%02x%02x.md", s[0], s[1], s[2], s[3]);
            size_t rl = strlen(rel), tl = strlen(tail);
            int name_ok = rl >= tl && !strcmp(rel + rl - tl, tail);
            if (!hash_file(path, h)) {
                if (!r->file_missing++)
                    r->first_bad_file_source = r->first_bad_file_source ? r->first_bad_file_source : sqlite3_column_int64(st, 0);
            } else if (!name_ok || memcmp(h, s, 32)) {
                if (!r->file_bad++)
                    r->first_bad_file_source = r->first_bad_file_source ? r->first_bad_file_source : sqlite3_column_int64(st, 0);
            }
        }
        sqlite3_finalize(st);
    }
out:
    sqlite3_exec(x->db, "COMMIT", NULL, NULL, NULL);
    r->seconds = now_s() - t0;
    return rc;
}
