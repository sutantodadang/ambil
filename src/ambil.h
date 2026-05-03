/*
 * ambil.h — shared types and options.
 *
 * Central definitions used across all translation units. Keeping these in a
 * single header avoids cyclic includes and gives every module a stable view
 * of the runtime configuration.
 */
#ifndef AMBIL_H
#define AMBIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#define AMBIL_VERSION "0.2.0"

/* Output coloring policy. */
typedef enum {
    COLOR_AUTO = 0,
    COLOR_ALWAYS,
    COLOR_NEVER
} color_mode_t;

/* Output mode. Mutually exclusive top-level rendering. */
typedef enum {
    OUT_TEXT = 0,    /* default ripgrep-style grouped (or no-heading) */
    OUT_COMPACT,     /* token-efficient grouped */
    OUT_JSON,        /* NDJSON structured grep events */
    OUT_COUNT,       /* -c: path:count */
    OUT_FILES_WITH,  /* -l: paths only */
    OUT_LOG_AGG,     /* legacy: --count-field <f> aggregation */
    OUT_LOG_JSON     /* legacy: --log-json line filter output */
} output_mode_t;

/*
 * options_t — parsed CLI configuration.
 *
 * Most pointer fields reference argv-owned memory. The dynamically grown
 * arrays (patterns, paths, globs, types) are owned by this struct and freed
 * via lf_options_free(). field_key may be malloc'd when parsed from K=V.
 */
typedef struct {
    /* Patterns: positional pattern + any -e flags merged into one array. */
    const char **patterns;
    size_t       n_patterns;
    size_t       cap_patterns;

    /* Input paths (files or directories). */
    const char **paths;
    size_t       n_paths;
    size_t       cap_paths;

    /* Search modifiers. */
    int ignore_case;          /* -i */
    int fixed_strings;        /* -F (default) */
    int word_regexp;          /* -w */
    int invert_match;         /* -v */
    int only_matching;        /* -o */

    /* Walking. */
    int recursive;            /* default 1; --no-recursive sets 0 */
    int follow_symlinks;      /* --follow */
    int hidden;               /* --hidden */
    int no_ignore;            /* --no-ignore */
    int text_mode;            /* -a */
    int max_depth;            /* -1 = unlimited */

    /* Globs and types. */
    const char **globs;
    size_t       n_globs;
    size_t       cap_globs;

    const char **types;
    size_t       n_types;
    size_t       cap_types;

    /* Context. */
    int after_context;        /* -A */
    int before_context;       /* -B */

    /* Output. */
    output_mode_t mode;
    int line_numbers;         /* default 1; -N disables */
    int show_filename;        /* -1 auto, 0 off, 1 on */
    int no_heading;           /* --no-heading */
    color_mode_t color;

    /* Performance. */
    int threads;              /* 0 = auto */

    /* Legacy log-mode fields. */
    int log_json_mode;        /* --log-json (or --json + log filters) */
    char *field_key;          /* malloc'd when set */
    const char *field_value;  /* into argv */
    int64_t from_ts;          /* --since INT64_MIN if unset */
    int64_t to_ts;            /* --until INT64_MIN if unset */
    const char *count_field;  /* --count-field FIELD */
} options_t;

void lf_options_init(options_t *o);
void lf_options_free(options_t *o);

/* True if any legacy log-mode filter/aggregation is active. */
int  lf_options_is_log_mode(const options_t *o);

/* Append helpers (abort on OOM via lf_die). */
void lf_options_add_pattern(options_t *o, const char *p);
void lf_options_add_path(options_t *o, const char *p);
void lf_options_add_glob(options_t *o, const char *g);
void lf_options_add_type(options_t *o, const char *t);

/* Single chunk slice (used by single-file parallel scan path). */
typedef struct {
    const char *data;
    size_t      len;
    size_t      id;
    size_t      base_offset;
} chunk_t;

#endif /* AMBIL_H */
