/*
 * thread_pool.c — file-level work dispatch with optional intra-file chunking.
 *
 * Job queue (§4.2): Vyukov bounded MPMC ring with per-slot sequence numbers.
 * Producer (single dispatcher thread) and consumers (workers) coordinate via
 * C11 atomics on a fixed-size slot array; the mutex is only entered to sleep
 * when the queue is empty (consumer) or full (producer). For uncontended
 * workloads the hot path is wait-free.
 *
 * Reference: D. Vyukov, "Bounded MPMC queue" (intel.com / 2011).
 *
 * Chunking (§4.1): when chunk_worker is set and a file's size >= threshold,
 * the dispatcher mmaps once, splits into N line-aligned chunks, and pushes
 * N sub-jobs sharing the mmap (refcounted). Sub-jobs get sequential seq
 * numbers so the in-order emit ring prints chunks in order.
 */
#include "thread_pool.h"
#include "file_reader.h"
#include "util.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared mmap + reference-count for a file split into chunks. Touched only
 * by the dispatcher thread (under D.sleep_mu when emitting). */
typedef struct shared_mmap_s {
    lf_mmap_t mm;
    int       chunks_remaining; /* dec on each chunk emit; close at 0 */
    int       any_match;        /* OR of per-chunk had_match */
} shared_mmap_t;

typedef struct file_job_s {
    char           *path;
    char           *rel_path;
    uint64_t        seq;
    lf_buf_t        out;
    lf_file_stats_t stats;
    int             done;
    int             rc;

    /* Chunking. shared_mm == NULL means whole-file job. */
    shared_mmap_t  *shared_mm;
    size_t          chunk_off;
    size_t          chunk_len;
    int             chunk_idx;
    int             chunk_total;
} file_job_t;

/* ---- Vyukov MPMC bounded queue ----------------------------------------- */

typedef struct {
    _Atomic uint64_t seq;
    file_job_t      *value;
} jq_slot_t;

typedef struct {
    _Atomic uint64_t head;     /* producer position */
    _Atomic uint64_t tail;     /* consumer position */
    size_t           mask;     /* cap - 1 (cap is power of two) */
    jq_slot_t       *slots;

    /* Sleep machinery — only entered on full (producer) or empty (consumers). */
    pthread_mutex_t  sleep_mu;
    pthread_cond_t   cv_work;
    pthread_cond_t   cv_space;
    _Atomic int      closed;
} lf_jobqueue_t;

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static void jq_init(lf_jobqueue_t *q, size_t cap) {
    cap = next_pow2(cap);
    q->mask  = cap - 1;
    q->slots = (jq_slot_t *)lf_xcalloc(cap, sizeof(*q->slots));
    for (size_t i = 0; i < cap; i++) atomic_init(&q->slots[i].seq, (uint64_t)i);
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    atomic_init(&q->closed, 0);
    pthread_mutex_init(&q->sleep_mu, NULL);
    pthread_cond_init (&q->cv_work,  NULL);
    pthread_cond_init (&q->cv_space, NULL);
}

static void jq_destroy(lf_jobqueue_t *q) {
    free(q->slots);
    pthread_mutex_destroy(&q->sleep_mu);
    pthread_cond_destroy (&q->cv_work);
    pthread_cond_destroy (&q->cv_space);
}

/* Mark queue closed; wake all sleepers. After close, push fails; pop returns
 * NULL once drained. */
static void jq_close(lf_jobqueue_t *q) {
    pthread_mutex_lock(&q->sleep_mu);
    atomic_store_explicit(&q->closed, 1, memory_order_relaxed);
    pthread_cond_broadcast(&q->cv_work);
    pthread_cond_broadcast(&q->cv_space);
    pthread_mutex_unlock(&q->sleep_mu);
}

/* SPSC-from-producer push. Blocks if full until space frees or close. Returns
 * 0 on success, -1 on closed-while-full. */
