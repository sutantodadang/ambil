/*
 * json_emit.h — shared JSON formatting primitives.
 *
 * All subcommands use these building blocks to construct NDJSON output.
 * Each subcommand is responsible for its own schema (event types, field
 * names, nesting). This module provides only the low-level primitives:
 * escaping, printf-to-buffer, and basic JSON syntax helpers.
 *
 * No grep types, no match structures, no subcommand-specific knowledge.
 */
#ifndef AMBIL_JSON_EMIT_H
#define AMBIL_JSON_EMIT_H

#include "util.h"

#include <stddef.h>

/* Append a JSON-escaped copy of [s, s+n) to the buffer.
 * Control characters become \u00xx, quotes/backslash/newline/etc. become
 * the usual two-character escapes. Embedded NUL bytes are passed through
 * (valid JSON: "\u0000"). */
void lf_json_escape(lf_buf_t *b, const char *s, size_t n);

/* printf-style append to a dynamic buffer.  Uses a stack buffer for the
 * common fast path (< 256 bytes), heap-allocates only when needed.
 * The formatting itself is delegated to vsnprintf, so every standard
 * printf conversion is supported. */
void lf_buf_appendf(lf_buf_t *b, const char *fmt, ...);

/* ---- JSON building primitives ----
 *
 * Each function appends the corresponding JSON token to the buffer.
 * These are trivial but give every subcommand a single, reviewable
 * spelling for JSON syntax so there is no risk of ad-hoc hand-roll.
 *
 * Usage pattern for an NDJSON event:
 *
 *   lf_json_begin_object(out);
 *   lf_json_key(out, "type");  lf_json_string(out, "match", 5);
 *   lf_json_key(out, "path");  lf_json_string(out, path, strlen(path));
 *   lf_json_key(out, "line");  lf_json_uint(out, lineno);
 *   lf_json_end_object(out);
 *   lf_buf_append(out, "\n", 1);
 */

void lf_json_begin_object(lf_buf_t *b);
void lf_json_end_object  (lf_buf_t *b);
void lf_json_begin_array (lf_buf_t *b);
void lf_json_end_array   (lf_buf_t *b);

/* Write `"key":` (comma is caller's responsibility). */
void lf_json_key(lf_buf_t *b, const char *key);

/* Write `"<escaped-val>"`. */
void lf_json_string(lf_buf_t *b, const char *val, size_t n);

/* Write signed/unsigned integer. */
void lf_json_int (lf_buf_t *b, long long val);
void lf_json_uint(lf_buf_t *b, unsigned long long val);

/* Write `true`, `false`, or `null`. */
void lf_json_bool(lf_buf_t *b, int val);
void lf_json_null(lf_buf_t *b);

#endif /* AMBIL_JSON_EMIT_H */
