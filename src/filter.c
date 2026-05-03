/*
 * filter.c — predicate orchestration for one log line.
 */
#include "filter.h"
#include "parser.h"
#include "util.h"

#include <stdint.h>
#include <string.h>

int lf_filter_build(lf_filter_t *f, const options_t *o) {
    memset(f, 0, sizeof(*f));

    const char *pat = (o->n_patterns > 0) ? o->patterns[0] : NULL;
    if (pat && pat[0]) {
        if (lf_search_init(&f->searcher, pat, o->ignore_case) != 0) {
            return -1;
        }
        f->has_search = 1;
    }

    f->has_time = (o->from_ts != INT64_MIN || o->to_ts != INT64_MIN);
    f->from_ts  = (o->from_ts == INT64_MIN) ? INT64_MIN : o->from_ts;
    f->to_ts    = (o->to_ts   == INT64_MIN) ? INT64_MAX : o->to_ts;

    f->json_mode = o->log_json_mode;

    if (o->field_key) {
        f->field_key         = o->field_key;
        f->field_key_len     = strlen(o->field_key);
        f->field_value       = o->field_value;
        f->field_value_len   = o->field_value ? strlen(o->field_value) : 0;
        f->ignore_case_field = o->ignore_case;
    }
    return 0;
}

void lf_filter_free(lf_filter_t *f) {
    if (f->has_search) lf_search_free(&f->searcher);
    memset(f, 0, sizeof(*f));
}

static int parse_json_ts_field(const char *line, size_t llen, int64_t *ts) {
    const char *value_ptr;
    size_t value_len;
    char tmp[128];

    if (lf_json_get_field(line, llen, "ts", 2, &value_ptr, &value_len) != 0) return -1;
    if (value_len == 0 || value_len >= sizeof(tmp)) return -1;

    memcpy(tmp, value_ptr, value_len);
    tmp[value_len] = '\0';
    return lf_parse_time(tmp, ts);
}

int lf_filter_match(const lf_filter_t *f, const char *line, size_t llen) {
    /* 1. Time range — cheapest, anchored at line start. */
    if (f->has_time) {
        int64_t ts;
        if (lf_parse_line_time(line, llen, &ts) != 0) {
            if (!f->json_mode || parse_json_ts_field(line, llen, &ts) != 0) return 0;
        }
        if (ts < f->from_ts || ts > f->to_ts) return 0;
    }

    /* 2. Substring — fast path eliminates the majority of non-matching lines. */
    if (f->has_search) {
        if (!lf_search_find(&f->searcher, line, llen)) return 0;
    }

    /* 3. JSON field equality — most expensive, runs last.
     *
     * Field VALUES from the JSON line carry escapes verbatim (e.g. a path
     * `/api/billing` may serialize as `/api\/billing`). Compare via the
     * decoder so the user-supplied literal matches both encoded forms. */
    if (f->field_key) {
        const char *vp;
        size_t vlen;
        if (lf_json_get_field(line, llen, f->field_key, f->field_key_len, &vp, &vlen) != 0) {
            return 0;
        }
        if (f->field_value) {
            if (!lf_json_str_eq(vp, vlen, f->field_value, f->field_value_len,
                                f->ignore_case_field)) return 0;
        }
    }
    return 1;
}
