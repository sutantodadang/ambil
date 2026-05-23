/*
 * walker.h — recursive directory walker.
 *
 * Cross-platform: POSIX uses opendir/readdir/lstat; Win32 uses
 * FindFirstFileW/FindNextFileW with wide-char paths to safely handle
 * non-ASCII (paths returned to the caller are UTF-8).
 *
 * Iteration is depth-first. The walker integrates with lf_ignore_t to skip
 * ignored files and (crucially) entire directories before descending — this
 * keeps node_modules and similar from inflating walk time.
 */
#ifndef AMBIL_WALKER_H
#define AMBIL_WALKER_H

#include "ignore.h"
#include "platform_stat.h"

#include <stddef.h>
#include <stdint.h>

typedef struct lf_walker_s lf_walker_t;

/* Result for one yielded entry. `path` is heap-owned by the walker; the
 * caller may use it until the next call to lf_walk_next or lf_walk_close. */
typedef struct {
    const char *path;     /* full path with original separators */
    const char *rel_path; /* relative to walk root (forward slashes) */
    lf_stat_t   st;
    int         is_root_arg; /* 1 if user passed this path explicitly */
} lf_walk_entry_t;

/*
 * Open a walker. `paths` are user-supplied roots (files or dirs). The walker
 * borrows the array and the strings; they must outlive the walker.
 *
 * `recursive`: 1 to descend into directories, 0 to only emit files at the
 * top level (and explicitly listed dirs are not entered).
 *
 * `max_depth`: -1 unlimited, else max levels below a root dir (0 = root only).
 *
 * `follow`: follow symlinks (default 0).
 * `yield_dirs`: when 1, directory entries are yielded alongside files (for ls/find).
 */
lf_walker_t *lf_walk_open(const char **paths, size_t n_paths,
                          lf_ignore_t *ig,
                          int recursive, int max_depth, int follow,
                          int yield_dirs);

/* Get the next file entry. Returns 1 with *out filled, or 0 on exhaustion. */
int lf_walk_next(lf_walker_t *w, lf_walk_entry_t *out);

/* Number of root paths or directory entries the walker could not access.
 * Drives the process exit code (≥ 1 → exit 2). */
unsigned lf_walk_error_count(const lf_walker_t *w);

void lf_walk_close(lf_walker_t *w);

#endif /* AMBIL_WALKER_H */
