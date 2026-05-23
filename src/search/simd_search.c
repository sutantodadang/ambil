/*
 * simd_search.c — SIMD-accelerated substring search (case-sensitive + case-insensitive).
 *
 * Architecture dispatch:
 *   x86-64: prefer AVX2 (32-byte chunks), fall back to SSE2 (16-byte).
 *   AArch64/NEON: 16-byte chunks via vceqq_u8.
 *   Everything else: NULL finder (caller falls through to scalar BMH).
 *
 * Each SIMD path matches on the last byte of the pattern (BMH anchor),
 * then confirms with memcmp (or per-byte tolower compare in the CI path).
 * Tail bytes that cannot form a full 16/32-byte aligned window are handled
 * by a scalar BMH scan.
 *
 * Case-insensitive paths fold haystack bytes via the classic ASCII trick:
 *   is_upper = (b >= 'A') & (b <= 'Z')
 *   lower    = b | (is_upper & 0x20)
 * This avoids per-byte table lookups and works for the ASCII subset, which
 * matches lf_tolower()'s semantics. Non-ASCII bytes pass through unchanged.
 */
#include "simd_search.h"
#include "util.h"

#include <string.h>

/* ---- tolower-aware compare for confirmation in CI paths ------------------ */

static int memcmp_ci(const unsigned char *a, const unsigned char *b_lower, size_t n) {
    /* b_lower is already lowercased; fold a's ASCII letters on the fly. */
    for (size_t i = 0; i < n; i++) {
        unsigned char ac = a[i];
        if (ac >= 'A' && ac <= 'Z') ac = (unsigned char)(ac + 32);
        if (ac != b_lower[i]) return 1;
    }
    return 0;
}

/* ---- scalar BMH tails ---------------------------------------------------- */

static const char *simd_tail_scalar(const unsigned char *pat, size_t plen,
                                    const unsigned char *hay, size_t hlen,
                                    const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last = hlen - plen;
    size_t i = 0;
    while (i <= last) {
        unsigned char c = hay[i + plen - 1];
        if (c == pat[plen - 1] && memcmp(hay + i, pat, plen - 1) == 0)
            return (const char *)(hay + i);
        i += skip[c];
    }
    return NULL;
}

static const char *simd_tail_scalar_ci(const unsigned char *pat, size_t plen,
                                       const unsigned char *hay, size_t hlen,
                                       const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last = hlen - plen;
    size_t i = 0;
    while (i <= last) {
        unsigned char c = hay[i + plen - 1];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c == pat[plen - 1] && memcmp_ci(hay + i, pat, plen - 1) == 0)
            return (const char *)(hay + i);
        i += skip[c];
    }
    return NULL;
}

/* =========================================================================
 *  x86 SSE2 / AVX2
 * ========================================================================= */

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>

/* ---- SSE2 case-sensitive (16-byte chunks) ------------------------------- */

static const char *lf_simd_find_sse2(const unsigned char *pat, size_t plen,
                                     const unsigned char *hay, size_t hlen,
                                     const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last_start = hlen - plen;
    __m128i needle = _mm_set1_epi8((char)pat[plen - 1]);

    size_t i = 0;
    while (i + 16 <= hlen - plen + 1) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(hay + i + plen - 1));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, needle));
        while (mask) {
            int off = __builtin_ctz((unsigned)mask);
            size_t pos = i + (size_t)off;
            if (pos <= last_start && memcmp(hay + pos, pat, plen - 1) == 0)
                return (const char *)(hay + pos);
            mask &= mask - 1;
        }
        i += 16;
    }
    if (i <= last_start)
        return simd_tail_scalar(pat, plen, hay + i, hlen - i, skip);
    return NULL;
}

/* ---- SSE2 case-insensitive (16-byte chunks) ----------------------------- */