static int jq_push(lf_jobqueue_t *q, file_job_t *job) {
    for (;;) {
        uint64_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
        jq_slot_t *s = &q->slots[h & q->mask];
        uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)(seq - h);
        if (diff == 0) {
            /* slot ready for write */
            s->value = job;
            atomic_store_explicit(&s->seq, h + 1, memory_order_release);
            atomic_store_explicit(&q->head, h + 1, memory_order_relaxed);
            /* Wake one consumer. Even with no waiters, the lock+signal is cheap
             * and avoids missed-wakeup races. */
            pthread_mutex_lock(&q->sleep_mu);
            pthread_cond_signal(&q->cv_work);
            pthread_mutex_unlock(&q->sleep_mu);
            return 0;
        }
        /* diff < 0: full. Sleep until a consumer advances the slot. */
        pthread_mutex_lock(&q->sleep_mu);
        seq = atomic_load_explicit(&s->seq, memory_order_acquire);
        if ((intptr_t)(seq - h) != 0
            && !atomic_load_explicit(&q->closed, memory_order_relaxed)) {
            pthread_cond_wait(&q->cv_space, &q->sleep_mu);
        }
        int closed = atomic_load_explicit(&q->closed, memory_order_relaxed);
        pthread_mutex_unlock(&q->sleep_mu);
        if (closed) return -1;
    }
}

/* MPMC pop. Blocks on empty until push or close. Returns NULL only when the
 * queue is closed AND empty. */
static file_job_t *jq_pop(lf_jobqueue_t *q) {
    for (;;) {
        uint64_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
        jq_slot_t *s = &q->slots[t & q->mask];
        uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)(seq - (t + 1));
        if (diff == 0) {
            /* slot has a value at this tail. Claim via CAS. */
            if (atomic_compare_exchange_weak_explicit(
                    &q->tail, &t, t + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                file_job_t *job = s->value;
                /* Mark slot ready for producer's next round. */
                atomic_store_explicit(&s->seq, t + q->mask + 1, memory_order_release);
                pthread_mutex_lock(&q->sleep_mu);
                pthread_cond_signal(&q->cv_space);
                pthread_mutex_unlock(&q->sleep_mu);
                return job;
            }
            /* CAS lost (another consumer beat us); retry. */
            continue;
        }
        if (diff < 0) {
            /* empty — sleep if not closed. */
            if (atomic_load_explicit(&q->closed, memory_order_relaxed)) {
                /* Recheck head: producer might have published right before close. */
                uint64_t h = atomic_load_explicit(&q->head, memory_order_acquire);
                if (atomic_load_explicit(&q->tail, memory_order_relaxed) >= h) return NULL;
                continue;
            }
            pthread_mutex_lock(&q->sleep_mu);
            seq = atomic_load_explicit(&s->seq, memory_order_acquire);
            if ((intptr_t)(seq - (t + 1)) < 0
                && !atomic_load_explicit(&q->closed, memory_order_relaxed)) {
                pthread_cond_wait(&q->cv_work, &q->sleep_mu);
            }
            pthread_mutex_unlock(&q->sleep_mu);
            continue;
        }
        /* diff > 0: tail moved under us; retry. */
    }
}

/* ---- dispatcher --------------------------------------------------------- */

typedef struct {
    lf_jobqueue_t            q;

    lf_file_worker_fn        worker;
    lf_file_chunk_worker_fn  chunk_worker;
    void                    *ctx;

    /* For dispatcher waiting on done; workers signal here after completion.
     * Separate from q.sleep_mu so producer's space-wait and dispatcher's
     * done-wait don't contend. */
    pthread_mutex_t          done_mu;
    pthread_cond_t           cv_done;
} dispatch_t;

static void *dispatch_worker(void *vp) {
    dispatch_t *D = (dispatch_t *)vp;
    for (;;) {
        file_job_t *job = jq_pop(&D->q);
        if (!job) break;

        lf_buf_init(&job->out);
        memset(&job->stats, 0, sizeof(job->stats));
        if (job->shared_mm && D->chunk_worker) {
            job->rc = D->chunk_worker(job->path, job->rel_path,
                                       job->shared_mm->mm.data,
                                       job->shared_mm->mm.size,
                                       job->chunk_off, job->chunk_len,
                                       job->chunk_idx, job->chunk_total,
                                       D->ctx, &job->out, &job->stats);
        } else {
            job->rc = D->worker(job->path, job->rel_path, D->ctx,
                                 &job->out, &job->stats);
        }

        pthread_mutex_lock(&D->done_mu);
        job->done = 1;
        pthread_cond_broadcast(&D->cv_done);
        pthread_mutex_unlock(&D->done_mu);
    }
    return NULL;
}

