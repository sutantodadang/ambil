/*
 * re_tiny.c — minimal regex compiler + backtracking matcher.
 *
 * Compilation walks the pattern left-to-right, emitting one token per atom.
 * Quantifiers (* + ?) are emitted as separate tokens that apply to the
 * preceding atom — the matcher recognises the pair when it sees a quantifier
 * token following an atom. Character classes [...] are stored as a
 * NUL-terminated body inside prog->ccl_buf; the op's u.ccl points into it.
 *
 * Matching is recursive-descent backtracking. Greedy quantifiers consume
 * maximally then back off. Anchors (^, $) are honoured during the outer scan.
 *
 * Inspired by github.com/kokke/tiny-regex-c (public domain).
 */
#include "re_tiny.h"
#include "util.h"

#include <ctype.h>
#include <string.h>

enum {
    OP_UNUSED = 0,
    OP_DOT,
    OP_BEGIN,
    OP_END,
    OP_QUESTIONMARK,
    OP_STAR,
    OP_PLUS,
    OP_CHAR,
    OP_CHAR_CLASS,
    OP_INV_CHAR_CLASS,
    OP_DIGIT,
    OP_NOT_DIGIT,
    OP_ALPHA,        /* \w */
    OP_NOT_ALPHA,    /* \W */
    OP_WHITESPACE,   /* \s */
    OP_NOT_WHITESPACE/* \S */
};

/* ---- character class helpers ------------------------------------------- */

