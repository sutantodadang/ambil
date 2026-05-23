/*
 * ignore.h — gitignore-style filtering, default ignores, globs, type aliases.
 *
 * Lifecycle:
 *   - lf_ignore_init() builds the engine from options (default ignores,
 *     hidden policy, user -g globs, -t type filters, no_ignore flag).
 *   - During the walk, lf_ignore_push_dir() loads .gitignore/.ignore/.rgignore
 *     files in a newly-entered directory; lf_ignore_pop_dir() unwinds them
 *     when leaving. Per-directory rules apply only within that subtree.
 *   - lf_ignore_match_file() / lf_ignore_match_dir() answer YES/NO for a
 *     given relative path. Negative user globs always win; otherwise the
 *     last matching pattern wins (gitignore semantics).
 *
 * Pattern syntax (subset):
 *   *      any chars except '/'
 *   ?      single char except '/'
 *   **     any chars including '/'
 *   /pat   anchored to the gitignore's directory
 *   pat/   directory-only
 *   !pat   negation
 *
 * Limitations: no character classes [abc], no escapes \\, no rooted
 * negation interaction edge cases. Covers ~95% of realistic patterns.
 */
#ifndef AMBIL_IGNORE_H
#define AMBIL_IGNORE_H

#include "ambil.h"

#include <stddef.h>

typedef struct lf_ignore_s lf_ignore_t;

lf_ignore_t *lf_ignore_new(const grep_opts_t *o);
void         lf_ignore_free(lf_ignore_t *ig);

/* Push gitignore/ignore/rgignore files found in directory `dir_abs` whose
 * paths displayed relative to walk root start with `rel_prefix` (may be
 * empty for the root). Returns the depth marker to pass to pop_dir. */
int lf_ignore_push_dir(lf_ignore_t *ig, const char *dir_abs, const char *rel_prefix);
void lf_ignore_pop_dir(lf_ignore_t *ig, int marker);

/* Decide whether to skip. `rel` is the path relative to the walk root,
 * using forward slashes. `basename` is the trailing component (for hidden
 * checks). Returns 1 if the path should be ignored. */
int lf_ignore_match_file(const lf_ignore_t *ig, const char *rel, const char *basename);
int lf_ignore_match_dir (const lf_ignore_t *ig, const char *rel, const char *basename);

/* Used to skip binary file extensions cheaply. */
int lf_ignore_is_binary_ext(const char *path);

/* Type-alias helpers exposed for tests. */
int lf_ignore_typename_known(const char *name);

#endif /* AMBIL_IGNORE_H */
