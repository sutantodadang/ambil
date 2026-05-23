/*
 * json_emit.c — shared JSON formatting primitives.
 *
 * Extracted from grep.c (originally static json_escape / buf_appendf).
 * The grep-specific NDJSON event emitters (json_emit_begin / json_emit_event /
 * json_emit_end) remain in grep.c because their schema is grep-specific.
 *
 * Every subcommand links against this module to produce NDJSON output.
 */
#include "json_emit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- JSON string escaping ---------- */

void lf_json_escape(lf_buf_t *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  lf_buf_append(b, "\\\"", 2); break;
            case '\\': lf_buf_append(b, "\\\\", 2); break;
            case '\n': lf_buf_append(b, "\\n",  2); break;
            case '\r': lf_buf_append(b, "\\r",  2); break;
            case '\t': lf_buf_append(b, "\\t",  2); break;
            case '\b': lf_buf_append(b, "\\b",  2); break;
            case '\f': lf_buf_append(b, "\\f",  2); break;
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

/* ---------- formatted append ---------- */

void lf_buf_appendf(lf_buf_t *b, const char *fmt, ...) {
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

/* ---------- JSON building primitives ---------- */

void lf_json_begin_object(lf_buf_t *b) { lf_buf_append(b, "{", 1); }
void lf_json_end_object  (lf_buf_t *b) { lf_buf_append(b, "}", 1); }
void lf_json_begin_array (lf_buf_t *b) { lf_buf_append(b, "[", 1); }
void lf_json_end_array   (lf_buf_t *b) { lf_buf_append(b, "]", 1); }

void lf_json_key(lf_buf_t *b, const char *key) {
    lf_json_string(b, key, strlen(key));
    lf_buf_append(b, ":", 1);
}

void lf_json_string(lf_buf_t *b, const char *val, size_t n) {
    lf_buf_append(b, "\"", 1);
    lf_json_escape(b, val, n);
    lf_buf_append(b, "\"", 1);
}

void lf_json_int(lf_buf_t *b, long long val) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", val);
    if (n > 0) lf_buf_append(b, tmp, (size_t)n);
}

void lf_json_uint(lf_buf_t *b, unsigned long long val) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%llu", val);
    if (n > 0) lf_buf_append(b, tmp, (size_t)n);
}

void lf_json_bool(lf_buf_t *b, int val) {
    if (val) lf_buf_append(b, "true", 4);
    else     lf_buf_append(b, "false", 5);
}

void lf_json_null(lf_buf_t *b) {
    lf_buf_append(b, "null", 4);
}
