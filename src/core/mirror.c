/* mirror.c: the readable file tree of agent sessions.
 *   ROOT/<harness>/<YYYY-MM-DD_HHMMSS>_<session id>/session.md
 *   ROOT/<harness>/<...>/<YYYY-MM-DD_HH-MM-SS.mmm>_<role>_<sha8>.md   one per turn
 * A turn file holds the turn's stored text (header line + text) and is written when the turn
 * is stored; times are local. The index (SQLite) stays authoritative for search; every path
 * written is recorded in kv (mirror:root, mirror:dir:H:SID, mirror:file:SOURCE) so deleting a
 * memory or forgetting a session removes exactly its files. Files 0600, directories 0700. */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { PATHMAX = 4096 };

static int mkdir_p(const char *path)
{
    char tmp[PATHMAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp)
        return 0;
    for (char *p = tmp + 1; *p; ++p)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return 0;
            *p = '/';
        }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static void local_stamp(int64_t us, const char *fmt, char *out, size_t cap)
{
    time_t s = (time_t)(us / 1000000);
    struct tm tm;
    localtime_r(&s, &tm);
    strftime(out, cap, fmt, &tm);
}

/* harness, session, role from xksm-turn://H/SID/ROLE/US; 0 if not a turn uri */
static int turn_parts(const char *uri, char *h, size_t hc, char *sid, size_t sc, char *role, size_t rc,
                      int64_t *us)
{
    if (strncmp(uri, "xksm-turn://", 12))
        return 0;
    const char *p = uri + 12, *a = strchr(p, '/'), *b = a ? strchr(a + 1, '/') : NULL,
               *c = b ? strchr(b + 1, '/') : NULL;
    if (!a || !b || !c || a == p || b == a + 1 || c == b + 1)
        return 0;
    snprintf(h, hc, "%.*s", (int)(a - p), p);
    snprintf(sid, sc, "%.*s", (int)(b - a - 1), a + 1);
    snprintf(role, rc, "%.*s", (int)(c - b - 1), b + 1);
    *us = strtoll(c + 1, NULL, 10);
    return 1;
}

/* path components must not escape the tree */
static int safe_part(const char *s)
{
    if (!*s || !strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    for (; *s; ++s)
        if (*s == '/' || *s == '\\' || (unsigned char)*s < 0x20)
            return 0;
    return 1;
}

static int kv_str(xksm *x, const char *key, char *out, size_t cap)
{
    size_t n = 0;
    if (xksm_kv_get(x, key, out, cap - 1, &n) != XKSM_OK)
        return 0;
    out[n < cap - 1 ? n : cap - 1] = 0;
    return 1;
}

static int write_file(const char *path, const char *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, data + off, n - off);
        if (w <= 0) {
            close(fd);
            return 0;
        }
        off += (size_t)w;
    }
    return close(fd) == 0;
}

/* session.md: harness, id, working directory, time range, turns (from the session table) */
static void write_session_md(xksm *x, const char *dir, const char *h, const char *sid)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT first_us, last_us, turns, cwd FROM session WHERE harness = ?1 AND id = ?2",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, h, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, sid, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return;
    }
    char a[48], b[48], path[PATHMAX], md[PATHMAX + 512];
    local_stamp(sqlite3_column_int64(st, 0), "%Y-%m-%d %H:%M:%S %z", a, sizeof a);
    local_stamp(sqlite3_column_int64(st, 1), "%Y-%m-%d %H:%M:%S %z", b, sizeof b);
    const char *cwd = (const char *)sqlite3_column_text(st, 3);
    int n = snprintf(md, sizeof md,
                     "# %s session %s\n\n- first turn: %s\n- last turn: %s\n- turns: %lld\n- working directory: %s\n",
                     h, sid, a, b, (long long)sqlite3_column_int64(st, 2), cwd ? cwd : "-");
    sqlite3_finalize(st);
    if (n > 0 && snprintf(path, sizeof path, "%s/session.md", dir) < (int)sizeof path)
        write_file(path, md, (size_t)n < sizeof md ? (size_t)n : sizeof md - 1);
}

