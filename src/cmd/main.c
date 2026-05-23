/*
 * main.c — CLI entry, options parsing, and dispatch.
 *
 * Single execution path: walk paths, parallel-scan files, print results.
 */
#include "ambil.h"
#include "thread_pool.h"
#include "util.h"
#include "grep.h"
#include "ignore.h"
#include "walker.h"
#include "help.h"
#include "cat.h"
#include "wc.h"
#include "ls.h"
#include "find.h"
#include "env.h"
#include "simd_search.h"
#include "filetype.h"
#include "file_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

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

static int parse_args(int argc, char **argv, grep_opts_t *o) {
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
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) { printf("ambil %s (simd=%s)\n", AMBIL_VERSION, lf_simd_active_name()); free(pos); exit(0); }
        if (strcmp(a, "--simd-info") == 0) { printf("simd=%s\n", lf_simd_active_name()); free(pos); exit(0); }

        if (strcmp(a, "-i") == 0 || strcmp(a, "--ignore-case") == 0)   { o->ignore_case = 1; continue; }
        if (strcmp(a, "-F") == 0 || strcmp(a, "--fixed-strings") == 0) { o->fixed_strings = 1; o->extended_regexp = 0; continue; }
        if (strcmp(a, "-E") == 0 || strcmp(a, "--extended-regexp") == 0) { o->extended_regexp = 1; o->fixed_strings = 0; continue; }
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
        if (strcmp(a, "-c") == 0 || strcmp(a, "--count") == 0)         { o->mode = OUT_COUNT; continue; }
        if (strcmp(a, "-l") == 0 || strcmp(a, "--files-with-matches") == 0) { o->mode = OUT_FILES_WITH; continue; }
        if (strcmp(a, "--no-color") == 0)                              { o->color = COLOR_NEVER; continue; }
        if (strcmp(a, "--stream") == 0)                                { o->stream = 1; continue; }

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
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "ambil: unknown option '%s' (try --help)\n", a);
            free(pos); return -1;
        }
        pos[npos++] = a;
    }

    /* Resolve positionals: first positional is pattern (unless -e was used,
     * in which case ALL positionals are paths). Remaining are paths. */
    if (o->n_patterns == 0) {
        if (npos == 0) {
            fprintf(stderr, "ambil: missing PATTERN (try --help)\n");
            free(pos); return -1;
        }
        lf_options_add_pattern(o, pos[0]);
        for (int k = 1; k < npos; k++) lf_options_add_path(o, pos[k]);
    } else {
        for (int k = 0; k < npos; k++) lf_options_add_path(o, pos[k]);
    }

    if (o->n_paths == 0) lf_options_add_path(o, ".");

    if (o->threads <= 0) o->threads = lf_detect_cpus();
    free(pos);
    return 0;
}

static int want_color(color_mode_t m) {
    if (m == COLOR_NEVER)  return 0;
    if (m == COLOR_ALWAYS) return 1;
    return isatty(fileno(stdout)) ? 1 : 0;
}

/* ---------- Grep mode ---------- */

typedef struct {
    const grep_opts_t   *opt;
    const lf_patterns_t *ps;
    int                  color;
} grep_worker_ctx_t;

static int grep_worker(const char *path, const char *rel_path,
                       void *vctx, lf_buf_t *buf, lf_file_stats_t *stats) {
    grep_worker_ctx_t *ctx = (grep_worker_ctx_t *)vctx;
    if (ctx->opt->stream) buf->stream = 1;
    return lf_grep_path(path, rel_path, ctx->opt, ctx->ps, ctx->color, buf, stats);
}

/* Chunk worker for intra-file parallelism. Counts newlines in [0, chunk_off)
 * to translate chunk-relative line numbers into file-global line numbers,
 * then runs lf_grep_buffer_chunk with that base. */