/* Emit a completed job's output + accumulate stats. For chunks, defer the
 * per-file counts until the last chunk's emit. */
static void emit_and_free(file_job_t *job, lf_dispatch_stats_t *out_stats) {
    if (job->out.len) fwrite(job->out.data, 1, job->out.len, stdout);

    out_stats->matched_lines  += job->stats.matched_lines;
    out_stats->matches        += job->stats.matched_lines;
    out_stats->bytes_searched += job->stats.bytes_searched;

    if (job->shared_mm) {
        shared_mmap_t *sm = job->shared_mm;
        sm->any_match |= job->stats.had_match;
        sm->chunks_remaining--;
        if (sm->chunks_remaining == 0) {
            out_stats->searched_files  += 1;
            if (sm->any_match) out_stats->files_with_matches += 1;
            lf_mmap_close(&sm->mm);
            free(sm);
        }
    } else {
        out_stats->searched_files += 1;
        if (job->stats.had_match) out_stats->files_with_matches += 1;
    }
    if (job->rc != 0) out_stats->errors += 1;

    lf_buf_free(&job->out);
    free(job->path);
    free(job->rel_path);
    free(job);
}

/* Compute up to max_chunks line-aligned [off, len) pairs covering [0, size). */
static int compute_chunks(const char *data, size_t size,
                          size_t chunk_bytes, int max_chunks,
                          size_t *off_out, size_t *len_out) {
    if (size == 0 || chunk_bytes == 0) {
        off_out[0] = 0; len_out[0] = size;
        return 1;
    }
    size_t target = (size + chunk_bytes - 1) / chunk_bytes;
    if (target < 1) target = 1;
    if ((int)target > max_chunks) target = (size_t)max_chunks;
    int n = (int)target;
    if (n > 64) n = 64;

    size_t boundaries[64 + 1];
    size_t step = size / (size_t)n;
    if (step == 0) step = size;
    boundaries[0] = 0;
    boundaries[n] = size;
    for (int i = 1; i < n; i++) {
        size_t raw = (size_t)i * step;
        if (raw >= size) { boundaries[i] = size; continue; }
        const char *nl = (const char *)memchr(data + raw, '\n', size - raw);
        boundaries[i] = nl ? (size_t)(nl - data) + 1 : size;
    }
    int real = 0;
    for (int i = 0; i < n; i++) {
        size_t off = boundaries[i];
        size_t end = boundaries[i + 1];
        if (i + 1 < n && end <= off) continue;
        off_out[real] = off;
        len_out[real] = end - off;
        real++;
    }
    if (real == 0) { off_out[0] = 0; len_out[0] = size; real = 1; }
    return real;
}

