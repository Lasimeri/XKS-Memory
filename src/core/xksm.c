/* xksm.c: handle lifecycle. */
#include "internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#define XKSM_STR2(x) #x
#define XKSM_STR(x) XKSM_STR2(x)

const char *xksm_version(void)
{
    return XKSM_STR(XKSM_VERSION_MAJOR) "." XKSM_STR(XKSM_VERSION_MINOR) "." XKSM_STR(
        XKSM_VERSION_PATCH);
}

xksm_status xksm__fail(xksm *x, xksm_status s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->err, sizeof x->err, fmt, ap);
    va_end(ap);
    return s;
}

xksm_status xksm_open(const char *path, xksm **out)
{
    if (!out)
        return XKSM_EINVAL;
    *out = NULL;
    if (!path)
        return XKSM_EINVAL;

    xksm *x = calloc(1, sizeof *x);
    if (!x)
        return XKSM_ENOMEM;
    *out = x;

    int rc = sqlite3_open_v2(path, &x->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK)
        return xksm__fail(x, XKSM_EIO, "open %s: %s", path,
                          x->db ? sqlite3_errmsg(x->db) : sqlite3_errstr(rc));

    char *err = NULL;
    if (sqlite3_exec(x->db,
                     "PRAGMA journal_mode = WAL;"
                     "PRAGMA synchronous = NORMAL;"
                     "PRAGMA foreign_keys = ON;"
                     "PRAGMA secure_delete = ON;",
                     NULL, NULL, &err) != SQLITE_OK) {
        xksm_status s = xksm__fail(x, XKSM_EIO, "pragmas: %s", err ? err : "unknown");
        sqlite3_free(err);
        return s;
    }
    return xksm__migrate(x);
}

void xksm_close(xksm *x)
{
    if (!x)
        return;
    sqlite3_close(x->db);
    free(x);
}

int xksm_schema_version(const xksm *x)
{
    return x ? x->schema : -1;
}

const char *xksm_errmsg(const xksm *x)
{
    return x ? x->err : "null handle";
}

int64_t xksm__now_us(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
        return 0;
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
}
