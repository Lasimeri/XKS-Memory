/* port.h: private access to a semantic port file (static whole-word vectors computed once by
 * an embedding model; see tools/port_build.c for the format). No model at run time. */
#ifndef XKSM_PORT_H
#define XKSM_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { XKSM_PORT_BUCKET_CODE = 11 };

typedef struct xksm_port xksm_port;

xksm_port *xksm__port_open(const char *path); /* NULL on failure */
void xksm__port_close(xksm_port *p);
size_t xksm__port_dim(const xksm_port *p);
size_t xksm__port_size(const xksm_port *p);

/* index of the lowercase key s[0..n), or -1 */
int64_t xksm__port_find(const xksm_port *p, const char *s, size_t n);
const float *xksm__port_vec(const xksm_port *p, size_t i); /* unit length, dim floats */
int xksm__port_bucket(const xksm_port *p, size_t i);

/* 1 if the lowercase word should never drive or receive an expansion (stop word, digits,
 * under 3 bytes) */
int xksm__port_stop(const char *w, size_t n);

#ifdef __cplusplus
}
#endif
#endif
