/*
 * thread_pool.c — work dispatch.
 *
 * Two execution paths:
 *
 *   1. lf_dispatch_files (NEW): file-level parallelism for grep mode.
 *      Walker enumerates files into a FIFO queue; N worker threads pop a
 *      job, mmap+scan, and store the formatted output buffer back on the
 *      job. The producer thread waits in submission-order for each job to
 *      finish, then writes the buffer to stdout. This preserves stable
 *      output ordering while allowing maximum scan parallelism.
 *
 *   2. lf_run_parallel (LEGACY): single-file chunked scan for log mode.
 *      Slices the mmap into N line-aligned ranges; each worker fills a
 *      per-thread output buffer (or aggregation map). Output is reordered
 *      by chunk id at the end. Kept verbatim from v0.1 for back-compat.
 */
#include "thread_pool.h"
#include "filter.h"
#include "parser.h"
#include "search.h"
#include "util.h"
#include "grep.h"
#include "walker.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- legacy log-mode chunk scan ---------- */

#define ANSI_HL    "\x1b[1;31m"
#define ANSI_RESET "\x1b[0m"

typedef struct {
    const char *data;
    size_t      file_size;
    size_t      start;
    size_t      end;
    const lf_filter_t *f;
    int         count_mode;
    const char *count_field;
    size_t      count_field_len;
    int         color;
    lf_worker_out_t *out;
} chunk_arg_t;

static void emit_line(chunk_arg_t *a, const char *line, size_t llen) {
    lf_buf_t *b = &a->out->out;
    if (a->color && a->f->has_search) {
        const lf_searcher_t *s = &a->f->searcher;
        size_t plen = s->plen;
        const char *p = line;
        size_t remaining = llen;
        while (remaining >= plen) {
            const char *m = lf_search_find(s, p, remaining);
            if (!m) break;
            lf_buf_append(b, p, (size_t)(m - p));
            lf_buf_append(b, ANSI_HL, sizeof(ANSI_HL) - 1);
            lf_buf_append(b, m, plen);
            lf_buf_append(b, ANSI_RESET, sizeof(ANSI_RESET) - 1);
            size_t consumed = (size_t)(m - p) + plen;
            p += consumed;
            remaining -= consumed;
        }
        lf_buf_append(b, p, remaining);
    } else {
        lf_buf_append(b, line, llen);
    }
    lf_buf_append(b, "\n", 1);
}

static void *chunk_worker(void *vp) {
    chunk_arg_t *a = (chunk_arg_t *)vp;
    const char *data = a->data;
    size_t fs = a->file_size;
    size_t i = a->start;
    if (i > 0) {
        const char *nl = (const char *)memchr(data + i, '\n', fs - i);
        if (!nl) return NULL;
        i = (size_t)(nl - data) + 1;
    }
    size_t end = a->end;
    if (end < fs) {
        const char *nl = (const char *)memchr(data + end, '\n', fs - end);
        end = nl ? (size_t)(nl - data) + 1 : fs;
    } else {
        end = fs;
    }
    while (i < end) {
        const char *nl = (const char *)memchr(data + i, '\n', end - i);
        size_t line_end = nl ? (size_t)(nl - data) : end;
        size_t llen = line_end - i;
        size_t eff_len = llen;
        if (eff_len > 0 && data[i + eff_len - 1] == '\r') eff_len--;
        const char *line = data + i;
        a->out->scanned++;
        if (lf_filter_match(a->f, line, eff_len)) {
            a->out->matched++;
            if (a->count_mode) {
                const char *vp_;
                size_t vlen;
                if (lf_json_get_field(line, eff_len,
                                      a->count_field, a->count_field_len,
                                      &vp_, &vlen) == 0) {
                    lf_agg_add(&a->out->agg, vp_, vlen, 1);
                }
            } else {
                emit_line(a, line, eff_len);
            }
        }
        i = nl ? (size_t)(nl - data) + 1 : end;
    }
    return NULL;
}

