/*
 * main.c — CLI entry, options parsing, and dispatch.
 *
 * Two execution paths:
 *   - GREP MODE (default): walk paths, parallel-scan files, print results.
 *   - LOG MODE  (legacy):  triggered by --field/--count-field/--since/--until
 *     or --log-json. Uses the original mmap+chunk pipeline against a single
 *     file path, preserving 0.1 behaviour and tests.
 */
#include "ambil.h"
#include "filter.h"
#include "aggregate.h"
#include "thread_pool.h"
#include "util.h"
#include "grep.h"
#include "ignore.h"
#include "walker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define fileno _fileno
#define isatty _isatty
#else
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(FILE *out) {
    fprintf(out,
        "ambil " AMBIL_VERSION " — token-efficient grep for AI coding agents\n"
        "\n"
        "Usage:\n"
        "  ambil [OPTIONS] PATTERN [PATH...]\n"
        "\n"
        "Search:\n"
        "  -e, --regexp PATTERN    additional pattern (literal; alias --pattern)\n"
        "  -i, --ignore-case       case-insensitive\n"
        "  -F, --fixed-strings     literal pattern (default; only mode supported)\n"
        "  -w, --word-regexp       whole-word match\n"
        "  -v, --invert-match      lines that do NOT match\n"
        "  -o, --only-matching     print only matched substrings\n"
        "\n"
        "Files:\n"
        "  PATH                    file or directory; default '.'\n"
        "      --no-recursive      do not descend into directories\n"
        "  -g, --glob GLOB         include/exclude glob (! prefix excludes)\n"
        "  -t, --type TYPE         file type alias (rust|py|ts|js|c|cpp|go|md|json|yaml|toml|sh)\n"
        "      --max-depth N       max directory depth\n"
        "      --hidden            include hidden files\n"
        "      --no-ignore         disable .gitignore + default ignores\n"
        "      --follow            follow symlinks\n"
        "  -a, --text              treat binary files as text\n"
        "\n"
        "Context:\n"
        "  -A, --after-context N\n"
        "  -B, --before-context N\n"
        "  -C, --context N\n"
        "\n"
        "Output:\n"
        "  -n, --line-number       show line numbers (default ON)\n"
        "  -N, --no-line-number\n"
        "  -H, --with-filename     show filename (default ON for >1 file)\n"
        "      --no-filename\n"
        "      --no-heading        path:line:text instead of grouped\n"
        "      --compact           token-efficient grouped output (recommended for agents)\n"
        "      --json              NDJSON structured output\n"
        "  -c, --count             only print count per file\n"
        "  -l, --files-with-matches\n"
        "      --color WHEN        auto|always|never (default auto)\n"
        "      --no-color          shorthand for --color never\n"
        "\n"
        "Performance:\n"
        "  -j, --threads N         worker threads (default = CPU count)\n"
        "\n"
        "Log mode (bonus features):\n"
        "      --since TS / --until TS\n"
        "      --field K=V\n"
        "      --count-field FIELD\n"
        "      --log-json          structured log line filter\n"
        "\n"
        "Misc:\n"
        "  -h, --help              show this help\n"
        "  -V, --version           show version\n"
    );
}

/* Return 1 if arg is exactly "--name" or "--name=value"; sets *value if =. */
static int long_opt(const char *arg, const char *name, const char **value) {
    size_t n = strlen(name);
    if (strncmp(arg, "--", 2) != 0) return 0;
    if (strncmp(arg + 2, name, n) != 0) return 0;
    char tail = arg[2 + n];
    if (tail == '\0') { *value = NULL; return 1; }
    if (tail == '=')  { *value = arg + 3 + n; return 1; }
    return 0;
}

/* Take the value for a flag: inline (--flag=val), next arg, or error. */
#define TAKE_VALUE(name) \
    do { \
        if (!val) { \
            if (++i >= argc) { fprintf(stderr, "ambil: %s requires a value\n", name); return -1; } \
            val = argv[i]; \
        } \
    } while (0)

