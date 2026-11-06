/* internal.h: shared private declarations of libxksm. Not installed. */
#ifndef XKSM_INTERNAL_H
#define XKSM_INTERNAL_H

#include "xksm.h"

#include <sqlite3.h>
#include <stdio.h>

#define XKSM_NSTMT 8

struct xksm {
    sqlite3 *db;
    int schema;
    char err[512];
    sqlite3_stmt *st[XKSM_NSTMT]; /* cached statements, memory/memory.c */
};

/* Record a formatted error on the handle and return status s. */
xksm_status xksm__fail(xksm *x, xksm_status s, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Current wall-clock time, unix microseconds UTC (C11 timespec_get). */
int64_t xksm__now_us(void);

/* 1 if s[0..n) is valid UTF-8 per RFC 3629 (memory/memory.c). */
int xksm__utf8_valid(const uint8_t *s, size_t n);

/* Finalize the cached statements (memory/memory.c); called by xksm_close. */
void xksm__memory_finalize(xksm *x);

/* Bring the open database to XKSM_SCHEMA_VERSION (storage/schema.c). */
xksm_status xksm__migrate(xksm *x);

#endif
