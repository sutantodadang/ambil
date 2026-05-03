/*
 * parser.h — line-oriented JSON field extraction and timestamp lifting.
 *
 * We deliberately do NOT implement a full JSON parser. Log lines are flat
 * objects 99% of the time, and a token scanner that finds `"key":<value>`
 * is 5-10x faster than a recursive descent parser on this workload.
 *
 * Tradeoffs (documented for future maintainers):
 *   - Nested objects are not deeply traversed; the first top-level match
 *     for `key` wins. For nested keys, callers should use dotted paths
 *     (not implemented in MVP).
 *   - String values have their JSON escapes left as-is (no unescaping).
 *     This is intentional: comparisons and counting work byte-wise on the
 *     raw value, which matches both `--field level=error` semantics and
 *     aggregation buckets.
 */
#ifndef AMBIL_PARSER_H
#define AMBIL_PARSER_H

#include <stddef.h>
#include <stdint.h>

/*
 * lf_json_get_field — locate the value of `key` within a single JSON line.
 *
 * On success: returns 0, sets *vptr to the start of the value bytes (inside
 * the line buffer; not NUL terminated) and *vlen to its length.
 *
 *   - String values: pointer/length cover the bytes BETWEEN the quotes
 *     (escapes preserved). Empty strings yield vlen == 0.
 *   - Numeric / true / false / null values: pointer/length cover the literal
 *     token (e.g. "123", "true").
 *
 * Returns -1 if the key is not found or the line is not parseable enough
 * to locate it. Cheap on miss — early-exits at first non-JSON byte.
 */
int lf_json_get_field(const char *line, size_t llen,
                      const char *key,  size_t klen,
                      const char **vptr, size_t *vlen);

/*
 * Compare a JSON-encoded string token (raw bytes between the surrounding
 * quotes, escapes intact) against a literal byte sequence.
 *
 * Returns 1 on equality, 0 otherwise. `ignore_case` folds ASCII only.
 * Decodes \", \\, \/, \b, \f, \n, \r, \t, and \uXXXX (BMP code points are
 * UTF-8 re-encoded; surrogate pairs are decoded jointly). Malformed escapes
 * fail the comparison rather than aborting.
 */
int lf_json_str_eq(const char *raw, size_t rlen,
                   const char *lit, size_t llen,
                   int ignore_case);

#endif /* AMBIL_PARSER_H */