static int parse_int(const char *s, long *out) {
    char *endp;
    long v = strtol(s, &endp, 10);
    if (*endp != '\0') return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, options_t *o) {
    lf_options_init(o);

    /* Positionals: the first becomes the pattern (if -e was not used),
     * the rest are paths. */
    const char **pos = (const char **)lf_xmalloc(sizeof(char *) * (size_t)(argc + 1));
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = NULL;

        if (strcmp(a, "--") == 0) {
            for (int k = i + 1; k < argc; k++) pos[npos++] = argv[k];
            break;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); free(pos); exit(0); }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) { printf("ambil %s\n", AMBIL_VERSION); free(pos); exit(0); }

        if (strcmp(a, "-i") == 0 || strcmp(a, "--ignore-case") == 0)   { o->ignore_case = 1; continue; }
        if (strcmp(a, "-F") == 0 || strcmp(a, "--fixed-strings") == 0) { o->fixed_strings = 1; continue; }
        if (strcmp(a, "-w") == 0 || strcmp(a, "--word-regexp") == 0)   { o->word_regexp = 1; continue; }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--invert-match") == 0)  { o->invert_match = 1; continue; }
        if (strcmp(a, "-o") == 0 || strcmp(a, "--only-matching") == 0) { o->only_matching = 1; continue; }
        if (strcmp(a, "-a") == 0 || strcmp(a, "--text") == 0)          { o->text_mode = 1; continue; }
        if (strcmp(a, "-r") == 0 || strcmp(a, "--recursive") == 0)     { o->recursive = 1; continue; }
        if (strcmp(a, "--no-recursive") == 0)                          { o->recursive = 0; continue; }
        if (strcmp(a, "--hidden") == 0)                                { o->hidden = 1; continue; }
        if (strcmp(a, "--no-ignore") == 0)                             { o->no_ignore = 1; continue; }
        if (strcmp(a, "--follow") == 0)                                { o->follow_symlinks = 1; continue; }
        if (strcmp(a, "-n") == 0 || strcmp(a, "--line-number") == 0)   { o->line_numbers = 1; continue; }
        if (strcmp(a, "-N") == 0 || strcmp(a, "--no-line-number") == 0){ o->line_numbers = 0; continue; }
        if (strcmp(a, "-H") == 0 || strcmp(a, "--with-filename") == 0) { o->show_filename = 1; continue; }
        if (strcmp(a, "--no-filename") == 0)                           { o->show_filename = 0; continue; }
        if (strcmp(a, "--no-heading") == 0)                            { o->no_heading = 1; continue; }
        if (strcmp(a, "--compact") == 0)                               { o->mode = OUT_COMPACT; continue; }
        if (strcmp(a, "--json") == 0)                                  { o->mode = OUT_JSON; continue; }
        if (strcmp(a, "--log-json") == 0)                              { o->log_json_mode = 1; continue; }
        if (strcmp(a, "-c") == 0 || strcmp(a, "--count") == 0)         { o->mode = OUT_COUNT; continue; }
        if (strcmp(a, "-l") == 0 || strcmp(a, "--files-with-matches") == 0) { o->mode = OUT_FILES_WITH; continue; }
        if (strcmp(a, "--no-color") == 0)                              { o->color = COLOR_NEVER; continue; }

        if (long_opt(a, "color", &val)) {
            const char *cv = val ? val : "always";
            if      (strcmp(cv, "auto")   == 0) o->color = COLOR_AUTO;
            else if (strcmp(cv, "always") == 0) o->color = COLOR_ALWAYS;
            else if (strcmp(cv, "never")  == 0) o->color = COLOR_NEVER;
            else { fprintf(stderr, "ambil: --color expects auto|always|never\n"); free(pos); return -1; }
            continue;
        }

        if (strcmp(a, "-e") == 0 || long_opt(a, "regexp", &val) || long_opt(a, "pattern", &val)) {
            if (strcmp(a, "-e") == 0) val = NULL;
            TAKE_VALUE("-e/--regexp/--pattern");
            lf_options_add_pattern(o, val);
            continue;
        }
        if (strcmp(a, "-g") == 0 || long_opt(a, "glob", &val)) {
            if (strcmp(a, "-g") == 0) val = NULL;
            TAKE_VALUE("-g/--glob");
            lf_options_add_glob(o, val);
            continue;
        }
        if (strcmp(a, "-t") == 0 || long_opt(a, "type", &val)) {
            if (strcmp(a, "-t") == 0) val = NULL;
            TAKE_VALUE("-t/--type");
            if (!lf_ignore_typename_known(val)) {
                fprintf(stderr, "ambil: unknown type '%s'\n", val); free(pos); return -1;
            }
            lf_options_add_type(o, val);
            continue;
        }
        if (long_opt(a, "max-depth", &val)) {
            TAKE_VALUE("--max-depth");
            long v;
            if (parse_int(val, &v) != 0 || v < 0) { fprintf(stderr, "ambil: bad --max-depth\n"); free(pos); return -1; }
            o->max_depth = (int)v;
            continue;
        }
        if (strcmp(a, "-A") == 0 || long_opt(a, "after-context", &val)) {
            if (strcmp(a, "-A") == 0) val = NULL;
            TAKE_VALUE("-A/--after-context");
            long v;
            if (parse_int(val, &v) != 0 || v < 0) { fprintf(stderr, "ambil: bad -A\n"); free(pos); return -1; }
            o->after_context = (int)v; continue;
        }
        if (strcmp(a, "-B") == 0 || long_opt(a, "before-context", &val)) {
            if (strcmp(a, "-B") == 0) val = NULL;
            TAKE_VALUE("-B/--before-context");
            long v;
            if (parse_int(val, &v) != 0 || v < 0) { fprintf(stderr, "ambil: bad -B\n"); free(pos); return -1; }
            o->before_context = (int)v; continue;
        }
        if (strcmp(a, "-C") == 0 || long_opt(a, "context", &val)) {
            if (strcmp(a, "-C") == 0) val = NULL;
            TAKE_VALUE("-C/--context");
            long v;
            if (parse_int(val, &v) != 0 || v < 0) { fprintf(stderr, "ambil: bad -C\n"); free(pos); return -1; }
            o->after_context = (int)v; o->before_context = (int)v; continue;
        }
        if (strcmp(a, "-j") == 0 || long_opt(a, "threads", &val)) {
            if (strcmp(a, "-j") == 0) val = NULL;
            TAKE_VALUE("-j/--threads");
            long v;
            if (parse_int(val, &v) != 0 || v < 1 || v > 1024) { fprintf(stderr, "ambil: bad -j\n"); free(pos); return -1; }
            o->threads = (int)v; continue;
        }
        /* Legacy log-mode flags. */
        if (long_opt(a, "field", &val)) {
            TAKE_VALUE("--field");
            const char *eq = strchr(val, '=');
            if (!eq) { fprintf(stderr, "ambil: --field expects key=value\n"); free(pos); return -1; }
            size_t klen = (size_t)(eq - val);
            char *kbuf = (char *)lf_xmalloc(klen + 1);
            memcpy(kbuf, val, klen); kbuf[klen] = '\0';
            o->field_key   = kbuf;
            o->field_value = eq + 1;
            o->log_json_mode = 1;
            continue;
        }
        if (long_opt(a, "since", &val) || long_opt(a, "from", &val)) {
            TAKE_VALUE("--since");
            if (lf_parse_time(val, &o->from_ts) != 0) { fprintf(stderr, "ambil: bad --since\n"); free(pos); return -1; }
            continue;
        }
        if (long_opt(a, "until", &val) || long_opt(a, "to", &val)) {
            TAKE_VALUE("--until");
            if (lf_parse_time(val, &o->to_ts) != 0) { fprintf(stderr, "ambil: bad --until\n"); free(pos); return -1; }
            continue;
        }
        if (long_opt(a, "count-field", &val)) {
            TAKE_VALUE("--count-field");
            o->count_field   = val;
            o->log_json_mode = 1;
            continue;
        }

        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "ambil: unknown option '%s' (try --help)\n", a);
            free(pos); return -1;
        }
        pos[npos++] = a;
    }

    /* Resolve positionals.
     *
     * Grep mode: first positional is pattern (unless any -e was provided,
     * in which case ALL positionals are paths). Remaining are paths.
     *
     * Log mode (no patterns required): first positional is treated as the
     * input file (preserves v0.1 behaviour where pattern was optional). */
    int log_mode = lf_options_is_log_mode(o);

    if (o->n_patterns == 0 && !log_mode) {
        if (npos == 0) {
            fprintf(stderr, "ambil: missing PATTERN (try --help)\n");
            free(pos); return -1;
        }
        lf_options_add_pattern(o, pos[0]);
        for (int k = 1; k < npos; k++) lf_options_add_path(o, pos[k]);
    } else if (log_mode && o->n_patterns == 0) {
        /* v0.1 behaviour: optional pattern + required file. */
        if (npos == 0) {
            fprintf(stderr, "ambil: missing input file\n"); free(pos); return -1;
        }
        if (npos == 1) {
            lf_options_add_path(o, pos[0]);
        } else {
            lf_options_add_pattern(o, pos[0]);
            for (int k = 1; k < npos; k++) lf_options_add_path(o, pos[k]);
        }
    } else {
        for (int k = 0; k < npos; k++) lf_options_add_path(o, pos[k]);
    }

    if (o->n_paths == 0) {
        if (log_mode) {
            fprintf(stderr, "ambil: missing input file (log mode)\n"); free(pos); return -1;
        }
        lf_options_add_path(o, ".");
    }

    if (o->threads <= 0) o->threads = lf_detect_cpus();

    /* --json is the structured grep emitter. In log mode there is no NDJSON
     * shape (the legacy emitter writes filtered raw lines), so treat
     * `--json` here as the documented `--log-json` alias rather than
     * silently emitting non-JSON output under an OUT_JSON label. */
    if (log_mode && o->mode == OUT_JSON) {
        o->log_json_mode = 1;
        o->mode = OUT_TEXT;
    }
    free(pos);
    return 0;
}

