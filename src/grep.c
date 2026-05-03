/*
 * grep.c — per-file search + output formatting.
 *
 * One worker pulls a file, mmaps it, and runs the entire pipeline:
 *   1. binary detection (skip unless --text)
 *   2. line iteration
 *   3. multi-pattern match union
 *   4. context buffering (-A/-B/-C)
 *   5. format into per-file output buffer (text/compact/json/count/files-with)
 *
 * The output buffer is the unit of cross-thread coordination: the dispatcher
 * collects buffers in submission order and writes them to stdout serially.
 */
#include "grep.h"
#include "binary.h"
#include "ignore.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define ANSI_HL    "\x1b[1;31m"
#define ANSI_PATH  "\x1b[1;36m"
#define ANSI_LN    "\x1b[1;33m"
#define ANSI_RESET "\x1b[0m"
#define ANSI_SEP   "\x1b[2;37m"

/* ---------- patterns ---------- */

int lf_patterns_build(lf_patterns_t *ps, const options_t *o) {
    memset(ps, 0, sizeof(*ps));
    ps->ignore_case  = o->ignore_case;
    ps->word_regexp  = o->word_regexp;
    ps->invert       = o->invert_match;
    ps->only_matching= o->only_matching;
    if (o->n_patterns == 0) return 0;
    ps->searchers = (lf_searcher_t *)lf_xcalloc(o->n_patterns, sizeof(lf_searcher_t));
    for (size_t i = 0; i < o->n_patterns; i++) {
        if (lf_search_init(&ps->searchers[i], o->patterns[i], o->ignore_case) != 0) {
            return -1;
        }
        ps->n++;
    }
    return 0;
}

void lf_patterns_free(lf_patterns_t *ps) {
    for (size_t i = 0; i < ps->n; i++) lf_search_free(&ps->searchers[i]);
    free(ps->searchers);
    memset(ps, 0, sizeof(*ps));
}

/* Find all match spans in line, union of all patterns; sorted by start.
 * Returns number of spans (0 if none). word_regexp filters spans. */
static size_t find_line_matches(const lf_patterns_t *ps,
                                const char *line, size_t llen,
                                lf_match_span_t **spans, size_t *cap) {
    size_t n = 0;
    for (size_t i = 0; i < ps->n; i++) {
        lf_search_find_all(&ps->searchers[i], line, llen, spans, &n, cap);
    }
    /* Sort by start ascending (insertion sort: usually tiny n). */
    for (size_t i = 1; i < n; i++) {
        lf_match_span_t k = (*spans)[i];
        size_t j = i;
        while (j > 0 && (*spans)[j - 1].start > k.start) {
            (*spans)[j] = (*spans)[j - 1]; j--;
        }
        (*spans)[j] = k;
    }
    /* Word boundary filter. */
    if (ps->word_regexp && n > 0) {
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (lf_search_is_word_boundary(line, llen, (*spans)[i].start, (*spans)[i].end)) {
                (*spans)[k++] = (*spans)[i];
            }
        }
        n = k;
    }
    /* Drop overlaps (keep earliest). */
    if (n > 1) {
        size_t k = 1;
        for (size_t i = 1; i < n; i++) {
            if ((*spans)[i].start >= (*spans)[k - 1].end) {
                (*spans)[k++] = (*spans)[i];
            }
        }
        n = k;
    }
    return n;
}

/* ---------- JSON helpers ---------- */

static void json_escape(lf_buf_t *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  lf_buf_append(b, "\\\"", 2); break;
            case '\\': lf_buf_append(b, "\\\\", 2); break;
            case '\n': lf_buf_append(b, "\\n", 2); break;
            case '\r': lf_buf_append(b, "\\r", 2); break;
            case '\t': lf_buf_append(b, "\\t", 2); break;
            case '\b': lf_buf_append(b, "\\b", 2); break;
            case '\f': lf_buf_append(b, "\\f", 2); break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    int len = snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    lf_buf_append(b, tmp, (size_t)len);
                } else {
                    lf_buf_append(b, (const char *)&c, 1);
                }
        }
    }
}

