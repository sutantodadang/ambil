/*
 * search.c — Boyer-Moore-Horspool substring search with case-insensitive mode.
 *
 * Why BMH (and not full BM, KMP, or two-way):
 *   - BMH has a tiny preprocessing cost (256-entry table) and excellent average
 *     case for ASCII log data (sublinear in haystack length for typical
 *     patterns of length >= 4).
 *   - libc's memmem is great when available, but is glibc-only and lacks an
 *     ignore-case variant; rolling our own keeps both paths uniform.
 *
 * For patterns shorter than 4 bytes we use a memchr-anchored loop because
 * the skip table can't beat memchr's hand-tuned SIMD in libc.
 */
#include "search.h"
#include "simd_search.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* TODO: Phase 1.3 — SWAR 64-bit haszero scan for 1-3 byte patterns. Current
 *       memchr+memcmp loop already beats SWAR on most libcs; keep as future. */
/* TODO: Phase 1.4 — Two-Way (Crochemore) for patterns > 32 bytes. SIMD BMH
 *       handles common cases well; Two-Way mainly guards pathological inputs
 *       (highly repetitive haystacks). Wrap libc memmem when available. */
int lf_search_init(lf_searcher_t *s, const char *pattern, int ignore_case, lf_search_kind_t kind) {
    if (!s || !pattern) return -1;
    size_t n = strlen(pattern);
    if (n == 0) return -1;

    memset(s, 0, sizeof(*s));
    s->kind        = kind;
    s->plen        = n;
    s->ignore_case = ignore_case;

    if (kind == LF_SEARCH_REGEX) {
        /* The matcher does case folding per character via re_tiny's `ignore_case`. */
        if (lf_re_compile(&s->re, pattern, ignore_case) != 0) return -1;
        s->pat = (const unsigned char *)pattern;
        return 0;
    }

    if (ignore_case) {
        s->owned = (unsigned char *)malloc(n);
        if (!s->owned) return -1;
        for (size_t i = 0; i < n; i++) {
            s->owned[i] = lf_tolower((unsigned char)pattern[i]);
        }
        s->pat = s->owned;
    } else {
        s->pat = (const unsigned char *)pattern;
    }

    /* BMH skip table: default skip = pattern length; for each char in
     * pattern[0..n-2] override with distance to the end. */
    for (int i = 0; i < 256; i++) s->skip[i] = n;
    for (size_t i = 0; i + 1 < n; i++) s->skip[s->pat[i]] = n - 1 - i;

    return 0;
}

void lf_search_free(lf_searcher_t *s) {
    if (!s) return;
    free(s->owned);
    s->owned = NULL;
    s->pat = NULL;
}

/* Case-sensitive scan via libc memchr + memcmp anchor. */
static const char *find_cs(const lf_searcher_t *s, const char *hay, size_t hlen) {
    if (s->plen > hlen) return NULL;
    if (s->plen == 1) {
        return (const char *)memchr(hay, s->pat[0], hlen);
    }

    const unsigned char *h = (const unsigned char *)hay;
    size_t n = s->plen;
    size_t i = 0;
    size_t last = hlen - n;
    /* Short patterns: memchr-anchored confirm. Beats BMH in microbench. */
    if (n < 4) {
        const unsigned char first = s->pat[0];
        while (i <= last) {
            const unsigned char *p = (const unsigned char *)memchr(h + i, first, hlen - i - n + 1);
            if (!p) return NULL;
            if (memcmp(p, s->pat, n) == 0) return (const char *)p;
            i = (size_t)(p - h) + 1;
        }
        return NULL;
    }
    /* SIMD dispatch: faster last-byte scan + memcmp confirm. */
    lf_simd_find_fn fn = lf_simd_get_finder();
    if (fn) return fn(s->pat, n, h, hlen, s->skip);

    /* Boyer-Moore-Horspool. */
    while (i <= last) {
        const unsigned char c = h[i + n - 1];
        if (c == s->pat[n - 1] && memcmp(h + i, s->pat, n - 1) == 0) {
            return (const char *)(h + i);
        }
        i += s->skip[c];
    }
    return NULL;
}

/* Case-insensitive: SIMD fold-and-compare when available, scalar BMH otherwise.
 * The pattern is already lowercased by lf_search_init; haystack bytes are
 * folded inline (per-byte in scalar, vectorized in SIMD via fold_lower_*). */
static const char *find_ci(const lf_searcher_t *s, const char *hay, size_t hlen) {
    if (s->plen > hlen) return NULL;
    const unsigned char *h = (const unsigned char *)hay;
    size_t n = s->plen;

    /* SIMD dispatch (pat >= 4 bytes): fold-and-compare per chunk. */
    if (n >= 4) {
        lf_simd_find_fn fn = lf_simd_get_finder_ci();
        if (fn) return fn(s->pat, n, h, hlen, s->skip);
    }

    size_t last = hlen - n;
    size_t i = 0;
    while (i <= last) {
        const unsigned char c = lf_tolower(h[i + n - 1]);
        if (c == s->pat[n - 1]) {
            size_t k = 0;
            for (; k < n - 1; k++) {
                if (lf_tolower(h[i + k]) != s->pat[k]) break;
            }
            if (k == n - 1) return (const char *)(h + i);
        }
        i += s->skip[c];
    }
    return NULL;
}

const char *lf_search_find(const lf_searcher_t *s, const char *hay, size_t hlen) {
    if (s->kind == LF_SEARCH_REGEX) {
        size_t mlen;
        int off = lf_re_find(&s->re, hay, hlen, &mlen);
        return off < 0 ? NULL : hay + off;
    }
    return s->ignore_case ? find_ci(s, hay, hlen) : find_cs(s, hay, hlen);
}

#include "util.h"

size_t lf_search_find_all(const lf_searcher_t *s, const char *hay, size_t hlen,
                          lf_match_span_t **spans, size_t *n, size_t *cap) {
    size_t added = 0;
    size_t off = 0;
    while (off < hlen) {
        size_t st, mlen;
        if (s->kind == LF_SEARCH_REGEX) {
            size_t mln;
            int rel = lf_re_find(&s->re, hay + off, hlen - off, &mln);
            if (rel < 0) break;
            st   = off + (size_t)rel;
            mlen = mln > 0 ? mln : 1; /* zero-width avoids infinite loop */
        } else {
            const char *m = lf_search_find(s, hay + off, hlen - off);
            if (!m) break;
            st   = (size_t)(m - hay);
            mlen = s->plen;
        }
        if (*n == *cap) {
            size_t nc = *cap ? *cap * 2 : 8;
            *spans = (lf_match_span_t *)lf_xrealloc(*spans, nc * sizeof(lf_match_span_t));
            *cap = nc;
        }
        (*spans)[*n].start = st;
        (*spans)[*n].end   = st + mlen;
        (*n)++;
        added++;
        off = st + mlen;
    }
    return added;
}

static int is_word_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_';
}

int lf_search_is_word_boundary(const char *hay, size_t hlen, size_t start, size_t end) {
    int left  = (start == 0)    || !is_word_char((unsigned char)hay[start - 1]);
    int right = (end   >= hlen) || !is_word_char((unsigned char)hay[end]);
    return left && right;
}
