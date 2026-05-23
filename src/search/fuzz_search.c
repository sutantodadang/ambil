/*
 * fuzz_search.c — libFuzzer entry point for the search engine.
 *
 * Exercises both fixed-string (BMH/SIMD) and regex (re_tiny) paths against
 * arbitrary inputs. Splits the corpus into [pattern \0 haystack]; if no NUL
 * is present the whole input is treated as the haystack and a fixed default
 * pattern is used.
 *
 * Build (clang + libFuzzer):
 *   make fuzz                # outputs build/ambil-fuzz
 *   ./build/ambil-fuzz corpus/  -max_total_time=60
 *
 * GCC / MinGW lack libFuzzer; the harness still compiles but cannot run.
 * Production fuzz runs should use clang on Linux.
 */
#include "search.h"
#include "re_tiny.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;

    /* Find NUL separator (if any) to split pattern from haystack. */
    const uint8_t *sep = (const uint8_t *)memchr(data, 0, size);
    const char *pat;
    char pat_buf[256];
    const char *hay;
    size_t hlen;

    if (sep && sep != data) {
        size_t plen = (size_t)(sep - data);
        if (plen >= sizeof(pat_buf)) plen = sizeof(pat_buf) - 1;
        memcpy(pat_buf, data, plen);
        pat_buf[plen] = '\0';
        pat = pat_buf;
        hay = (const char *)(sep + 1);
        hlen = size - plen - 1;
    } else {
        pat = "ab";
        hay = (const char *)data;
        hlen = size;
    }

    /* Fixed-string path (both case modes). */
    {
        lf_searcher_t s;
        if (lf_search_init(&s, pat, 0, LF_SEARCH_FIXED) == 0) {
            lf_match_span_t *spans = NULL;
            size_t n = 0, cap = 0;
            (void)lf_search_find_all(&s, hay, hlen, &spans, &n, &cap);
            free(spans);
            lf_search_free(&s);
        }
        if (lf_search_init(&s, pat, 1, LF_SEARCH_FIXED) == 0) {
            (void)lf_search_find(&s, hay, hlen);
            lf_search_free(&s);
        }
    }
    /* Regex path. */
    {
        lf_searcher_t s;
        if (lf_search_init(&s, pat, 0, LF_SEARCH_REGEX) == 0) {
            (void)lf_search_find(&s, hay, hlen);
            lf_search_free(&s);
        }
    }
    return 0;
}