static void buf_appendf(lf_buf_t *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        lf_buf_append(b, tmp, (size_t)n);
    } else {
        char *big = (char *)lf_xmalloc((size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        lf_buf_append(b, big, (size_t)n);
        free(big);
    }
}

/* ---------- emitters ---------- */

typedef struct {
    const options_t *opt;
    const lf_patterns_t *ps;
    const char *path;
    int color;
    int show_filename;
    lf_buf_t *out;
    int header_emitted;       /* text/compact mode: have we printed the path heading? */
    uint64_t emitted_match_lines;
    int after_remaining;      /* lines still to emit as -A context */
    uint64_t last_emitted_lineno;
    int needs_separator;      /* between non-contiguous match groups */
} emit_ctx_t;

static void emit_text_path_header(emit_ctx_t *e) {
    if (e->header_emitted) return;
    if (e->opt->mode == OUT_COMPACT) {
        if (e->color) lf_buf_append(e->out, ANSI_PATH, sizeof(ANSI_PATH) - 1);
        lf_buf_append(e->out, e->path, strlen(e->path));
        if (e->color) lf_buf_append(e->out, ANSI_RESET, sizeof(ANSI_RESET) - 1);
        lf_buf_append(e->out, "\n", 1);
    } else if (!e->opt->no_heading) {
        if (e->color) lf_buf_append(e->out, ANSI_PATH, sizeof(ANSI_PATH) - 1);
        lf_buf_append(e->out, e->path, strlen(e->path));
        if (e->color) lf_buf_append(e->out, ANSI_RESET, sizeof(ANSI_RESET) - 1);
        lf_buf_append(e->out, "\n", 1);
    }
    e->header_emitted = 1;
}

/* Write a colored or plain line with submatches highlighted. line excludes \n. */
static void emit_text_line(emit_ctx_t *e, uint64_t lineno,
                           const char *line, size_t llen,
                           const lf_match_span_t *spans, size_t n_spans,
                           const char *kind /* "match"|"before"|"after"|NULL */) {
    /* Group separator. */
    if (e->needs_separator && (e->opt->mode != OUT_COMPACT)) {
        if (e->color) lf_buf_append(e->out, ANSI_SEP, sizeof(ANSI_SEP) - 1);
        lf_buf_append(e->out, "--\n", 3);
        if (e->color) lf_buf_append(e->out, ANSI_RESET, sizeof(ANSI_RESET) - 1);
        e->needs_separator = 0;
    }

    int show_path_inline = e->show_filename && (e->opt->no_heading || (e->opt->mode == OUT_TEXT && 0));
    /* In no_heading mode we prefix path on each line. */
    if (e->show_filename && e->opt->no_heading) {
        if (e->color) lf_buf_append(e->out, ANSI_PATH, sizeof(ANSI_PATH) - 1);
        lf_buf_append(e->out, e->path, strlen(e->path));
        if (e->color) lf_buf_append(e->out, ANSI_RESET, sizeof(ANSI_RESET) - 1);
        lf_buf_append(e->out, ":", 1);
    }
    (void)show_path_inline;

    char sep = (kind && (strcmp(kind, "before") == 0 || strcmp(kind, "after") == 0)) ? '-' : ':';

    if (e->opt->line_numbers) {
        char nbuf[32];
        int nl = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)lineno);
        if (e->color) lf_buf_append(e->out, ANSI_LN, sizeof(ANSI_LN) - 1);
        lf_buf_append(e->out, nbuf, (size_t)nl);
        if (e->color) lf_buf_append(e->out, ANSI_RESET, sizeof(ANSI_RESET) - 1);
        lf_buf_append(e->out, &sep, 1);
    }

    /* Compact mode: trim trailing whitespace from the line. */
    size_t eff = llen;
    if (e->opt->mode == OUT_COMPACT) {
        while (eff > 0 && (line[eff - 1] == ' ' || line[eff - 1] == '\t')) eff--;
    }

    if (n_spans > 0 && e->color) {
        size_t cur = 0;
        for (size_t i = 0; i < n_spans; i++) {
            if (spans[i].start > eff) break;
            size_t end = spans[i].end > eff ? eff : spans[i].end;
            lf_buf_append(e->out, line + cur, spans[i].start - cur);
            lf_buf_append(e->out, ANSI_HL, sizeof(ANSI_HL) - 1);
            lf_buf_append(e->out, line + spans[i].start, end - spans[i].start);
            lf_buf_append(e->out, ANSI_RESET, sizeof(ANSI_RESET) - 1);
            cur = end;
        }
        if (cur < eff) lf_buf_append(e->out, line + cur, eff - cur);
    } else {
        lf_buf_append(e->out, line, eff);
    }
    lf_buf_append(e->out, "\n", 1);
}