int lf_run_parallel(const char *data, size_t size,
                    int threads,
                    const lf_filter_t *f,
                    int count_mode,
                    const char *count_field,
                    int color_matches,
                    lf_worker_out_t *outs) {
    if (threads < 1) threads = 1;
    if ((size_t)threads > size / (1 << 16) + 1) {
        threads = (int)(size / (1 << 16) + 1);
        if (threads < 1) threads = 1;
    }
    pthread_t   *tids = (pthread_t *)lf_xcalloc((size_t)threads, sizeof(pthread_t));
    chunk_arg_t *args = (chunk_arg_t *)lf_xcalloc((size_t)threads, sizeof(chunk_arg_t));

    size_t base = size / (size_t)threads;
    size_t rem  = size % (size_t)threads;
    size_t off  = 0;
    size_t cf_len = count_field ? strlen(count_field) : 0;

    for (int t = 0; t < threads; t++) {
        size_t sz = base + (t < (int)rem ? 1 : 0);
        args[t].data            = data;
        args[t].file_size       = size;
        args[t].start           = off;
        args[t].end             = off + sz;
        args[t].f               = f;
        args[t].count_mode      = count_mode;
        args[t].count_field     = count_field;
        args[t].count_field_len = cf_len;
        args[t].color           = color_matches;
        args[t].out             = &outs[t];
        if (count_mode) lf_agg_init(&outs[t].agg);
        else            lf_buf_init(&outs[t].out);
        off += sz;
    }
    int spawned = 0;
    for (int t = 0; t < threads; t++) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, chunk_worker, &args[t]) == 0) {
            tids[spawned++] = tid;
        } else {
            /* Spawn failed — run this chunk inline so its work isn't lost. */
            chunk_worker(&args[t]);
        }
    }
    for (int t = 0; t < spawned; t++) pthread_join(tids[t], NULL);
    free(tids);
    free(args);
    return 0;
}

/* ---------- file-level dispatcher (grep mode) ----------
 *
 * Design: bounded in-flight window. The walker enumerates files in the
 * dispatcher thread; each file becomes a job, pushed onto the worker queue.
 * Workers pop jobs, scan, and mark them done. The dispatcher streams jobs
 * to stdout in submission (seq) order, draining completed jobs as soon as
 * the head of the window is ready. When the window is full, the walker
 * blocks until the head completes \u2014 this caps memory at O(window) jobs
 * + their output buffers, regardless of how many files exist on disk.
 */
typedef struct file_job_s {
    char           *path;
    char           *rel_path;
    uint64_t        seq;
    lf_buf_t        out;
    lf_file_stats_t stats;
    int             done;
    int             rc;              /* lf_grep_path return code */
    struct file_job_s *q_next;
} file_job_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv_work;
    pthread_cond_t  cv_done;
    file_job_t     *q_head;
    file_job_t     *q_tail;
    int             closed;

    const options_t     *opt;
    const lf_patterns_t *ps;
    int                  color;
} dispatch_t;

static void *dispatch_worker(void *vp) {
    dispatch_t *D = (dispatch_t *)vp;
    for (;;) {
        pthread_mutex_lock(&D->mu);
        while (!D->closed && !D->q_head) pthread_cond_wait(&D->cv_work, &D->mu);
        if (!D->q_head && D->closed) { pthread_mutex_unlock(&D->mu); break; }
        file_job_t *job = D->q_head;
        D->q_head = job->q_next;
        if (!D->q_head) D->q_tail = NULL;
        job->q_next = NULL;
        pthread_mutex_unlock(&D->mu);

        lf_buf_init(&job->out);
        job->rc = lf_grep_path(job->path, job->rel_path, D->opt, D->ps,
                               D->color, &job->out, &job->stats);

        pthread_mutex_lock(&D->mu);
        job->done = 1;
        pthread_cond_broadcast(&D->cv_done);
        pthread_mutex_unlock(&D->mu);
    }
    return NULL;
}

/* Emit a finished job and accumulate its stats, then free it. */
static void emit_and_free(file_job_t *job, lf_dispatch_stats_t *out_stats) {
    if (job->out.len) fwrite(job->out.data, 1, job->out.len, stdout);

    out_stats->matched_lines  += job->stats.matched_lines;
    out_stats->matches        += job->stats.matched_lines;
    out_stats->bytes_searched += job->stats.bytes_searched;
    out_stats->searched_files += 1;
    if (job->stats.had_match) out_stats->files_with_matches += 1;
    if (job->rc != 0)         out_stats->errors += 1;

    lf_buf_free(&job->out);
    free(job->path);
    free(job->rel_path);
    free(job);
}

