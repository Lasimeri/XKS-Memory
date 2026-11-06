/* test.h: minimal test harness. CHECK records failures and keeps going. */
#ifndef XKSM_TEST_H
#define XKSM_TEST_H

#include "xksm.h"

#include <stdio.h>

static int test_checks, test_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++test_checks;                                                           \
        if (!(cond)) {                                                           \
            ++test_failures;                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static inline int test_report(const char *name)
{
    printf("%s: %d/%d checks passed\n", name, test_checks - test_failures, test_checks);
    return test_failures != 0;
}

#endif