static int want_color(color_mode_t m) {
    if (m == COLOR_NEVER)  return 0;
    if (m == COLOR_ALWAYS) return 1;
    return isatty(fileno(stdout)) ? 1 : 0;
}

/* ---------- Legacy log-mode runner (single file, chunked) ---------- */

typedef struct {
    const char *addr;
    size_t size;
    int fd;
#ifdef _WIN32
    HANDLE mapping;
#endif
} mapped_file_t;

static int map_file(const char *path, mapped_file_t *mf) {
    memset(mf, 0, sizeof(*mf));
    mf->fd = -1;
    int fd;
#ifdef _WIN32
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (n <= 0) {
            fprintf(stderr, "ambil: cannot translate path '%s'\n", path);
            return -1;
        }
        wchar_t *wp = (wchar_t *)lf_xmalloc((size_t)n * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, n);
        fd = _wopen(wp, _O_RDONLY | _O_BINARY);
        free(wp);
    }
#else
    fd = open(path, O_RDONLY);
#endif
    if (fd < 0) {
        fprintf(stderr, "ambil: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "ambil: '%s' is not a regular file\n", path);
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return -1;
    }
    if (st.st_size == 0) { mf->fd = fd; return 0; }
#ifdef _WIN32
    HANDLE os_file = (HANDLE)_get_osfhandle(fd);
    HANDLE mapping = CreateFileMapping(os_file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) { _close(fd); return -1; }
    void *p = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(mapping); _close(fd); return -1; }
    mf->addr = (const char *)p;
    mf->size = (size_t)st.st_size;
    mf->fd = fd;
    mf->mapping = mapping;
    return 0;
