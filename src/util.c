/*
 * util.c — time parsing, dynamic buffer, CPU detection.
 *
 * Time parsing is hand-rolled (not strptime) because:
 *   1. We need a strict, fast, allocation-free path for per-line timestamps.
 *   2. timegm() is non-portable; we compute UTC epoch directly via the
 *      civil-from-days algorithm (Howard Hinnant), which is branch-light
 *      and correct for the full proleptic Gregorian range.
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ---------- dynamic buffer ---------- */

void lf_buf_init(lf_buf_t *b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

void lf_buf_free(lf_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void lf_buf_append(lf_buf_t *b, const char *p, size_t n) {
    if (n == 0) return;
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < b->len + n) {
            /* 1.5x growth: lower peak memory than 2x for log workloads. */
            size_t grown = ncap + (ncap >> 1);
            if (grown <= ncap) { /* overflow guard */
                ncap = b->len + n;
                break;
            }
            ncap = grown;
        }
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) {
            fprintf(stderr, "ambil: out of memory (buffer grow to %zu)\n", ncap);
            abort();
        }
        b->data = nd;
        b->cap  = ncap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* ---------- CPU detection ---------- */

int lf_detect_cpus(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    unsigned long n = info.dwNumberOfProcessors;
    if (n < 1) n = 1;
    if (n > 1024) n = 1024;
    return (int)n;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 1024) n = 1024;
    return (int)n;
#endif
}

/* ---------- date arithmetic ----------
 *
 * days_from_civil: Hinnant's algorithm. Returns days since 1970-01-01 for any
 * proleptic Gregorian (y,m,d). Branch-free, integer-only, exact.
 */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                 /* [0, 399] */
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u; /* [0, 365] */
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;  /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

/* Parse exactly `n` ASCII digits into *out. Returns 0 on success. */
static int parse_digits(const char *s, size_t n, unsigned *out) {
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10u + (unsigned)(s[i] - '0');
    }
    *out = v;
    return 0;
}

/*
 * core: parse "YYYY-MM-DD[ T]HH:MM:SS" starting at s. On success returns the
 * number of bytes consumed and writes UTC epoch (no zone) to *epoch_utc.
 * Caller handles fractional seconds and timezone suffix.
 */
static int parse_core(const char *s, size_t len, int64_t *epoch_utc) {
    if (len < 19) return -1;
    unsigned Y, Mo, D, H, Mi, S;
    if (parse_digits(s,      4, &Y))  return -1;
    if (s[4]  != '-')                 return -1;
    if (parse_digits(s + 5,  2, &Mo)) return -1;
    if (s[7]  != '-')                 return -1;
    if (parse_digits(s + 8,  2, &D))  return -1;
    if (s[10] != ' ' && s[10] != 'T') return -1;
    if (parse_digits(s + 11, 2, &H))  return -1;
    if (s[13] != ':')                 return -1;
    if (parse_digits(s + 14, 2, &Mi)) return -1;
    if (s[16] != ':')                 return -1;
    if (parse_digits(s + 17, 2, &S))  return -1;

    if (Mo < 1 || Mo > 12 || D < 1 || D > 31) return -1;
    if (H > 23 || Mi > 59 || S > 60)          return -1; /* allow leap second */

    int64_t days = days_from_civil((int64_t)Y, Mo, D);
    *epoch_utc = days * 86400 + (int64_t)H * 3600 + (int64_t)Mi * 60 + (int64_t)S;
    return 19;
}

int lf_parse_time(const char *s, int64_t *out) {
    if (!s) return -1;
    size_t len = strlen(s);
    int64_t base;
    int n = parse_core(s, len, &base);
    if (n < 0) return -1;

    size_t i = (size_t)n;

    /* Optional fractional seconds — accepted, value discarded. */
    if (i < len && s[i] == '.') {
        i++;
        size_t start = i;
        while (i < len && s[i] >= '0' && s[i] <= '9') i++;
        if (i == start) return -1;
    }

    int64_t offset = 0;
    if (i < len) {
        char z = s[i];
        if (z == 'Z' || z == 'z') {
            i++;
        } else if (z == '+' || z == '-') {
            if (i + 5 >= len) return -1;
            unsigned oh, om;
            if (parse_digits(s + i + 1, 2, &oh)) return -1;
            if (s[i + 3] != ':')                 return -1;
            if (parse_digits(s + i + 4, 2, &om)) return -1;
            int sign = (z == '+') ? 1 : -1;
            offset = sign * ((int64_t)oh * 3600 + (int64_t)om * 60);
            i += 6;
        } else {
            return -1;
        }
    }
    /* Trailing whitespace tolerated. */
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != len) return -1;

    *out = base - offset;
    return 0;
}

