/*
 * util.h — small helpers: time parsing, ascii case ops, dynamic buffer.
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

/*
 * lf_parse_time — parse a timestamp string into UTC epoch seconds.
 *
 * Accepts:
 *   - "YYYY-MM-DD HH:MM:SS"
 *   - "YYYY-MM-DDTHH:MM:SS"
 *   - "YYYY-MM-DDTHH:MM:SSZ"
 *   - "YYYY-MM-DDTHH:MM:SS+HH:MM" / "-HH:MM"
 *   - Optional fractional seconds (.fff) ignored.
 *
 * Returns 0 on success and writes epoch seconds to *out, -1 on parse failure.
 * All times without an explicit zone are interpreted as UTC (deterministic
 * for log analysis; TZ-of-the-machine semantics are a footgun).
 */
int lf_parse_time(const char *s, int64_t *out);

/*
 * lf_parse_line_time — extract a timestamp from the start of a log line.
 *
 * Scans up to `len` bytes of `line` for one of the supported timestamp
 * shapes anchored at offset 0. Returns 0 on success, -1 if no timestamp
 * is recognizable. Cheap: no allocation, single linear pass.
 */
int lf_parse_line_time(const char *line, size_t len, int64_t *out);

/* Dynamic byte buffer used by workers to stage their output. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} lf_buf_t;

void lf_buf_init(lf_buf_t *b);
void lf_buf_free(lf_buf_t *b);
/* Append `n` bytes; aborts process on allocation failure (fatal). */
void lf_buf_append(lf_buf_t *b, const char *p, size_t n);

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

#endif /* AMBIL_UTIL_H */
