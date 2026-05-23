/*
 * thread_pool.h — generic file dispatch with optional intra-file chunking.
 *
 *   lf_dispatch_files: walk a directory tree, run a caller-supplied
 *     worker callback per file (in parallel), emit per-file output
 *     buffers in submission order.
 *
 *   lf_dispatch_files_chunked: as above, but additionally splits files
 *     above `chunk_threshold` bytes into chunks of ~`chunk_bytes` aligned
 *     on '\n' boundaries. Each chunk runs the chunk_worker on a slice of
 *     a shared mmap; outputs are emitted in chunk order.
 *
 * The callbacks are opaque — no grep types leak into this header.
 */
#ifndef AMBIL_THREAD_POOL_H
#define AMBIL_THREAD_POOL_H

#include "ambil.h"
#include "util.h"
#include "walker.h"

#include <stddef.h>
#include <stdint.h>

/* Per-file worker callback invoked on worker threads.
 *   path / rel_path:  file to process
 *   ctx:              opaque caller context (e.g. grep_opts_t + patterns)
 *   buf:              output buffer (caller must lf_buf_init before use)
 *   stats:            per-file stats to update (caller must zero first)
 * Returns 0 on success, nonzero for open/read errors. */
typedef int (*lf_file_worker_fn)(const char *path, const char *rel_path,
                                  void *ctx, lf_buf_t *buf,
                                  lf_file_stats_t *stats);

/* Per-chunk worker callback. Operates on a slice of a shared mmap.
 *   data:         base pointer of the WHOLE mmapped file (chunks share)
 *   total_size:   full file size in bytes
 *   chunk_off:    byte offset of this chunk within the file (line-aligned)
 *   chunk_len:    byte length of this chunk (line-aligned)
 *   idx, total:   chunk index and total chunks for this file
 *   buf, stats:   per-chunk output buffer + stats
 * Returns 0 on success. The dispatcher aggregates per-chunk stats into a
 * single per-file count (had_match across chunks is OR'd; searched_files
 * is counted once per file at last-chunk emit). */
typedef int (*lf_file_chunk_worker_fn)(const char *path, const char *rel_path,
                                        const char *data, size_t total_size,
                                        size_t chunk_off, size_t chunk_len,
                                        int idx, int total,
                                        void *ctx, lf_buf_t *buf,
                                        lf_file_stats_t *stats);

/* Aggregate stats across all dispatched files. */
typedef struct {
    uint64_t matched_lines;
    uint64_t matches;
    uint64_t files_with_matches;
    uint64_t searched_files;
    uint64_t bytes_searched;
    uint64_t errors;
} lf_dispatch_stats_t;

/* Original single-job-per-file dispatcher. Equivalent to
 * lf_dispatch_files_chunked(..., NULL, 0, 0, ...). */
int lf_dispatch_files(lf_walker_t *w,
                      lf_file_worker_fn worker, void *ctx,
                      int threads,
                      lf_dispatch_stats_t *out_stats);

/* Chunked dispatcher. If chunk_worker is NULL OR chunk_threshold is 0, behaves
 * identically to lf_dispatch_files. Otherwise files >= chunk_threshold are
 * split into chunks of approximately chunk_bytes (aligned to '\n'). */
int lf_dispatch_files_chunked(lf_walker_t *w,
                              lf_file_worker_fn worker,
                              lf_file_chunk_worker_fn chunk_worker,
                              void *ctx,
                              int threads,
                              size_t chunk_threshold,
                              size_t chunk_bytes,
                              lf_dispatch_stats_t *out_stats);

#endif /* AMBIL_THREAD_POOL_H */
