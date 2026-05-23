/*
 * re_tiny.h — minimal embedded regex engine.
 *
 * Subset (per plan §6.1):
 *   . * + ?           dot, greedy quantifiers, optional
 *   ^ $               line anchors
 *   [abc] [a-z] [^x]  character classes (including negation, ranges)
 *   \d \D \w \W \s \S digit / word / whitespace and inversions
 *   \\ \. \*          literal escapes
 *
 * No groups, no alternation, no backreferences (those require PCRE-class
 * complexity and are explicitly out of scope). Implementation uses a small
 * pre-compiled token array (max LF_RE_MAX_OBJECTS) and a recursive backtracker.
 * No heap allocation at match time.
 *
 * Inspired by github.com/kokke/tiny-regex-c (public domain), re-implemented
 * to fit ambil's API style (lf_re_* prefix, separate compile/match steps).
 */
#ifndef AMBIL_RE_TINY_H
#define AMBIL_RE_TINY_H

#include <stddef.h>

#define LF_RE_MAX_OBJECTS    64   /* tokens per compiled pattern */
#define LF_RE_MAX_CHARCLASS 128   /* total class-body bytes across all classes in a pattern */

typedef struct lf_re_node {
    unsigned char type;
    union {
        unsigned char  ch;
        unsigned char *ccl;       /* pointer into ccl_buf (NUL-terminated body) */
    } u;
} lf_re_node_t;

typedef struct lf_re_program {
    lf_re_node_t  ops[LF_RE_MAX_OBJECTS];
    unsigned char ccl_buf[LF_RE_MAX_CHARCLASS];
    int           ignore_case;
} lf_re_program_t;

/* Compile `pattern` into `prog`. Returns 0 on success, -1 on syntax error or
 * pattern-too-complex (exceeds LF_RE_MAX_OBJECTS / LF_RE_MAX_CHARCLASS). */
int lf_re_compile(lf_re_program_t *prog, const char *pattern, int ignore_case);

/* Find first match in [text, text+len). Returns offset to match start, or -1
 * if no match. On match, *match_len is set to the matched byte length. */
int lf_re_find(const lf_re_program_t *prog, const char *text, size_t len, size_t *match_len);

#endif
