/*
 * filter.h — per-line predicate combining time, search, and JSON field tests.
 *
 * The filter is constructed once per run from options_t and passed read-only
 * to every worker. Evaluation order is intentional: cheapest discriminators
 * (time range, then substring search) run before the JSON scanner, so common
 * misses cost as little as possible.
 */
#ifndef AMBIL_FILTER_H
#define AMBIL_FILTER_H

#include "ambil.h"
#include "search.h"

#include <stdint.h>

typedef struct {
    int has_search;
    lf_searcher_t searcher;

    int has_time;
    int64_t from_ts;          /* inclusive; INT64_MIN means -inf */
    int64_t to_ts;            /* inclusive; INT64_MAX means +inf */

    int json_mode;
    const char *field_key;    /* for --field key=value match (NULL if none) */
    size_t      field_key_len;
    const char *field_value;
    size_t      field_value_len;
    int         ignore_case_field;
} lf_filter_t;

int  lf_filter_build(lf_filter_t *f, const options_t *o);
void lf_filter_free(lf_filter_t *f);

/*
 * Returns 1 if the line passes all active predicates, 0 otherwise. `line`
 * does NOT include the trailing newline. Pure / thread-safe across workers.
 */
int  lf_filter_match(const lf_filter_t *f, const char *line, size_t llen);

#endif /* AMBIL_FILTER_H */