int lf_dispatch_files(lf_walker_t *w,
                      const options_t *opt,
                      const lf_patterns_t *ps,
                      int color, int show_filename,
                      lf_dispatch_stats_t *out_stats) {
    (void)show_filename;
    memset(out_stats, 0, sizeof(*out_stats));

    int nthreads = opt->threads > 0 ? opt->threads : lf_detect_cpus();
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    /* In-flight window: unfinished jobs already enqueued for workers plus
     * those waiting at the head for their turn to be emitted. Sized to keep
     * every worker fed without unbounded memory growth on huge trees. */
    const size_t WINDOW = (size_t)nthreads * 4 + 16;

    dispatch_t D;
    memset(&D, 0, sizeof(D));
    pthread_mutex_init(&D.mu, NULL);
    pthread_cond_init(&D.cv_work, NULL);
    pthread_cond_init(&D.cv_done, NULL);
    D.opt = opt; D.ps = ps; D.color = color;

    /* Track only *successfully* created tids so we never join garbage. */
    pthread_t *tids = (pthread_t *)lf_xcalloc((size_t)nthreads, sizeof(pthread_t));
    int spawned = 0;
    for (int t = 0; t < nthreads; t++) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, dispatch_worker, &D) == 0) {
            tids[spawned++] = tid;
        }
    }

    /* Ring of in-flight jobs in submission order. */
    file_job_t **ring = (file_job_t **)lf_xcalloc(WINDOW, sizeof(*ring));
    size_t head_seq = 0;     /* seq of next job to emit */
    size_t next_seq = 0;     /* seq to assign to the next pushed job */
    int    walker_done = 0;
    int    inline_mode = (spawned == 0);

    while (!walker_done || head_seq < next_seq) {
        /* Try to pull more work into the window. */
        while (!walker_done && (next_seq - head_seq) < WINDOW) {
            lf_walk_entry_t we;
            if (!lf_walk_next(w, &we)) { walker_done = 1; break; }

            file_job_t *job = (file_job_t *)lf_xcalloc(1, sizeof(*job));
            job->path     = lf_xstrdup(we.path);
            job->rel_path = lf_xstrdup(we.rel_path && we.rel_path[0] ? we.rel_path : we.path);
            job->seq      = next_seq;
            ring[next_seq % WINDOW] = job;
            next_seq++;

            if (inline_mode) {
                /* No worker threads \u2014 run inline. */
                lf_buf_init(&job->out);
                job->rc = lf_grep_path(job->path, job->rel_path, opt, ps, color,
                                       &job->out, &job->stats);
                job->done = 1;
            } else {
                pthread_mutex_lock(&D.mu);
                if (D.q_tail) D.q_tail->q_next = job; else D.q_head = job;
                D.q_tail = job;
                pthread_cond_signal(&D.cv_work);
                pthread_mutex_unlock(&D.mu);
            }
        }

        /* Drain head jobs that have completed, in order. */
        if (head_seq < next_seq) {
            file_job_t *job = ring[head_seq % WINDOW];
            if (inline_mode) {
                /* Always done in inline mode. */
            } else {
                pthread_mutex_lock(&D.mu);
                while (!job->done) pthread_cond_wait(&D.cv_done, &D.mu);
                pthread_mutex_unlock(&D.mu);
            }
            ring[head_seq % WINDOW] = NULL;
            head_seq++;
            emit_and_free(job, out_stats);
        }
    }

    pthread_mutex_lock(&D.mu);
    D.closed = 1;
    pthread_cond_broadcast(&D.cv_work);
    pthread_mutex_unlock(&D.mu);

    for (int t = 0; t < spawned; t++) pthread_join(tids[t], NULL);
    free(tids);
    free(ring);

    pthread_mutex_destroy(&D.mu);
    pthread_cond_destroy(&D.cv_work);
    pthread_cond_destroy(&D.cv_done);
    return 0;
}
