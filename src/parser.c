/*
 * parser.c — JSON token scanner.
 *
 * The scanner is a small state machine that walks the line once. For every
 * top-level `"name"` it sees, it compares against the wanted key; on match
 * it emits the value span. On miss it skips the value and continues. This
 * keeps the work proportional to the bytes BEFORE the matched key, not the
 * full line — important for wide log records.
 */
#include "parser.h"

#include <string.h>

/* Skip whitespace; returns new index. */
static inline size_t skip_ws(const char *s, size_t i, size_t n) {
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') i++;
        else break;
    }
    return i;
}

/*
 * Walk past a JSON string starting at i (s[i] must be '"'). Honors backslash
 * escapes (so \" doesn't end the string). Returns index after the closing
 * quote, or n on malformed input.
 */
static size_t skip_string(const char *s, size_t i, size_t n) {
    if (i >= n || s[i] != '"') return n;
    i++;
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            i += 2;       /* skip escape pair; tolerates trailing \ at EOF */
            continue;
        }
        if (c == '"') return i + 1;
        i++;
    }
    return n;
}

/* Walk past a balanced {...} or [...] block. Returns index after closer. */
static size_t skip_container(const char *s, size_t i, size_t n) {
    if (i >= n) return n;
    int depth  = 1;
    i++;
    while (i < n && depth > 0) {
        char c = s[i];
        if (c == '"') {
            i = skip_string(s, i, n);
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        i++;
    }
    return i;
}

/* Walk past a primitive (number, true, false, null) value. */
static size_t skip_primitive(const char *s, size_t i, size_t n, size_t *vstart, size_t *vlen) {
    size_t start = i;
    while (i < n) {
        char c = s[i];
        if (c == ',' || c == '}' || c == ']' ||
            c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
        i++;
    }
    if (vstart) *vstart = start;
    if (vlen)   *vlen   = i - start;
    return i;
}

/* Decode a single \uXXXX hex quad. Returns -1 on bad hex. */
static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

static inline char ascii_fold(char c, int ci) {
    if (!ci) return c;
    unsigned char u = (unsigned char)c;
    if (u >= 'A' && u <= 'Z') return (char)(u + 32);
    return c;
}

/* Decode the next codepoint from a JSON string body and emit its UTF-8
 * encoding into out[]. Returns bytes written, or -1 on malformed escape.
 * Advances *ip past the consumed input. */
static int decode_one(const char *raw, size_t rlen, size_t *ip,
                      char out[4]) {
    size_t i = *ip;
    if (i >= rlen) return -1;
    unsigned char c = (unsigned char)raw[i];
    if (c != '\\') {
        out[0] = (char)c;
        *ip = i + 1;
        return 1;
    }
    if (i + 1 >= rlen) return -1;
    char esc = raw[i + 1];
    switch (esc) {
        case '"': out[0] = '"';  *ip = i + 2; return 1;
        case '\\':out[0] = '\\'; *ip = i + 2; return 1;
        case '/': out[0] = '/';  *ip = i + 2; return 1;
        case 'b': out[0] = '\b'; *ip = i + 2; return 1;
        case 'f': out[0] = '\f'; *ip = i + 2; return 1;
        case 'n': out[0] = '\n'; *ip = i + 2; return 1;
        case 'r': out[0] = '\r'; *ip = i + 2; return 1;
        case 't': out[0] = '\t'; *ip = i + 2; return 1;
        case 'u': {
            if (i + 6 > rlen) return -1;
            int cp = hex4(raw + i + 2);
            if (cp < 0) return -1;
            *ip = i + 6;
            /* Handle surrogate pair if high surrogate. */
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (*ip + 6 > rlen || raw[*ip] != '\\' || raw[*ip + 1] != 'u') return -1;
                int lo = hex4(raw + *ip + 2);
                if (lo < 0xDC00 || lo > 0xDFFF) return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                *ip += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                /* Lone low surrogate — invalid. */
                return -1;
            }
            if (cp < 0x80) {
                out[0] = (char)cp; return 1;
            } else if (cp < 0x800) {
                out[0] = (char)(0xC0 | (cp >> 6));
                out[1] = (char)(0x80 | (cp & 0x3F));
                return 2;
            } else if (cp < 0x10000) {
                out[0] = (char)(0xE0 | (cp >> 12));
                out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[2] = (char)(0x80 | (cp & 0x3F));
                return 3;
            } else {
                out[0] = (char)(0xF0 | (cp >> 18));
                out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[3] = (char)(0x80 | (cp & 0x3F));
                return 4;
            }
        }
        default:
            return -1;
    }
}

int lf_json_str_eq(const char *raw, size_t rlen,
                   const char *lit, size_t llen,
                   int ignore_case) {
    size_t ri = 0, li = 0;
    char buf[4];
    while (ri < rlen) {
        int n = decode_one(raw, rlen, &ri, buf);
        if (n < 0) return 0;
        if (li + (size_t)n > llen) return 0;
        for (int k = 0; k < n; k++) {
            if (ascii_fold(buf[k], ignore_case) != ascii_fold(lit[li + k], ignore_case))
                return 0;
        }
        li += (size_t)n;
    }
    return li == llen;
}

int lf_json_get_field(const char *line, size_t llen,
                      const char *key,  size_t klen,
                      const char **vptr, size_t *vlen) {
    if (!line || !key || klen == 0 || llen == 0) return -1;

    size_t i = skip_ws(line, 0, llen);
    if (i >= llen || line[i] != '{') return -1;
    i++;

    while (i < llen) {
        i = skip_ws(line, i, llen);
        if (i >= llen) return -1;
        if (line[i] == '}') return -1;          /* end of object, no match */

        if (line[i] != '"') return -1;          /* malformed */
        size_t name_start = i + 1;
        size_t name_end_excl = skip_string(line, i, llen); /* index after closing " */
        if (name_end_excl > llen) return -1;
        size_t name_len = (name_end_excl - 1) - name_start;
        i = name_end_excl;

        i = skip_ws(line, i, llen);
        if (i >= llen || line[i] != ':') return -1;
        i++;
        i = skip_ws(line, i, llen);
        if (i >= llen) return -1;

        int matched = (name_len == klen) &&
                      (memcmp(line + name_start, key, klen) == 0);

        char c = line[i];
        if (c == '"') {
            size_t v_start = i + 1;
            size_t v_end_excl = skip_string(line, i, llen);
            if (v_end_excl > llen) return -1;
            size_t v_len = (v_end_excl - 1) - v_start;
            i = v_end_excl;
            if (matched) {
                *vptr = line + v_start;
                *vlen = v_len;
                return 0;
            }
        } else if (c == '{' || c == '[') {
            size_t v_start = i;
            i = skip_container(line, i, llen);
            if (matched) {
                *vptr = line + v_start;
                *vlen = i - v_start;
                return 0;
            }
        } else {
            size_t v_start, v_len;
            i = skip_primitive(line, i, llen, &v_start, &v_len);
            if (matched) {
                *vptr = line + v_start;
                *vlen = v_len;
                return 0;
            }
        }

        i = skip_ws(line, i, llen);
        if (i < llen && line[i] == ',') { i++; continue; }
        if (i < llen && line[i] == '}') return -1;
        /* Anything else: bail. */
        return -1;
    }
    return -1;
}
