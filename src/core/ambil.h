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
#include <signal.h>
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
} output_mode_t;

/*
 * common_opts_t — options shared across all subcommands.
 *
 * Every subcommand's option struct embeds this as its first field so common
 * flags (--format, --color, --threads) are consistently accessible.
 */
typedef struct {
    output_mode_t format;  /* OUT_TEXT, OUT_COMPACT, OUT_JSON */
    color_mode_t  color;
    int           threads; /* 0 = auto */
} common_opts_t;

/*
 * grep_opts_t — parsed grep-mode configuration.
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
    int extended_regexp;      /* -E / --extended-regexp: enables re_tiny */
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
    int stream;               /* --stream: flush per-line, no buffering, no cross-file order */

} grep_opts_t;

void lf_options_init(grep_opts_t *o);
void lf_options_free(grep_opts_t *o);

/* Append helpers (abort on OOM via lf_die). */
void lf_options_add_pattern(grep_opts_t *o, const char *p);
void lf_options_add_path(grep_opts_t *o, const char *p);
void lf_options_add_glob(grep_opts_t *o, const char *g);
void lf_options_add_type(grep_opts_t *o, const char *t);

/* Per-file scan stats (shared by grep + thread_pool). */
typedef struct {
    uint64_t matched_lines;
    uint64_t bytes_searched;
    uint64_t searches;        /* always 1 for single-file path */
    int      had_match;
} lf_file_stats_t;

#endif /* AMBIL_H */