int lf_parse_line_time(const char *line, size_t len, int64_t *out) {
    int64_t base;
    int n = parse_core(line, len, &base);
    if (n < 0) return -1;
    size_t i = (size_t)n;

    if (i < len && line[i] == '.') {
        i++;
        while (i < len && line[i] >= '0' && line[i] <= '9') i++;
    }
    int64_t offset = 0;
    if (i < len) {
        char z = line[i];
        if (z == 'Z' || z == 'z') {
            /* UTC marker, no offset. */
        } else if (z == '+' || z == '-') {
            if (i + 5 < len) {
                unsigned oh, om;
                if (parse_digits(line + i + 1, 2, &oh) == 0 &&
                    line[i + 3] == ':' &&
                    parse_digits(line + i + 4, 2, &om) == 0) {
                    int sign = (z == '+') ? 1 : -1;
                    offset = sign * ((int64_t)oh * 3600 + (int64_t)om * 60);
                }
            }
        }
        /* Anything else: assume UTC, ignore — log lines often have local TZ
         * already baked in or non-standard separators after the timestamp. */
    }
    *out = base - offset;
    return 0;
}

/* ---------- error and allocation helpers ---------- */

void lf_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("ambil: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void *lf_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) lf_die("out of memory (malloc %zu)", n);
    return p;
}

void *lf_xcalloc(size_t nmemb, size_t sz) {
    void *p = calloc(nmemb ? nmemb : 1, sz ? sz : 1);
    if (!p) lf_die("out of memory (calloc %zu*%zu)", nmemb, sz);
    return p;
}

void *lf_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) lf_die("out of memory (realloc %zu)", n);
    return q;
}

char *lf_xstrdup(const char *s) {
    size_t n = strlen(s);
    char *q = (char *)lf_xmalloc(n + 1);
    memcpy(q, s, n + 1);
    return q;
}

int lf_path_has_ext_ci(const char *path, const char *ext) {
    size_t pl = strlen(path), el = strlen(ext);
    if (pl < el) return 0;
    const char *tail = path + pl - el;
    for (size_t i = 0; i < el; i++) {
        unsigned char a = lf_tolower((unsigned char)tail[i]);
        unsigned char b = lf_tolower((unsigned char)ext[i]);
        if (a != b) return 0;
    }
    return 1;
}

const char *lf_path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (lf_is_sep(*p)) base = p + 1;
    }
    return base;
}

/* ---------- options struct ---------- */

void lf_options_init(options_t *o) {
    memset(o, 0, sizeof(*o));
    o->from_ts        = INT64_MIN;
    o->to_ts          = INT64_MIN;
    o->color          = COLOR_AUTO;
    o->mode           = OUT_TEXT;
    o->fixed_strings  = 1;
    o->recursive      = 1;
    o->max_depth      = -1;
    o->line_numbers   = 1;
    o->show_filename  = -1;
    o->after_context  = 0;
    o->before_context = 0;
}

void lf_options_free(options_t *o) {
    free(o->patterns);
    free(o->paths);
    free(o->globs);
    free(o->types);
    free(o->field_key);
    memset(o, 0, sizeof(*o));
}

int lf_options_is_log_mode(const options_t *o) {
    return (o->field_key != NULL) ||
           (o->count_field != NULL) ||
           (o->from_ts != INT64_MIN) ||
           (o->to_ts != INT64_MIN) ||
           o->log_json_mode;
}

static void grow_strarr(const char ***arr, size_t *n, size_t *cap, const char *v) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        *arr = (const char **)lf_xrealloc((void *)*arr, nc * sizeof(*arr[0]));
        *cap = nc;
    }
    (*arr)[(*n)++] = v;
}

void lf_options_add_pattern(options_t *o, const char *p) { grow_strarr(&o->patterns, &o->n_patterns, &o->cap_patterns, p); }
void lf_options_add_path   (options_t *o, const char *p) { grow_strarr(&o->paths,    &o->n_paths,    &o->cap_paths,    p); }
void lf_options_add_glob   (options_t *o, const char *g) { grow_strarr(&o->globs,    &o->n_globs,    &o->cap_globs,    g); }
void lf_options_add_type   (options_t *o, const char *t) { grow_strarr(&o->types,    &o->n_types,    &o->cap_types,    t); }
