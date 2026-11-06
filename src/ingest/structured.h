/* structured.h: private record extraction for JSON, JSONL and CSV imports. */
#ifndef XKSM_STRUCTURED_H
#define XKSM_STRUCTURED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { XKSM_ST_JSON = 1, XKSM_ST_JSONL = 2, XKSM_ST_CSV = 3 };
enum { XKSM_JSON_MAX_DEPTH = 256 };

typedef struct xksm_record {
    size_t src_off, src_len; /* raw bytes of the record in the source */
    size_t txt_off, txt_len; /* rendered text in the arena */
} xksm_record;

typedef struct xksm_records {
    xksm_record *v;
    size_t n, cap;
    char *arena; /* rendered text of every record, back to back */
    size_t used, acap;
    const char *err;  /* static message on a syntax error */
    size_t err_off;   /* byte offset of the error in the source */
} xksm_records;

/* Parse b[0..n) (valid UTF-8) as kind and render every record as "path: value" lines.
 * Returns 1 on success, 0 on a syntax error (err, err_off set), -1 out of memory.
 * Records that render to nothing (only nulls or empty strings) are omitted. */
int xksm__structured(const char *b, size_t n, int kind, xksm_records *out);
void xksm__records_free(xksm_records *r);

#ifdef __cplusplus
}
#endif
#endif