static int grep_chunk_worker(const char *path, const char *rel_path,
                             const char *data, size_t total_size,
                             size_t chunk_off, size_t chunk_len,
                             int chunk_idx, int chunk_total,
                             void *vctx, lf_buf_t *buf, lf_file_stats_t *stats) {
    (void)path; (void)chunk_total;
    grep_worker_ctx_t *ctx = (grep_worker_ctx_t *)vctx;
    if (ctx->opt->stream) buf->stream = 1;

    uint64_t base = 0;
    if (chunk_off > 0) {
        const char *p   = data;
        const char *end = data + chunk_off;
        while (p < end) {
            const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            base++;
            p = nl + 1;
        }
    }
    int suppress = (chunk_idx > 0);
    return lf_grep_buffer_chunk(data, total_size, chunk_off, chunk_len,
                                 rel_path, ctx->opt, ctx->ps, ctx->color,
                                 buf, stats, base, suppress);
}

static int run_grep_mode(grep_opts_t *opt) {
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
                                   opt->recursive, opt->max_depth, opt->follow_symlinks, 0);

    grep_worker_ctx_t gctx = { opt, &ps, color };
    lf_dispatch_stats_t stats;

    /* Intra-file chunking is safe for OUT_TEXT only — other modes have
     * per-file headers/footers/counts that don't compose across chunks.
     * Stream mode also disables chunking (output ordering already lost). */
    size_t chunk_threshold = 0;
    size_t chunk_bytes     = 0;
    if (opt->mode == OUT_TEXT && !opt->stream) {
        chunk_threshold = 16 * 1024 * 1024;
        chunk_bytes     =  4 * 1024 * 1024;
    }
    int rc = lf_dispatch_files_chunked(w, grep_worker, grep_chunk_worker,
                                        &gctx, opt->threads,
                                        chunk_threshold, chunk_bytes, &stats);

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

/* ---------- Subcommand dispatch ---------- */

typedef enum {
    SC_GREP = 0,
    SC_CAT,
    SC_LS,
    SC_FIND,
    SC_WC,
    SC_FILE,
    SC_ENV,
    SC_HELP,
    SC_UNKNOWN
} subcommand_t;

typedef struct {
    const char *name;
    subcommand_t cmd;
} sc_entry_t;

static const sc_entry_t sc_table[] = {
    {"grep",  SC_GREP},
    {"cat",   SC_CAT},
    {"ls",    SC_LS},
    {"find",  SC_FIND},
    {"wc",    SC_WC},
    {"file",  SC_FILE},
    {"env",   SC_ENV},
    {"help",  SC_HELP},
    {NULL,    SC_UNKNOWN}
};

static subcommand_t lookup_subcommand(const char *name) {
    for (const sc_entry_t *e = sc_table; e->name; e++) {
        if (strcmp(name, e->name) == 0) return e->cmd;
    }
    return SC_UNKNOWN;
}

static void subcommand_help(FILE *out) {
    fprintf(out, "ambil " AMBIL_VERSION " — token-efficient filesystem tools for AI agents\n");
    lf_help_spacer(out);
    lf_help_usage(out, "ambil", NULL, "--cmd <subcommand> [options] [args...]");
    lf_help_spacer(out);
    lf_help_desc(out, "Without --cmd, ambil defaults to grep mode (backward compatible):");
    lf_help_desc(out, "ambil [OPTIONS] PATTERN [PATH...]");
    lf_help_spacer(out);
    lf_help_section(out, "Available subcommands");
    lf_help_flag(out, "grep", "recursive search (default)");
    lf_help_flag(out, "cat",  "file reading with line/byte ranges");
    lf_help_flag(out, "ls",   "directory listing");
    lf_help_flag(out, "find", "filtered recursive listing");
    lf_help_flag(out, "wc",   "line/word/byte counting");
    lf_help_flag(out, "file", "file type detection");
    lf_help_flag(out, "env",  "capability probe");
    lf_help_flag(out, "help", "this overview");
    lf_help_spacer(out);
    lf_help_section(out, "Global options (before or after --cmd)");
    lf_help_flag(out, "--json",            "NDJSON structured output");
    lf_help_flag(out, "--compact",         "token-efficient grouped output");
    lf_help_flag(out, "--no-color",        "disable color output");
    lf_help_flag(out, "-j, --threads N",   "worker threads (default = CPU count)");
    lf_help_flag(out, "-h, --help",        "show help for current mode");
    lf_help_flag(out, "-V, --version",     "show version");
}