xksm_status xksm_mirror_turn(xksm *x, const char *root, xksm_id source)
{
    if (!x || !root || !*root)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT uri, body, sha256 FROM source WHERE id = ?1", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(st, 1, source) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "mirror: %s", sqlite3_errmsg(x->db));
    }
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return XKSM_ENOTFOUND;
    }
    char h[64], sid[256], role[32], dkey[400], dname[512], dir[PATHMAX], path[PATHMAX], rel[PATHMAX],
        fkey[64];
    int64_t us = 0;
    const char *uri = (const char *)sqlite3_column_text(st, 0);
    const uint8_t *sha = sqlite3_column_blob(st, 2);
    if (!uri || !turn_parts(uri, h, sizeof h, sid, sizeof sid, role, sizeof role, &us) || !safe_part(h) ||
        !safe_part(sid) || !safe_part(role) || sqlite3_column_bytes(st, 2) != 32) {
        sqlite3_finalize(st);
        return XKSM_EINVAL; /* not an agent turn */
    }
    /* the session folder: fixed at its first turn, recorded, reused */
    snprintf(dkey, sizeof dkey, "mirror:dir:%s:%s", h, sid);
    if (!kv_str(x, dkey, dname, sizeof dname)) {
        int64_t first = us;
        sqlite3_stmt *ss = NULL;
        if (sqlite3_prepare_v2(x->db, "SELECT first_us FROM session WHERE harness = ?1 AND id = ?2", -1, &ss,
                               NULL) == SQLITE_OK &&
            sqlite3_bind_text(ss, 1, h, -1, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_text(ss, 2, sid, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(ss) == SQLITE_ROW)
            first = sqlite3_column_int64(ss, 0);
        sqlite3_finalize(ss);
        char stamp[32];
        local_stamp(first, "%Y-%m-%d_%H%M%S", stamp, sizeof stamp);
        snprintf(dname, sizeof dname, "%s_%s", stamp, sid);
        xksm_kv_put(x, dkey, dname, strlen(dname));
    }
    char stamp[40];
    local_stamp(us, "%Y-%m-%d_%H-%M-%S", stamp, sizeof stamp);
    if (snprintf(dir, sizeof dir, "%s/%s/%s", root, h, dname) >= (int)sizeof dir ||
        snprintf(rel, sizeof rel, "%s/%s/%s.%03d_%s_%02x%02x%02x%02x.md", h, dname, stamp,
                 (int)((us / 1000) % 1000), role, sha[0], sha[1], sha[2], sha[3]) >= (int)sizeof rel ||
        snprintf(path, sizeof path, "%s/%s", root, rel) >= (int)sizeof path) {
        sqlite3_finalize(st);
        return XKSM_EINVAL;
    }
    int ok = mkdir_p(dir) &&
             write_file(path, (const char *)sqlite3_column_blob(st, 1), (size_t)sqlite3_column_bytes(st, 1));
    sqlite3_finalize(st);
    if (!ok)
        return xksm__fail(x, XKSM_EIO, "mirror: cannot write %s: %s", path, strerror(errno));
    char cur[PATHMAX];
    if (!kv_str(x, "mirror:root", cur, sizeof cur) || strcmp(cur, root))
        xksm_kv_put(x, "mirror:root", root, strlen(root));
    snprintf(fkey, sizeof fkey, "mirror:file:%lld", (long long)source);
    xksm_kv_put(x, fkey, rel, strlen(rel));
    write_session_md(x, dir, h, sid);
    return XKSM_OK;
}

xksm_status xksm_mirror_rebuild(xksm *x, const char *root, size_t *written)
{
    if (written)
        *written = 0;
    if (!x || !root)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    xksm_id *ids = NULL;
    size_t n = 0, cap = 0;
    xksm_status rc = XKSM_OK;
    /* sources of agent turns that still have an active memory, oldest first */
    if (sqlite3_prepare_v2(x->db,
                           "SELECT s.id FROM source s WHERE substr(s.uri, 1, 12) = 'xksm-turn://' AND EXISTS "
                           "(SELECT 1 FROM memory m WHERE m.source_id = s.id AND m.state = 1) ORDER BY s.id",
                           -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "mirror rebuild: %s", sqlite3_errmsg(x->db));
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? 2 * cap : 1024;
            xksm_id *ni = realloc(ids, sizeof *ni * cap);
            if (!ni) {
                rc = XKSM_ENOMEM;
                break;
            }
            ids = ni;
        }
        ids[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    size_t done = 0;
    for (size_t i = 0; rc == XKSM_OK && i < n; ++i) {
        xksm_status s = xksm_mirror_turn(x, root, ids[i]);
        if (s == XKSM_OK)
            ++done;
        else if (s != XKSM_EINVAL)
            rc = s;
    }
    free(ids);
    if (written)
        *written = done;
    return rc;
}

/* remove the mirror file of one source (no error if none) */
static void unlink_source_file(xksm *x, xksm_id source)
{
    char fkey[64], rel[PATHMAX], root[PATHMAX], path[2 * PATHMAX];
    snprintf(fkey, sizeof fkey, "mirror:file:%lld", (long long)source);
    if (!kv_str(x, fkey, rel, sizeof rel) || !kv_str(x, "mirror:root", root, sizeof root))
        return;
    snprintf(path, sizeof path, "%s/%s", root, rel);
    unlink(path);
    xksm_kv_delete(x, fkey);
}

xksm_status xksm_mirror_prune(xksm *x, xksm_id source)
{
    if (!x)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    int live = 1;
    if (sqlite3_prepare_v2(x->db, "SELECT EXISTS (SELECT 1 FROM memory WHERE source_id = ?1 AND state = 1)", -1,
                           &st, NULL) == SQLITE_OK &&
        sqlite3_bind_int64(st, 1, source) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        live = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (live)
        return XKSM_OK; /* other chunks of the turn remain */
    unlink_source_file(x, source);
    st = NULL;
    int ok = sqlite3_prepare_v2(x->db, "DELETE FROM source WHERE id = ?1 AND substr(uri, 1, 12) = 'xksm-turn://'",
                                -1, &st, NULL) == SQLITE_OK &&
             sqlite3_bind_int64(st, 1, source) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? XKSM_OK : xksm__fail(x, XKSM_EIO, "mirror prune: %s", sqlite3_errmsg(x->db));
}

/* every file of a session folder, then the folder (called by xksm_session_forget) */
void xksm__mirror_forget(xksm *x, const char *harness, const char *id, const xksm_id *sources, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        unlink_source_file(x, sources[i]);
    char dkey[400], dname[512], root[PATHMAX], dir[2 * PATHMAX], md[2 * PATHMAX + 16];
    snprintf(dkey, sizeof dkey, "mirror:dir:%s:%s", harness, id);
    if (!kv_str(x, dkey, dname, sizeof dname) || !kv_str(x, "mirror:root", root, sizeof root))
        return;
    snprintf(dir, sizeof dir, "%s/%s/%s", root, harness, dname);
    snprintf(md, sizeof md, "%s/session.md", dir);
    unlink(md);
    rmdir(dir); /* only when empty: files not written by the mirror are left alone */
    xksm_kv_delete(x, dkey);
}
