/*
 * search.h — substring search engine.
 *
 * Compiles a pattern once into a Boyer-Moore-Horspool skip table and reuses
 * it across all chunks/threads (the table is read-only after construction,
 * so it is shared without locking). For very short patterns (< 4 bytes) we
 * fall back to memchr/strncmp loops, which are typically faster than BMH
 * setup overhead.
 */
#ifndef AMBIL_SEARCH_H
#define AMBIL_SEARCH_H

#include <stddef.h>

typedef struct {
    const unsigned char *pat;   /* lowercased copy if ignore_case else original */
    size_t               plen;
    int                  ignore_case;
    size_t               skip[256];
    unsigned char       *owned;  /* lowercased buffer, freed by lf_search_free */
} lf_searcher_t;

/*
 * Build a searcher. Returns 0 on success. On case-insensitive mode the pattern
 * is folded to lowercase once; haystack bytes are folded inline during scan.
 * Empty pattern is rejected (callers should treat "no pattern" as no filter).
 */
int  lf_search_init(lf_searcher_t *s, const char *pattern, int ignore_case);
void lf_search_free(lf_searcher_t *s);

/*
 * Scan [hay, hay+hlen) for the first occurrence of the pattern.
 * Returns pointer to the match or NULL. Pure function: thread-safe across
 * concurrent calls with the same searcher.
 */
const char *lf_search_find(const lf_searcher_t *s, const char *hay, size_t hlen);

/* Span result for lf_search_find_all. */
typedef struct {
    size_t start;
    size_t end;       /* exclusive */
} lf_match_span_t;

/*
 * Find all non-overlapping matches in [hay, hay+hlen). Appends spans to
 * *spans (reallocating *cap as needed via realloc/lf_xrealloc), updates
 * *n. The caller owns *spans and must free() it. Returns count appended.
 */
size_t lf_search_find_all(const lf_searcher_t *s, const char *hay, size_t hlen,
                          lf_match_span_t **spans, size_t *n, size_t *cap);

/* Whole-word filter: returns 1 if the match at [start,end) is bordered by
 * non-word characters (or buffer edges). Word chars: [A-Za-z0-9_]. */
int lf_search_is_word_boundary(const char *hay, size_t hlen, size_t start, size_t end);

#endif /* AMBIL_SEARCH_H */
