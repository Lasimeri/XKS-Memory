/* xksm: command-line front end to libxksm. */
#include "xksm.h"

#include <stdio.h>
#include <string.h>

static int usage(void)
{
    fputs("usage: xksm init <store>     create or migrate a store\n"
          "       xksm info <store>     print library and schema version\n"
          "       xksm version\n",
          stderr);
    return 2;
}

static int open_or_report(const char *path, xksm **x)
{
    xksm_status s = xksm_open(path, x);
    if (s != XKSM_OK) {
        fprintf(stderr, "xksm: %s (status %d)\n", xksm_errmsg(*x), (int)s);
        xksm_close(*x);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "version")) {
        printf("xksm %s, schema %d\n", xksm_version(), XKSM_SCHEMA_VERSION);
        return 0;
    }
    if (argc != 3)
        return usage();

    xksm *x = NULL;
    if (!strcmp(argv[1], "init") || !strcmp(argv[1], "info")) {
        if (open_or_report(argv[2], &x))
            return 1;
        printf("store %s: schema %d (library %s)\n", argv[2], xksm_schema_version(x),
               xksm_version());
        xksm_close(x);
        return 0;
    }
    return usage();
}
