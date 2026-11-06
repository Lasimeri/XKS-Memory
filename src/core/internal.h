/* internal.h: shared private declarations of libxksm. Not installed. */
#ifndef XKSM_INTERNAL_H
#define XKSM_INTERNAL_H

#include "xksm.h"

#include <sqlite3.h>
#include <stdio.h>

struct xksm {
    sqlite3 *db;
    int schema;
    char err[512];
};

/* Record a formatted error on the handle and return status s. */
xksm_status xksm__fail(xksm *x, xksm_status s, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Bring the open database to XKSM_SCHEMA_VERSION (storage/schema.c). */
xksm_status xksm__migrate(xksm *x);

#endif
