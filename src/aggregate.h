/*
 * aggregate.h — open-addressed hashmap for value -> count.
 *
 * Designed for the `--count <field>` workflow:
 *   - Per-thread maps avoid lock contention (each worker writes to its own).
 *   - Final merge sums counts into a single map for printing.
 *   - Keys are owned copies (one allocation per unique value) since the
 *     mmapped source can be unmapped before printing.
 *
 * Hash: FNV-1a (cheap, good enough for short ASCII keys).
 * Probing: linear, with 0.75 max load factor before doubling.
 */
#ifndef AMBIL_AGGREGATE_H
#define AMBIL_AGGREGATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    char    *key;       /* malloc'd, NUL terminated */
    size_t   klen;
    uint64_t count;
    uint64_t hash;      /* cached for resize */
    int      used;      /* slot in use */
} lf_agg_slot_t;

typedef struct {
    lf_agg_slot_t *slots;
    size_t         cap;       /* power of two */
    size_t         size;      /* live slots */
} lf_agg_t;

void lf_agg_init(lf_agg_t *a);
void lf_agg_free(lf_agg_t *a);

/* Add `n` to the count for the given (non-NUL-terminated) key. */
void lf_agg_add(lf_agg_t *a, const char *key, size_t klen, uint64_t n);

/* Merge `src` into `dst` (dst absorbs src's counts). */
void lf_agg_merge(lf_agg_t *dst, const lf_agg_t *src);

/* Print as TSV (count\tkey\n) sorted by count desc, then key asc. */
void lf_agg_print_sorted(const lf_agg_t *a, FILE *out);

#endif /* AMBIL_AGGREGATE_H */
