#ifndef AMBIL_SIMD_SEARCH_H
#define AMBIL_SIMD_SEARCH_H
#include <stddef.h>

/*
 * SIMD substring finders.
 *
 * Two function-pointer families:
 *   CS  — case-sensitive: pattern stored as-is.
 *   CI  — case-insensitive: pattern lowercased in caller; haystack folded
 *         to lowercase on the fly via ASCII A-Z mask trick (no per-byte
 *         table lookup). Non-ASCII bytes pass through unchanged, matching
 *         the scalar lf_tolower() semantics.
 *
 * Both families require plen >= 4 and use BMH skip-table tails. For pat < 4
 * bytes the caller stays on the memchr path. For pat > 32 bytes the caller
 * may route through libc memmem (when available) instead.
 */
typedef const char *(*lf_simd_find_fn)(const unsigned char *pat, size_t plen,
                                       const unsigned char *hay, size_t hlen,
                                       const size_t skip[256]);

/* Probe CPU once at startup; sets the dispatch function pointers. Idempotent. */
void lf_search_runtime_init(void);

/* Case-sensitive finder. NULL = SIMD disabled, caller falls back to scalar. */
lf_simd_find_fn lf_simd_get_finder(void);

/* Case-insensitive finder (pattern must already be lowercased). NULL = no SIMD. */
lf_simd_find_fn lf_simd_get_finder_ci(void);

/* "avx2" | "sse2" | "neon" | "none" — for --simd-info introspection. */
const char *lf_simd_active_name(void);

#endif