static int dispatch_subcommand(const char *name, int argc, char **argv, int cmd_idx) {
    subcommand_t sc = lookup_subcommand(name);

    int nargs = argc - 2;
    char **sub_args = (char **)lf_xmalloc(sizeof(char *) * (size_t)(nargs + 1));
    int ai = 0;
    sub_args[ai++] = argv[0];
    for (int k = 1; k < cmd_idx; k++)        sub_args[ai++] = argv[k];
    for (int k = cmd_idx + 2; k < argc; k++) sub_args[ai++] = argv[k];

    switch (sc) {
    case SC_GREP: {
        grep_opts_t opt;
        int rc = parse_args(nargs, sub_args, &opt);
        free(sub_args);
        if (rc != 0) return 2;
        rc = run_grep_mode(&opt);
        lf_options_free(&opt);
        return rc;
    }
    case SC_HELP:
        free(sub_args);
        subcommand_help(stdout);
        return 0;
    case SC_ENV:
        free(sub_args);
        lf_env_print_text(stdout);
        return 0;
    case SC_CAT: {
        int rc = run_cat(nargs, sub_args);
        free(sub_args);
        return rc;
    }
    case SC_WC: {
        int rc = run_wc(nargs, sub_args);
        free(sub_args);
        return rc;
    }
	case SC_LS: {
		int rc = run_ls(nargs, sub_args);
		free(sub_args);
		return rc;
	}
	case SC_FIND: {
		int rc = run_find(nargs, sub_args);
		free(sub_args);
		return rc;
	}
	case SC_FILE: {
        /* `ambil --cmd file [--json] PATH...` — print detected file type per path. */
        int json = 0;
        int first_path = 0;
        for (int k = 0; k < nargs; k++) {
            if (strcmp(sub_args[k], "--json") == 0) { json = 1; continue; }
            if (strcmp(sub_args[k], "-h") == 0 || strcmp(sub_args[k], "--help") == 0) {
                printf("Usage: ambil --cmd file [--json] PATH...\n"
                       "Detect file type from content (magic bytes + shebang + signatures).\n");
                free(sub_args);
                return 0;
            }
            first_path = k;
            break;
        }
        int rc_any = 0;
        for (int k = first_path; k < nargs; k++) {
            const char *p = sub_args[k];
            lf_mmap_t m;
            lf_filetype_t ft;
            if (lf_mmap_open(p, &m) == 0) {
                size_t peek = m.size < 8192 ? m.size : 8192;
                ft = lf_filetype_detect(m.data, peek, p);
                lf_mmap_close(&m);
            } else {
                fprintf(stderr, "ambil: cannot open '%s'\n", p);
                rc_any = 2;
                continue;
            }
            if (json) {
                printf("{\"path\":\"%s\",\"type\":\"%s\"}\n", p, lf_filetype_name(ft));
            } else {
                printf("%s: %s\n", p, lf_filetype_name(ft));
            }
        }
        free(sub_args);
        return rc_any;
    }
    case SC_UNKNOWN:
    default:
        free(sub_args);
        fprintf(stderr, "ambil: unknown subcommand '%s' (try --cmd help)\n", name);
        return 2;
    }
}

int main(int argc, char **argv) {
    lf_search_runtime_init();
    lf_setup_cancel_signal();
    int cmd_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cmd") == 0) { cmd_idx = i; break; }
    }

    if (cmd_idx >= 0) {
        if (cmd_idx + 1 >= argc) {
            fprintf(stderr, "ambil: --cmd requires a subcommand name (try --cmd help)\n");
            return 2;
        }
        return dispatch_subcommand(argv[cmd_idx + 1], argc, argv, cmd_idx);
    }

    grep_opts_t opt;
    if (parse_args(argc, argv, &opt) != 0) { lf_options_free(&opt); return 2; }

    int rc = run_grep_mode(&opt);

    lf_options_free(&opt);
    return rc;
}