static inline __m128i fold_lower_sse2(__m128i v) {
    /* Signed cmp is fine: ASCII letters live in [0, 127]. High-bit bytes are
     * negative under cmpgt_epi8 and harmlessly fail both range checks. */
    __m128i a_minus_1 = _mm_set1_epi8('A' - 1); /* 64 */
    __m128i z_plus_1  = _mm_set1_epi8('Z' + 1); /* 91 */
    __m128i ge_a      = _mm_cmpgt_epi8(v, a_minus_1);
    __m128i lt_zp1    = _mm_cmpgt_epi8(z_plus_1, v);
    __m128i is_upper  = _mm_and_si128(ge_a, lt_zp1);
    __m128i offset    = _mm_and_si128(is_upper, _mm_set1_epi8(0x20));
    return _mm_or_si128(v, offset);
}

static const char *lf_simd_find_ci_sse2(const unsigned char *pat, size_t plen,
                                        const unsigned char *hay, size_t hlen,
                                        const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last_start = hlen - plen;
    __m128i needle = _mm_set1_epi8((char)pat[plen - 1]); /* pat is already lower */

    size_t i = 0;
    while (i + 16 <= hlen - plen + 1) {
        __m128i chunk  = _mm_loadu_si128((const __m128i *)(hay + i + plen - 1));
        __m128i folded = fold_lower_sse2(chunk);
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(folded, needle));
        while (mask) {
            int off = __builtin_ctz((unsigned)mask);
            size_t pos = i + (size_t)off;
            if (pos <= last_start && memcmp_ci(hay + pos, pat, plen - 1) == 0)
                return (const char *)(hay + pos);
            mask &= mask - 1;
        }
        i += 16;
    }
    if (i <= last_start)
        return simd_tail_scalar_ci(pat, plen, hay + i, hlen - i, skip);
    return NULL;
}

/* ---- AVX2 (32-byte chunks; compiled with target attribute) -------------- */

__attribute__((target("avx2")))
static const char *lf_simd_find_avx2(const unsigned char *pat, size_t plen,
                                     const unsigned char *hay, size_t hlen,
                                     const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last_start = hlen - plen;
    __m256i needle = _mm256_set1_epi8((char)pat[plen - 1]);

    size_t i = 0;
    while (i + 32 <= hlen - plen + 1) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(hay + i + plen - 1));
        unsigned int mask = (unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, needle));
        while (mask) {
            int off = __builtin_ctz(mask);
            size_t pos = i + (size_t)off;
            if (pos <= last_start && memcmp(hay + pos, pat, plen - 1) == 0)
                return (const char *)(hay + pos);
            mask &= mask - 1;
        }
        i += 32;
    }
    if (i <= last_start)
        return simd_tail_scalar(pat, plen, hay + i, hlen - i, skip);
    return NULL;
}

__attribute__((target("avx2")))
static inline __m256i fold_lower_avx2(__m256i v) {
    __m256i a_minus_1 = _mm256_set1_epi8('A' - 1);
    __m256i z_plus_1  = _mm256_set1_epi8('Z' + 1);
    __m256i ge_a      = _mm256_cmpgt_epi8(v, a_minus_1);
    __m256i lt_zp1    = _mm256_cmpgt_epi8(z_plus_1, v);
    __m256i is_upper  = _mm256_and_si256(ge_a, lt_zp1);
    __m256i offset    = _mm256_and_si256(is_upper, _mm256_set1_epi8(0x20));
    return _mm256_or_si256(v, offset);
}

__attribute__((target("avx2")))
static const char *lf_simd_find_ci_avx2(const unsigned char *pat, size_t plen,
                                        const unsigned char *hay, size_t hlen,
                                        const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last_start = hlen - plen;
    __m256i needle = _mm256_set1_epi8((char)pat[plen - 1]);

    size_t i = 0;
    while (i + 32 <= hlen - plen + 1) {
        __m256i chunk  = _mm256_loadu_si256((const __m256i *)(hay + i + plen - 1));
        __m256i folded = fold_lower_avx2(chunk);
        unsigned int mask = (unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(folded, needle));
        while (mask) {
            int off = __builtin_ctz(mask);
            size_t pos = i + (size_t)off;
            if (pos <= last_start && memcmp_ci(hay + pos, pat, plen - 1) == 0)
                return (const char *)(hay + pos);
            mask &= mask - 1;
        }
        i += 32;
    }
    if (i <= last_start)
        return simd_tail_scalar_ci(pat, plen, hay + i, hlen - i, skip);
    return NULL;
}

#endif /* x86 */

