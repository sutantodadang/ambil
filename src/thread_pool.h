/*
 * thread_pool.h — work dispatch for grep + legacy log-mode chunk scan.
 *
 * Two modes:
 *   - lf_dispatch_files:  drive a walker, run grep on each file, and emit
 *     per-file output buffers in submission order. Workers parallelise
 *     across files.
 *   - lf_run_parallel:    legacy single-file chunked scan (log-mode kept
 *     for back-compat tests + bench).
 */
#ifndef AMBIL_THREAD_POOL_H
#define AMBIL_THREAD_POOL_H

#include "ambil.h"
#include "filter.h"
#include "aggregate.h"
#include "util.h"
#include "grep.h"
#include "walker.h"

#include <stddef.h>
#include <stdint.h>

/* Aggregate stats across all dispatched files. */
typedef struct {
    uint64_t matched_lines;
    uint64_t matches;
    uint64_t files_with_matches;
    uint64_t searched_files;
    uint64_t bytes_searched;
    uint64_t errors;            /* per-file open/read failures */
} lf_dispatch_stats_t;

int lf_dispatch_files(lf_walker_t *w,
                      const options_t *opt,
                      const lf_patterns_t *ps,
                      int color, int show_filename,
                      lf_dispatch_stats_t *out_stats);

/* ---------- Legacy chunked single-file scan (log mode) ---------- */
typedef struct {
    lf_buf_t  out;
    lf_agg_t  agg;
    uint64_t  matched;
    uint64_t  scanned;
} lf_worker_out_t;

int lf_run_parallel(const char *data, size_t size,
                    int threads,
                    const lf_filter_t *f,
                    int count_mode,
                    const char *count_field,
                    int color_matches,
                    lf_worker_out_t *outs);

#endif /* AMBIL_THREAD_POOL_H */