#else
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    (void)madvise(p, (size_t)st.st_size, MADV_SEQUENTIAL);
    mf->addr = (const char *)p;
    mf->size = (size_t)st.st_size;
    mf->fd = fd;
    return 0;
#endif
}

static void unmap_file(mapped_file_t *mf) {
    if (!mf) return;
#ifdef _WIN32
    if (mf->addr) UnmapViewOfFile(mf->addr);
    if (mf->mapping) CloseHandle(mf->mapping);
#else
    if (mf->addr) munmap((void *)mf->addr, mf->size);
#endif
#ifdef _WIN32
    if (mf->fd >= 0) _close(mf->fd);
#else
    if (mf->fd >= 0) close(mf->fd);
#endif
    memset(mf, 0, sizeof(*mf));
    mf->fd = -1;
}

static int run_log_mode(options_t *opt) {
    if (opt->n_paths != 1) {
        fprintf(stderr, "ambil: log mode requires exactly one input file\n");
        return 1;
    }
    /* Translate options_t into the legacy single-file options view. */
    mapped_file_t mf;
    if (map_file(opt->paths[0], &mf) != 0) return 1;

    /* Build a v0.1 options proxy for filter. */
    options_t leg;
    memset(&leg, 0, sizeof(leg));
    leg.from_ts = INT64_MIN; leg.to_ts = INT64_MIN;
    /* Re-use old option names — only fields filter/parser look at. */
    static char patbuf[1024];
    if (opt->n_patterns > 0) {
        strncpy(patbuf, opt->patterns[0], sizeof(patbuf) - 1);
        patbuf[sizeof(patbuf) - 1] = '\0';
    } else {
        patbuf[0] = '\0';
    }
    /* Reach through legacy filter via shimmed fields by reusing main option. */

    /* Build filter from current options. We construct a small adapter. */
    typedef struct {
        const char *pattern;
        int ignore_case;
        int json_mode;
        const char *field_key;
        const char *field_value;
        int64_t from_ts, to_ts;
    } shim_t;
    shim_t s = {
        .pattern = (opt->n_patterns > 0) ? opt->patterns[0] : NULL,
        .ignore_case = opt->ignore_case,
        .json_mode = (opt->field_key != NULL),
        .field_key = opt->field_key,
        .field_value = opt->field_value,
        .from_ts = opt->from_ts,
        .to_ts   = opt->to_ts
    };

    /* Use lf_filter_build with a shim cast to options_t — they share the
     * same prefix layout. We build a real options_t to be safe. */
    options_t legacy_proxy;
    memset(&legacy_proxy, 0, sizeof(legacy_proxy));
    legacy_proxy.n_patterns = opt->n_patterns;
    legacy_proxy.patterns   = opt->patterns;
    legacy_proxy.ignore_case = opt->ignore_case;
    legacy_proxy.field_key   = opt->field_key;
    legacy_proxy.field_value = opt->field_value;
    legacy_proxy.from_ts = opt->from_ts;
    legacy_proxy.to_ts   = opt->to_ts;
    legacy_proxy.log_json_mode = 1;

    lf_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    /* Build filter fields directly. */
    if (s.pattern && s.pattern[0]) {
        if (lf_search_init(&filter.searcher, s.pattern, s.ignore_case) != 0) {
            unmap_file(&mf); return 1;
        }
        filter.has_search = 1;
    }
    filter.has_time = (opt->from_ts != INT64_MIN || opt->to_ts != INT64_MIN);
    filter.from_ts  = (opt->from_ts == INT64_MIN) ? INT64_MIN : opt->from_ts;
    filter.to_ts    = (opt->to_ts   == INT64_MIN) ? INT64_MAX : opt->to_ts;
    filter.json_mode = 1;
    filter.field_key = opt->field_key;
    filter.field_key_len = opt->field_key ? strlen(opt->field_key) : 0;
    filter.field_value = opt->field_value;
    filter.field_value_len = opt->field_value ? strlen(opt->field_value) : 0;
    filter.ignore_case_field = opt->ignore_case;

    int color = (opt->count_field == NULL) && want_color(opt->color);
    int count_mode = (opt->count_field != NULL);

    int nthreads = opt->threads;
    if (mf.size < (size_t)(64 * 1024)) nthreads = 1;

    lf_worker_out_t *outs = (lf_worker_out_t *)lf_xcalloc((size_t)nthreads, sizeof(lf_worker_out_t));
    int rc = 0;
    if (mf.size > 0) {
        rc = lf_run_parallel(mf.addr, mf.size, nthreads, &filter,
                             count_mode, opt->count_field, color, outs);
    }

    uint64_t matched_total = 0;
    if (count_mode) {
        lf_agg_t merged;
        lf_agg_init(&merged);
        for (int t = 0; t < nthreads; t++) {
            lf_agg_merge(&merged, &outs[t].agg);
            matched_total += outs[t].matched;
        }
        lf_agg_print_sorted(&merged, stdout);
        lf_agg_free(&merged);
    } else {
        for (int t = 0; t < nthreads; t++) {
            if (outs[t].out.len) fwrite(outs[t].out.data, 1, outs[t].out.len, stdout);
            matched_total += outs[t].matched;
        }
    }
    fflush(stdout);

    for (int t = 0; t < nthreads; t++) {
        if (count_mode) lf_agg_free(&outs[t].agg);
        else            lf_buf_free(&outs[t].out);
    }
    free(outs);
    if (filter.has_search) lf_search_free(&filter.searcher);
    unmap_file(&mf);
    if (rc != 0) return 1;
    return matched_total > 0 ? 0 : 1;
}