/* =========================================================================
 *  AArch64 / ARM NEON (16-byte chunks)
 * ========================================================================= */

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>

static const char *lf_simd_find_neon(const unsigned char *pat, size_t plen,
                                     const unsigned char *hay, size_t hlen,
                                     const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last_start = hlen - plen;
    uint8x16_t needle = vdupq_n_u8(pat[plen - 1]);

    size_t i = 0;
    while (i + 16 <= hlen - plen + 1) {
        uint8x16_t chunk = vld1q_u8(hay + i + plen - 1);
        uint8x16_t cmp   = vceqq_u8(chunk, needle);
        if (vmaxvq_u8(cmp) != 0) {
            uint8_t lanes[16];
            vst1q_u8(lanes, cmp);
            for (int k = 0; k < 16; k++) {
                if (lanes[k]) {
                    size_t pos = i + (size_t)k;
                    if (pos <= last_start && memcmp(hay + pos, pat, plen - 1) == 0)
                        return (const char *)(hay + pos);
                }
            }
        }
        i += 16;
    }
    if (i <= last_start)
        return simd_tail_scalar(pat, plen, hay + i, hlen - i, skip);
    return NULL;
}

static inline uint8x16_t fold_lower_neon(uint8x16_t v) {
    uint8x16_t a    = vdupq_n_u8('A');
    uint8x16_t z1   = vdupq_n_u8('Z' + 1);
    uint8x16_t ge_a = vcgeq_u8(v, a);
    uint8x16_t lt_z = vcltq_u8(v, z1);
    uint8x16_t is_u = vandq_u8(ge_a, lt_z);
    uint8x16_t off  = vandq_u8(is_u, vdupq_n_u8(0x20));
    return vorrq_u8(v, off);
}

static const char *lf_simd_find_ci_neon(const unsigned char *pat, size_t plen,
                                        const unsigned char *hay, size_t hlen,
                                        const size_t skip[256]) {
    if (plen > hlen) return NULL;
    size_t last_start = hlen - plen;
    uint8x16_t needle = vdupq_n_u8(pat[plen - 1]);

    size_t i = 0;
    while (i + 16 <= hlen - plen + 1) {
        uint8x16_t chunk  = vld1q_u8(hay + i + plen - 1);
        uint8x16_t folded = fold_lower_neon(chunk);
        uint8x16_t cmp    = vceqq_u8(folded, needle);
        if (vmaxvq_u8(cmp) != 0) {
            uint8_t lanes[16];
            vst1q_u8(lanes, cmp);
            for (int k = 0; k < 16; k++) {
                if (lanes[k]) {
                    size_t pos = i + (size_t)k;
                    if (pos <= last_start && memcmp_ci(hay + pos, pat, plen - 1) == 0)
                        return (const char *)(hay + pos);
                }
            }
        }
        i += 16;
    }
    if (i <= last_start)
        return simd_tail_scalar_ci(pat, plen, hay + i, hlen - i, skip);
    return NULL;
}

#endif /* NEON */

/* =========================================================================
 *  runtime dispatch
 * ========================================================================= */

static lf_simd_find_fn g_simd_finder    = NULL;
static lf_simd_find_fn g_simd_finder_ci = NULL;
static const char     *g_simd_name      = "none";

void lf_search_runtime_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        g_simd_finder    = lf_simd_find_avx2;
        g_simd_finder_ci = lf_simd_find_ci_avx2;
        g_simd_name      = "avx2";
    } else if (__builtin_cpu_supports("sse2")) {
        g_simd_finder    = lf_simd_find_sse2;
        g_simd_finder_ci = lf_simd_find_ci_sse2;
        g_simd_name      = "sse2";
    }
#elif defined(__aarch64__) || defined(__ARM_NEON)
    g_simd_finder    = lf_simd_find_neon;
    g_simd_finder_ci = lf_simd_find_ci_neon;
    g_simd_name      = "neon";
#endif
}

lf_simd_find_fn lf_simd_get_finder(void)    { return g_simd_finder; }
lf_simd_find_fn lf_simd_get_finder_ci(void) { return g_simd_finder_ci; }
const char     *lf_simd_active_name(void)   { return g_simd_name; }