int lf_dispatch_files_chunked(lf_walker_t *w,
                              lf_file_worker_fn worker,
                              lf_file_chunk_worker_fn chunk_worker,
                              void *ctx,
                              int threads,
                              size_t chunk_threshold,
                              size_t chunk_bytes,
                              lf_dispatch_stats_t *out_stats) {
    memset(out_stats, 0, sizeof(*out_stats));

    int nthreads = threads > 0 ? threads : lf_detect_cpus();
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    const size_t WINDOW = (size_t)nthreads * 8 + 64;
    const size_t QUEUE_CAP = WINDOW * 2; /* slack so producer rarely blocks */

    dispatch_t D;
    memset(&D, 0, sizeof(D));
    jq_init(&D.q, QUEUE_CAP);
    pthread_mutex_init(&D.done_mu, NULL);
    pthread_cond_init (&D.cv_done, NULL);
    D.worker       = worker;
    D.chunk_worker = chunk_worker;
    D.ctx          = ctx;

    int chunking_enabled = (chunk_worker != NULL && chunk_threshold > 0 && nthreads > 1);
    if (chunk_bytes == 0) chunk_bytes = 4 * 1024 * 1024;

    pthread_t *tids = (pthread_t *)lf_xcalloc((size_t)nthreads, sizeof(pthread_t));
    int spawned = 0;
    for (int t = 0; t < nthreads; t++) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, dispatch_worker, &D) == 0) {
            tids[spawned++] = tid;
        }
    }

    file_job_t **ring = (file_job_t **)lf_xcalloc(WINDOW, sizeof(*ring));
    size_t head_seq = 0;
    size_t next_seq = 0;
    int    walker_done = 0;
    int    inline_mode = (spawned == 0);

    while (!walker_done || head_seq < next_seq) {
        while (!walker_done && (next_seq - head_seq) < WINDOW) {
            lf_walk_entry_t we;
            if (!lf_walk_next(w, &we)) { walker_done = 1; break; }

            int            do_chunk = 0;
            shared_mmap_t *sm       = NULL;
            size_t         offs[64];
            size_t         lens[64];
            int            n_chunks = 1;

            if (chunking_enabled && we.st.is_file && we.st.size >= chunk_threshold) {
                sm = (shared_mmap_t *)lf_xcalloc(1, sizeof(*sm));
                if (lf_mmap_open(we.path, &sm->mm) == 0 && sm->mm.size > 0) {
                    int max_chunks = nthreads * 4;
                    if (max_chunks > 64) max_chunks = 64;
                    n_chunks = compute_chunks(sm->mm.data, sm->mm.size,
                                              chunk_bytes, max_chunks,
                                              offs, lens);
                    sm->chunks_remaining = n_chunks;
                    sm->any_match        = 0;
                    do_chunk = (n_chunks > 1);
                    if (!do_chunk) {
                        lf_mmap_close(&sm->mm);
                        free(sm);
                        sm = NULL;
                    }
                } else {
                    free(sm);
                    sm = NULL;
                }
            }

            int total_jobs = do_chunk ? n_chunks : 1;
            for (int c = 0; c < total_jobs; c++) {
                /* Drain head if window full mid-fanout. */
                while (!inline_mode && (next_seq - head_seq) >= WINDOW) {
                    file_job_t *hjob = ring[head_seq % WINDOW];
                    pthread_mutex_lock(&D.done_mu);
                    while (!hjob->done) pthread_cond_wait(&D.cv_done, &D.done_mu);
                    pthread_mutex_unlock(&D.done_mu);
                    ring[head_seq % WINDOW] = NULL;
                    head_seq++;
                    emit_and_free(hjob, out_stats);
                }

                file_job_t *job = (file_job_t *)lf_xcalloc(1, sizeof(*job));
                job->path     = lf_xstrdup(we.path);
                job->rel_path = lf_xstrdup(we.rel_path && we.rel_path[0] ? we.rel_path : we.path);
                job->seq      = next_seq;
                if (do_chunk) {
                    job->shared_mm   = sm;
                    job->chunk_off   = offs[c];
                    job->chunk_len   = lens[c];
                    job->chunk_idx   = c;
                    job->chunk_total = n_chunks;
                }
                ring[next_seq % WINDOW] = job;
                next_seq++;

                if (inline_mode) {
                    lf_buf_init(&job->out);
                    memset(&job->stats, 0, sizeof(job->stats));
                    if (job->shared_mm) {
                        job->rc = chunk_worker(job->path, job->rel_path,
                                                sm->mm.data, sm->mm.size,
                                                job->chunk_off, job->chunk_len,
                                                job->chunk_idx, job->chunk_total,
                                                ctx, &job->out, &job->stats);
                    } else {
                        job->rc = worker(job->path, job->rel_path, ctx,
                                          &job->out, &job->stats);
                    }
                    job->done = 1;
                } else {
                    jq_push(&D.q, job);
                }
            }
        }

        if (head_seq < next_seq) {
            file_job_t *job = ring[head_seq % WINDOW];
            if (!inline_mode) {
                pthread_mutex_lock(&D.done_mu);
                while (!job->done) pthread_cond_wait(&D.cv_done, &D.done_mu);
                pthread_mutex_unlock(&D.done_mu);
            }
            ring[head_seq % WINDOW] = NULL;
            head_seq++;
            emit_and_free(job, out_stats);
        }
    }

    jq_close(&D.q);
    for (int t = 0; t < spawned; t++) pthread_join(tids[t], NULL);
    free(tids);
    free(ring);

    jq_destroy(&D.q);
    pthread_mutex_destroy(&D.done_mu);
    pthread_cond_destroy (&D.cv_done);
    return 0;
}

int lf_dispatch_files(lf_walker_t *w,
                      lf_file_worker_fn worker, void *ctx,
                      int threads,
                      lf_dispatch_stats_t *out_stats) {
    return lf_dispatch_files_chunked(w, worker, NULL, ctx, threads, 0, 0, out_stats);
}