static int re_is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int re_is_alpha_(unsigned char c) {
    return re_is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int re_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static unsigned char fold(unsigned char c, int ci) {
    if (ci && c >= 'A' && c <= 'Z') return (unsigned char)(c + 32);
    return c;
}

/* Match a single character against an OP_CHAR_CLASS body. Body is a sequence
 * of: either a single char, or `a-b` range; class metachars (\d \w \s and
 * their inversions) are honoured. NUL terminates. */
static int match_class(const unsigned char *ccl, unsigned char c, int ci) {
    c = fold(c, ci);
    while (*ccl) {
        if (ccl[0] == '\\' && ccl[1]) {
            unsigned char esc = ccl[1];
            int hit = 0;
            switch (esc) {
                case 'd': hit = re_is_digit(c);  break;
                case 'D': hit = !re_is_digit(c); break;
                case 'w': hit = re_is_alpha_(c);  break;
                case 'W': hit = !re_is_alpha_(c); break;
                case 's': hit = re_is_space(c);  break;
                case 'S': hit = !re_is_space(c); break;
                default:  hit = (c == fold(esc, ci)); break;
            }
            if (hit) return 1;
            ccl += 2;
        } else if (ccl[1] == '-' && ccl[2] && ccl[2] != ']') {
            unsigned char lo = fold(ccl[0], ci);
            unsigned char hi = fold(ccl[2], ci);
            if (lo > hi) { unsigned char t = lo; lo = hi; hi = t; }
            if (c >= lo && c <= hi) return 1;
            ccl += 3;
        } else {
            if (c == fold(*ccl, ci)) return 1;
            ccl++;
        }
    }
    return 0;
}

/* True if op matches a single character c. Quantifier ops never reach here. */
static int match_one(const lf_re_node_t *op, unsigned char c, int ci) {
    switch (op->type) {
        case OP_DOT:              return c != '\n';
        case OP_CHAR:             return fold(op->u.ch, ci) == fold(c, ci);
        case OP_CHAR_CLASS:       return  match_class(op->u.ccl, c, ci);
        case OP_INV_CHAR_CLASS:   return !match_class(op->u.ccl, c, ci);
        case OP_DIGIT:            return  re_is_digit(c);
        case OP_NOT_DIGIT:        return !re_is_digit(c);
        case OP_ALPHA:            return  re_is_alpha_(c);
        case OP_NOT_ALPHA:        return !re_is_alpha_(c);
        case OP_WHITESPACE:       return  re_is_space(c);
        case OP_NOT_WHITESPACE:   return !re_is_space(c);
    }
    return 0;
}

/* ---- matcher ------------------------------------------------------------ */

static int match_pattern(const lf_re_node_t *p,
                          const unsigned char *t, size_t tlen,
                          size_t *consumed, int ci);

static int match_star(const lf_re_node_t *atom, const lf_re_node_t *rest,
                       const unsigned char *t, size_t tlen,
                       size_t *consumed, int ci) {
    /* Greedy: eat max then back off. */
    size_t max = 0;
    while (max < tlen && match_one(atom, t[max], ci)) max++;
    for (size_t k = max + 1; k-- > 0; ) {
        size_t r;
        if (match_pattern(rest, t + k, tlen - k, &r, ci)) {
            *consumed = k + r;
            return 1;
        }
    }
    return 0;
}

static int match_plus(const lf_re_node_t *atom, const lf_re_node_t *rest,
                       const unsigned char *t, size_t tlen,
                       size_t *consumed, int ci) {
    size_t max = 0;
    while (max < tlen && match_one(atom, t[max], ci)) max++;
    for (size_t k = max; k >= 1; k--) {
        size_t r;
        if (match_pattern(rest, t + k, tlen - k, &r, ci)) {
            *consumed = k + r;
            return 1;
        }
    }
    return 0;
}

static int match_question(const lf_re_node_t *atom, const lf_re_node_t *rest,
                           const unsigned char *t, size_t tlen,
                           size_t *consumed, int ci) {
    /* Try with one match first (greedy), then zero. */
    if (tlen > 0 && match_one(atom, t[0], ci)) {
        size_t r;
        if (match_pattern(rest, t + 1, tlen - 1, &r, ci)) {
            *consumed = 1 + r;
            return 1;
        }
    }
    if (match_pattern(rest, t, tlen, consumed, ci)) return 1;
    return 0;
}

static int match_pattern(const lf_re_node_t *p,
                          const unsigned char *t, size_t tlen,
                          size_t *consumed, int ci) {
    if (p[0].type == OP_UNUSED) { *consumed = 0; return 1; }
    if (p[0].type == OP_END && p[1].type == OP_UNUSED) {
        *consumed = 0;
        return tlen == 0;
    }
    /* Peek next: if it's a quantifier, dispatch. */
    if (p[1].type == OP_QUESTIONMARK) return match_question(&p[0], &p[2], t, tlen, consumed, ci);
    if (p[1].type == OP_STAR)         return match_star    (&p[0], &p[2], t, tlen, consumed, ci);
    if (p[1].type == OP_PLUS)         return match_plus    (&p[0], &p[2], t, tlen, consumed, ci);

    if (tlen > 0 && match_one(&p[0], t[0], ci)) {
        size_t r;
        if (match_pattern(&p[1], t + 1, tlen - 1, &r, ci)) {
            *consumed = 1 + r;
            return 1;
        }
    }
    return 0;
}

int lf_re_find(const lf_re_program_t *prog, const char *text, size_t len, size_t *match_len) {
    const lf_re_node_t *p = prog->ops;
    const unsigned char *t = (const unsigned char *)text;
    int ci = prog->ignore_case;

    if (p[0].type == OP_BEGIN) {
        size_t r;
        if (match_pattern(&p[1], t, len, &r, ci)) { *match_len = r; return 0; }
        return -1;
    }
    for (size_t i = 0; i <= len; i++) {
        size_t r;
        if (match_pattern(p, t + i, len - i, &r, ci)) {
            *match_len = r;
            return (int)i;
        }
    }
    return -1;
}

/* ---- compiler ----------------------------------------------------------- */

int lf_re_compile(lf_re_program_t *prog, const char *pattern, int ignore_case) {
    if (!prog || !pattern) return -1;
    memset(prog, 0, sizeof(*prog));
    prog->ignore_case = ignore_case;

    int ci = 0;                /* compile-time char-class buf index */
    int oi = 0;                /* op index */
    int pi = 0;                /* pattern read index */
    unsigned char c;

    while ((c = (unsigned char)pattern[pi]) != '\0') {
        if (oi >= LF_RE_MAX_OBJECTS - 1) return -1;
        lf_re_node_t *op = &prog->ops[oi];
        switch (c) {
            case '^': op->type = OP_BEGIN; break;
            case '$': op->type = OP_END;   break;
            case '.': op->type = OP_DOT;   break;
            case '*': op->type = OP_STAR;        break;
            case '+': op->type = OP_PLUS;        break;
            case '?': op->type = OP_QUESTIONMARK; break;
            case '\\': {
                unsigned char esc = (unsigned char)pattern[pi + 1];
                if (!esc) return -1;
                pi++;
                switch (esc) {
                    case 'd': op->type = OP_DIGIT;          break;
                    case 'D': op->type = OP_NOT_DIGIT;      break;
                    case 'w': op->type = OP_ALPHA;          break;
                    case 'W': op->type = OP_NOT_ALPHA;      break;
                    case 's': op->type = OP_WHITESPACE;     break;
                    case 'S': op->type = OP_NOT_WHITESPACE; break;
                    default:  op->type = OP_CHAR; op->u.ch = esc; break;
                }
                break;
            }
            case '[': {
                int neg = 0;
                pi++; /* consume [ */
                if (pattern[pi] == '^') { neg = 1; pi++; }
                op->type = neg ? OP_INV_CHAR_CLASS : OP_CHAR_CLASS;
                op->u.ccl = &prog->ccl_buf[ci];
                while (pattern[pi] != '\0' && pattern[pi] != ']') {
                    if (ci >= LF_RE_MAX_CHARCLASS - 1) return -1;
                    if (pattern[pi] == '\\' && pattern[pi + 1] != '\0') {
                        prog->ccl_buf[ci++] = (unsigned char)pattern[pi++];
                    }
                    prog->ccl_buf[ci++] = (unsigned char)pattern[pi++];
                }
                if (pattern[pi] != ']') return -1; /* unclosed */
                if (ci >= LF_RE_MAX_CHARCLASS) return -1;
                prog->ccl_buf[ci++] = '\0';
                break;
            }
            default:
                op->type = OP_CHAR;
                op->u.ch = c;
                break;
        }
        pi++;
        oi++;
    }
    /* OP_UNUSED sentinel (already zeroed by memset). */
    return 0;
}
