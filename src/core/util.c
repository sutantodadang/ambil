/*
 * util.c — dynamic buffer, CPU detection, options init/free.
 */
#include "util.h"
#include "platform.h"

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

pthread_mutex_t lf_stream_mu = PTHREAD_MUTEX_INITIALIZER;

void lf_buf_init(lf_buf_t *b) {
    b->data   = NULL;
    b->len    = 0;
    b->cap    = 0;
    b->stream = 0;
}

void lf_buf_free(lf_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void lf_buf_reserve(lf_buf_t *b, size_t n) {
    if (n <= b->cap) return;
    char *nd = (char *)realloc(b->data, n);
    if (!nd) {
        fprintf(stderr, "ambil: out of memory (buffer reserve %zu)\n", n);
        abort();
    }
    b->data = nd;
    b->cap  = n;
}

void lf_buf_append(lf_buf_t *b, const char *p, size_t n) {
    if (n == 0) return;
    if (b->stream) {
        /* Stream mode: bypass buffer, write straight to stdout. The mutex
         * keeps individual fwrites atomic but does NOT preserve order across
         * files — this is the documented trade-off of --stream. */
        pthread_mutex_lock(&lf_stream_mu);
        fwrite(p, 1, n, stdout);
        pthread_mutex_unlock(&lf_stream_mu);
        return;
    }
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
    return lf_plat_detect_cpus();
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

void lf_options_init(grep_opts_t *o) {
    memset(o, 0, sizeof(*o));
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

void lf_options_free(grep_opts_t *o) {
    free(o->patterns);
    free(o->paths);
    free(o->globs);
    free(o->types);
    memset(o, 0, sizeof(*o));
}

static void grow_strarr(const char ***arr, size_t *n, size_t *cap, const char *v) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        *arr = (const char **)lf_xrealloc((void *)*arr, nc * sizeof(*arr[0]));
        *cap = nc;
    }
    (*arr)[(*n)++] = v;
}

void lf_options_add_pattern(grep_opts_t *o, const char *p) { grow_strarr(&o->patterns, &o->n_patterns, &o->cap_patterns, p); }
void lf_options_add_path   (grep_opts_t *o, const char *p) { grow_strarr(&o->paths,    &o->n_paths,    &o->cap_paths,    p); }
void lf_options_add_glob   (grep_opts_t *o, const char *g) { grow_strarr(&o->globs,    &o->n_globs,    &o->cap_globs,    g); }
void lf_options_add_type   (grep_opts_t *o, const char *t) { grow_strarr(&o->types,    &o->n_types,    &o->cap_types,    t); }

/* --- cancellation signal --- */

volatile sig_atomic_t lf_cancelled = 0;

static void cancel_handler(int sig)
{
    (void)sig;
    lf_cancelled = 1;
}

void lf_setup_cancel_signal(void)
{
    signal(SIGINT, cancel_handler);
#ifndef _WIN32
    signal(SIGTERM, cancel_handler);
#endif
}