static void emit_only_matching(emit_ctx_t *e, uint64_t lineno,
                               const char *line, size_t llen,
                               const lf_match_span_t *spans, size_t n_spans) {
    (void)llen;
    for (size_t i = 0; i < n_spans; i++) {
        if (e->show_filename && e->opt->no_heading) {
            lf_buf_append(e->out, e->path, strlen(e->path));
            lf_buf_append(e->out, ":", 1);
        }
        if (e->opt->line_numbers) {
            char nbuf[32];
            int nl = snprintf(nbuf, sizeof(nbuf), "%llu:", (unsigned long long)lineno);
            lf_buf_append(e->out, nbuf, (size_t)nl);
        }
        if (e->color) lf_buf_append(e->out, ANSI_HL, sizeof(ANSI_HL) - 1);
        lf_buf_append(e->out, line + spans[i].start, spans[i].end - spans[i].start);
        if (e->color) lf_buf_append(e->out, ANSI_RESET, sizeof(ANSI_RESET) - 1);
        lf_buf_append(e->out, "\n", 1);
    }
}

/* JSON mode emitters. */
static void json_emit_begin(emit_ctx_t *e) {
    lf_buf_append(e->out, "{\"type\":\"begin\",\"path\":\"", 24);
    json_escape(e->out, e->path, strlen(e->path));
    lf_buf_append(e->out, "\"}\n", 3);
}
static void json_emit_event(emit_ctx_t *e, const char *type, uint64_t lineno,
                            const char *line, size_t llen,
                            const lf_match_span_t *spans, size_t n_spans,
                            const char *kind) {
    buf_appendf(e->out, "{\"type\":\"%s\",\"path\":\"", type);
    json_escape(e->out, e->path, strlen(e->path));
    buf_appendf(e->out, "\",\"line_number\":%llu,\"lines\":\"", (unsigned long long)lineno);
    json_escape(e->out, line, llen);
    lf_buf_append(e->out, "\"", 1);
    if (kind) buf_appendf(e->out, ",\"kind\":\"%s\"", kind);
    if (spans && n_spans > 0) {
        lf_buf_append(e->out, ",\"submatches\":[", 15);
        for (size_t i = 0; i < n_spans; i++) {
            if (i) lf_buf_append(e->out, ",", 1);
            buf_appendf(e->out, "{\"start\":%zu,\"end\":%zu,\"text\":\"", spans[i].start, spans[i].end);
            json_escape(e->out, line + spans[i].start, spans[i].end - spans[i].start);
            lf_buf_append(e->out, "\"}", 2);
        }
        lf_buf_append(e->out, "]", 1);
    }
    lf_buf_append(e->out, "}\n", 2);
}
static void json_emit_end(emit_ctx_t *e, const lf_file_stats_t *st) {
    buf_appendf(e->out,
        "{\"type\":\"end\",\"path\":\"");
    json_escape(e->out, e->path, strlen(e->path));
    buf_appendf(e->out, "\",\"stats\":{\"matched_lines\":%llu,\"searches\":%llu}}\n",
                (unsigned long long)st->matched_lines,
                (unsigned long long)st->searches);
}

/* ---------- core scan ---------- */

