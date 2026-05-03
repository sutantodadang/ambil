/*
 * aggregate.c — open-addressed hashmap + sorted printer.
 */
#include "aggregate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INIT_CAP   64        /* power of two */
#define LOAD_NUM   3         /* load factor 3/4 */
#define LOAD_DEN   4

static uint64_t fnv1a(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void alloc_table(lf_agg_t *a, size_t cap) {
    a->slots = (lf_agg_slot_t *)calloc(cap, sizeof(lf_agg_slot_t));
    if (!a->slots) {
        fprintf(stderr, "ambil: out of memory (agg slots %zu)\n", cap);
        abort();
    }
    a->cap  = cap;
    a->size = 0;
}

void lf_agg_init(lf_agg_t *a) {
    alloc_table(a, INIT_CAP);
}

void lf_agg_free(lf_agg_t *a) {
    if (!a || !a->slots) return;
    for (size_t i = 0; i < a->cap; i++) {
        if (a->slots[i].used) free(a->slots[i].key);
    }
    free(a->slots);
    a->slots = NULL;
    a->cap = a->size = 0;
}

/* Insert by raw hash + key span; takes ownership of `keydup` only on insert. */
static void insert_owned(lf_agg_t *a, char *keydup, size_t klen, uint64_t hash, uint64_t n) {
    size_t mask = a->cap - 1;
    size_t i = (size_t)hash & mask;
    while (a->slots[i].used) {
        if (a->slots[i].hash == hash &&
            a->slots[i].klen == klen &&
            memcmp(a->slots[i].key, keydup, klen) == 0) {
            a->slots[i].count += n;
            free(keydup);   /* duplicate; caller's copy not needed */
            return;
        }
        i = (i + 1) & mask;
    }
    a->slots[i].key   = keydup;
    a->slots[i].klen  = klen;
    a->slots[i].count = n;
    a->slots[i].hash  = hash;
    a->slots[i].used  = 1;
    a->size++;
}

static void rehash(lf_agg_t *a) {
    size_t new_cap = a->cap * 2;
    lf_agg_slot_t *old = a->slots;
    size_t old_cap = a->cap;
    alloc_table(a, new_cap);
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].used) continue;
        /* Re-insert preserving ownership; pass through keydup pointer. */
        insert_owned(a, old[i].key, old[i].klen, old[i].hash, old[i].count);
    }
    free(old);
}

void lf_agg_add(lf_agg_t *a, const char *key, size_t klen, uint64_t n) {
    if (a->size * LOAD_DEN >= a->cap * LOAD_NUM) rehash(a);

    uint64_t h = fnv1a(key, klen);
    size_t mask = a->cap - 1;
    size_t i = (size_t)h & mask;
    while (a->slots[i].used) {
        if (a->slots[i].hash == h &&
            a->slots[i].klen == klen &&
            memcmp(a->slots[i].key, key, klen) == 0) {
            a->slots[i].count += n;
            return;
        }
        i = (i + 1) & mask;
    }
    /* New entry: copy the key (caller's buffer may be freed/unmapped later). */
    char *kd = (char *)malloc(klen + 1);
    if (!kd) {
        fprintf(stderr, "ambil: out of memory (agg key %zu)\n", klen);
        abort();
    }
    memcpy(kd, key, klen);
    kd[klen] = '\0';
    a->slots[i].key   = kd;
    a->slots[i].klen  = klen;
    a->slots[i].count = n;
    a->slots[i].hash  = h;
    a->slots[i].used  = 1;
    a->size++;
}

void lf_agg_merge(lf_agg_t *dst, const lf_agg_t *src) {
    for (size_t i = 0; i < src->cap; i++) {
        if (src->slots[i].used) {
            lf_agg_add(dst, src->slots[i].key, src->slots[i].klen, src->slots[i].count);
        }
    }
}

/* qsort comparator: count desc, then lexicographic key asc. */
static int cmp_slot(const void *pa, const void *pb) {
    const lf_agg_slot_t *a = (const lf_agg_slot_t *)pa;
    const lf_agg_slot_t *b = (const lf_agg_slot_t *)pb;
    if (a->count != b->count) return (a->count < b->count) ? 1 : -1;
    size_t n = a->klen < b->klen ? a->klen : b->klen;
    int c = memcmp(a->key, b->key, n);
    if (c != 0) return c;
    if (a->klen != b->klen) return (a->klen < b->klen) ? -1 : 1;
    return 0;
}

void lf_agg_print_sorted(const lf_agg_t *a, FILE *out) {
    if (a->size == 0) return;
    lf_agg_slot_t *flat = (lf_agg_slot_t *)malloc(a->size * sizeof(lf_agg_slot_t));
    if (!flat) {
        fprintf(stderr, "ambil: out of memory (sort %zu)\n", a->size);
        abort();
    }
    size_t j = 0;
    for (size_t i = 0; i < a->cap; i++) {
        if (a->slots[i].used) flat[j++] = a->slots[i];
    }
    qsort(flat, a->size, sizeof(lf_agg_slot_t), cmp_slot);
    for (size_t i = 0; i < a->size; i++) {
        fprintf(out, "%llu\t%.*s\n",
                (unsigned long long)flat[i].count,
                (int)flat[i].klen, flat[i].key);
    }
    free(flat);
}
