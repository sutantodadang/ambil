/*
 * grep.h — per-file search execution.
 *
 * The grep module owns the multi-pattern searcher set and the output
 * formatter. It mmaps a file (or accepts pre-mapped memory) and produces
 * a self-contained output buffer plus per-file stats. The walker drives
 * one call per file; the dispatcher (thread_pool) parallelises across
 * files for many-file workloads or across chunks for single huge files.
 */
#ifndef AMBIL_GREP_H
#define AMBIL_GREP_H

#include "ambil.h"
#include "search.h"
#include "util.h"

#include <stddef.h>
#include <stdint.h>

/* Compiled pattern set. Built once, shared read-only across workers. */
typedef struct {
    lf_searcher_t *searchers;
    size_t         n;
    int            ignore_case;
    int            word_regexp;
    int            invert;
    int            only_matching;
} lf_patterns_t;

int  lf_patterns_build(lf_patterns_t *ps, const options_t *o);
void lf_patterns_free (lf_patterns_t *ps);

/* Per-file scan stats. */
typedef struct {
    uint64_t matched_lines;
    uint64_t bytes_searched;
    uint64_t searches;        /* always 1 when invoked by single-file path */
    int      had_match;
} lf_file_stats_t;

/*
 * Run search over [data, data+size) and emit formatted output to `out`.
 *
 *  - rel_path: path string used in output (relative or absolute as supplied).
 *  - opt:      options (output mode, context, etc.).
 *  - ps:       compiled pattern set.
 *  - color:    apply ANSI colors (independent of opt->color so caller can
 *              decide based on TTY at runtime).
 *  - stats:    populated with results.
 *
 * Returns 0 on success.
 */
int lf_grep_buffer(const char *data, size_t size,
                   const char *rel_path,
                   const options_t *opt, const lf_patterns_t *ps,
                   int color, lf_buf_t *out, lf_file_stats_t *stats);

/* Convenience: open + mmap path, run, unmap. */
int lf_grep_path(const char *path, const char *rel_path,
                 const options_t *opt, const lf_patterns_t *ps,
                 int color, lf_buf_t *out, lf_file_stats_t *stats);

#endif /* AMBIL_GREP_H */
