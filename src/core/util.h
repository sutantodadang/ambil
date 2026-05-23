/*
 * util.h — small helpers: ascii case ops, dynamic buffer.
 */
#ifndef AMBIL_UTIL_H
#define AMBIL_UTIL_H

#include "ambil.h"

#include <stddef.h>
#include <stdint.h>

/* ASCII-only fast tolower; safe on signed char input (uses unsigned cast). */
static inline unsigned char lf_tolower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}


/* Dynamic byte buffer used by workers to stage their output.
 * If `stream` is set, appends flush directly to stdout under the global
 * stream mutex instead of growing the buffer. Used by --stream mode to cap
 * memory at the cost of cross-file output ordering. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    stream;
} lf_buf_t;

/* Global mutex held while streaming bufs write to stdout. */
#include <pthread.h>
extern pthread_mutex_t lf_stream_mu;

void lf_buf_init(lf_buf_t *b);
void lf_buf_free(lf_buf_t *b);
/* Append `n` bytes; aborts process on allocation failure (fatal). */
void lf_buf_append(lf_buf_t *b, const char *p, size_t n);

/* Reserve at least `n` bytes of capacity in one allocation. No-op if cap >= n. */
void lf_buf_reserve(lf_buf_t *b, size_t n);

/* Detect logical CPU count (sysconf), clamped to [1, 1024]. */
int lf_detect_cpus(void);

/* Print formatted message to stderr, then abort with code 2. */
void lf_die(const char *fmt, ...);

/* xmalloc/xrealloc/xstrdup: abort on failure via lf_die. */
void *lf_xmalloc(size_t n);
void *lf_xcalloc(size_t nmemb, size_t sz);
void *lf_xrealloc(void *p, size_t n);
char *lf_xstrdup(const char *s);

/* True if `path` ends with `suffix` (case-insensitive ASCII). */
int lf_path_has_ext_ci(const char *path, const char *ext);

/* Basename pointer (no allocation). Returns ptr into `path`. */
const char *lf_path_basename(const char *path);

/* Cross-platform path separator awareness: returns 1 if c is '/' or '\\'. */
static inline int lf_is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* Set up SIGINT/SIGTERM handler that sets the global lf_cancelled flag.
 * Call once early in main(). On repeated signals, falls back to _exit(1). */
void lf_setup_cancel_signal(void);

#endif /* AMBIL_UTIL_H */