/* Ring buffer of recent lines for -B context. */
typedef struct {
    const char **lptr;
    size_t      *llen;
    uint64_t    *lno;
    int          cap;
    int          n;       /* number of valid entries (<= cap) */
    int          head;    /* index of next slot to write */
} ring_t;

static void ring_push(ring_t *r, const char *p, size_t l, uint64_t no) {
    if (r->cap == 0) return;
    r->lptr[r->head] = p;
    r->llen[r->head] = l;
    r->lno[r->head]  = no;
    r->head = (r->head + 1) % r->cap;
    if (r->n < r->cap) r->n++;
}

int lf_grep_buffer(const char *data, size_t size,
                   const char *rel_path,
                   const options_t *opt, const lf_patterns_t *ps,
                   int color, lf_buf_t *out, lf_file_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->bytes_searched = size;
    stats->searches = 1;

    if (size == 0) return 0;
    if (!opt->text_mode && lf_is_binary(data, size)) return 0;

    emit_ctx_t E = {0};
    E.opt = opt;
    E.ps = ps;
    E.path = rel_path;
    E.color = color;
    E.show_filename = (opt->show_filename == 1) ? 1
                    : (opt->show_filename == 0) ? 0
                    : 1; /* auto handled by caller setting show_filename */
    E.out = out;

    /* For -l / -c we don't need per-line emission state. */
    int mode_l = opt->mode == OUT_FILES_WITH;
    int mode_c = opt->mode == OUT_COUNT;
    int mode_json = opt->mode == OUT_JSON;
    int header_pending_json = mode_json;

    /* Context. */
    int B = opt->before_context;
    int A = opt->after_context;
    ring_t ring = {0};
    if (B > 0) {
        ring.cap = B;
        ring.lptr = (const char **)lf_xcalloc((size_t)B, sizeof(*ring.lptr));
        ring.llen = (size_t *)     lf_xcalloc((size_t)B, sizeof(*ring.llen));
        ring.lno  = (uint64_t *)   lf_xcalloc((size_t)B, sizeof(*ring.lno));
    }

    lf_match_span_t *spans = NULL;
    size_t spans_cap = 0;

    /* Iterate lines. */
    uint64_t lineno = 0;
    size_t i = 0;
    while (i < size) {
        const char *nl = (const char *)memchr(data + i, '\n', size - i);
        size_t end = nl ? (size_t)(nl - data) : size;
        size_t llen = end - i;
        size_t eff_len = llen;
        if (eff_len > 0 && data[i + eff_len - 1] == '\r') eff_len--;
        const char *line = data + i;
        lineno++;

        size_t n_spans = 0;
        if (ps->n > 0) {
            n_spans = find_line_matches(ps, line, eff_len, &spans, &spans_cap);
        }
        int matched;
        if (ps->n == 0) {
            /* No patterns: only sensible with --invert? we treat as no match. */
            matched = 0;
        } else {
            matched = (n_spans > 0);
        }
        if (ps->invert) matched = !matched;

        if (matched) {
            stats->matched_lines++;
            stats->had_match = 1;

            if (mode_l) {
                if (E.show_filename || 1) lf_buf_append(out, rel_path, strlen(rel_path));
                lf_buf_append(out, "\n", 1);
                /* No need to scan further. */
                goto done;
            }
            if (mode_c) {
                /* count only — keep iterating */
            } else if (mode_json) {
                if (header_pending_json) { json_emit_begin(&E); header_pending_json = 0; }
                /* Flush before-context. */
                int avail = ring.n;
                for (int k = 0; k < avail; k++) {
                    int idx = (ring.head - avail + k + ring.cap) % ring.cap;
                    json_emit_event(&E, "context", ring.lno[idx],
                                    ring.lptr[idx], ring.llen[idx], NULL, 0, "before");
                }
                ring.n = 0;
                json_emit_event(&E, "match", lineno, line, eff_len,
                                ps->invert ? NULL : spans,
                                ps->invert ? 0 : n_spans, NULL);
                E.after_remaining = A;
            } else {
                emit_text_path_header(&E);
                /* Flush before-context. */
                int avail = ring.n;
                /* Determine if separator needed. */
                if (E.last_emitted_lineno != 0 &&
                    lineno > E.last_emitted_lineno + (uint64_t)(A + B + 1)) {
                    E.needs_separator = 1;
                }
                for (int k = 0; k < avail; k++) {
                    int idx = (ring.head - avail + k + ring.cap) % ring.cap;
                    if (ring.lno[idx] <= E.last_emitted_lineno) continue;
                    emit_text_line(&E, ring.lno[idx],
                                   ring.lptr[idx], ring.llen[idx], NULL, 0, "before");
                    E.last_emitted_lineno = ring.lno[idx];
                }
                ring.n = 0;
                if (opt->only_matching && !ps->invert) {
                    emit_only_matching(&E, lineno, line, eff_len, spans, n_spans);
                } else {
                    emit_text_line(&E, lineno, line, eff_len,
                                   ps->invert ? NULL : spans,
                                   ps->invert ? 0 : n_spans, NULL);
                }
                E.last_emitted_lineno = lineno;
                E.after_remaining = A;
            }
        } else if (E.after_remaining > 0) {
            if (mode_json) {
                json_emit_event(&E, "context", lineno, line, eff_len, NULL, 0, "after");
            } else if (!mode_c && !mode_l) {
                emit_text_line(&E, lineno, line, eff_len, NULL, 0, "after");
                E.last_emitted_lineno = lineno;
            }
            E.after_remaining--;
        } else {
            if (B > 0) ring_push(&ring, line, eff_len, lineno);
        }

        i = nl ? (size_t)(nl - data) + 1 : size;
    }

done:
    /* Mode-final emissions. */
    if (mode_c) {
        if (stats->matched_lines > 0) {
            buf_appendf(out, "%s:%llu\n", rel_path, (unsigned long long)stats->matched_lines);
        }
    } else if (mode_json && stats->had_match) {
        json_emit_end(&E, stats);
    } else if (E.header_emitted && opt->mode != OUT_COMPACT) {
        lf_buf_append(out, "\n", 1);  /* blank line between files */
    }

    free(spans);
    free(ring.lptr); free(ring.llen); free(ring.lno);
    return 0;
}