/* ---------- Grep mode ---------- */

static int run_grep_mode(options_t *opt) {
    /* Resolve auto-show-filename: ON when more than one root path or any
     * root is a directory (recursive walk). */
    if (opt->show_filename < 0) {
        int any_dir_or_multi = (opt->n_paths > 1) ? 1 : 0;
        if (!any_dir_or_multi && opt->n_paths == 1) {
            struct stat st;
            if (stat(opt->paths[0], &st) == 0 && S_ISDIR(st.st_mode)) any_dir_or_multi = 1;
        }
        opt->show_filename = any_dir_or_multi ? 1 : 0;
    }

    int color = want_color(opt->color);

    lf_patterns_t ps;
    if (lf_patterns_build(&ps, opt) != 0) {
        fprintf(stderr, "ambil: failed to build patterns\n");
        return 2;
    }

    lf_ignore_t *ig = lf_ignore_new(opt);
    lf_walker_t *w  = lf_walk_open(opt->paths, opt->n_paths, ig,
                                   opt->recursive, opt->max_depth, opt->follow_symlinks);

    lf_dispatch_stats_t stats;
    int rc = lf_dispatch_files(w, opt, &ps, color, opt->show_filename, &stats);

    /* If any root path or directory was unreadable, exit 2 even when other
     * inputs produced matches — mirrors GNU grep's behaviour. */
    unsigned werrs = lf_walk_error_count(w);

    lf_walk_close(w);
    lf_ignore_free(ig);
    lf_patterns_free(&ps);
    fflush(stdout);

    if (rc != 0 || werrs > 0 || stats.errors > 0) return 2;
    return stats.matched_lines > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    options_t opt;
    if (parse_args(argc, argv, &opt) != 0) { lf_options_free(&opt); return 2; }

    int rc;
    if (lf_options_is_log_mode(&opt)) {
        rc = run_log_mode(&opt);
    } else {
        rc = run_grep_mode(&opt);
    }

    lf_options_free(&opt);
    return rc;
}