/* ---------- mmap path ---------- */

int lf_grep_path(const char *path, const char *rel_path,
                 const options_t *opt, const lf_patterns_t *ps,
                 int color, lf_buf_t *out, lf_file_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));

    /* Cheap binary-extension skip: avoids the open/mmap/sniff cost on the
     * long tail of `.png/.zip/.so/...` files that the README documents as
     * skipped. Content sniffing in lf_grep_buffer is the safety net for
     * files with neutral extensions. */
    if (!opt->text_mode && lf_ignore_is_binary_ext(path)) {
        stats->searches = 1;
        return 0;
    }

    int fd;
#ifdef _WIN32
    /* Use _wopen for non-ASCII paths. */
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        wchar_t *wp = (wchar_t *)lf_xmalloc((size_t)n * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, n);
        fd = _wopen(wp, _O_RDONLY | _O_BINARY);
        free(wp);
    }
#else
    fd = open(path, O_RDONLY);
#endif
    if (fd < 0) {
        fprintf(stderr, "ambil: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return -1;
    }
    size_t size = (size_t)st.st_size;
    if (size == 0) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        stats->searches = 1;
        return 0;
    }
#ifdef _WIN32
    HANDLE os_file = (HANDLE)_get_osfhandle(fd);
    HANDLE mapping = CreateFileMappingW(os_file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) { _close(fd); return -1; }
    const char *p = (const char *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(mapping); _close(fd); return -1; }
    int rc = lf_grep_buffer(p, size, rel_path, opt, ps, color, out, stats);
    UnmapViewOfFile(p);
    CloseHandle(mapping);
    _close(fd);
    return rc;
#else
    void *p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    (void)madvise(p, size, MADV_SEQUENTIAL);
    int rc = lf_grep_buffer((const char *)p, size, rel_path, opt, ps, color, out, stats);
    munmap(p, size);
    close(fd);
    return rc;
#endif
}
