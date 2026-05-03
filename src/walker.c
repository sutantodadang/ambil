/*
 * walker.c — POSIX + Win32 directory walker.
 *
 * The walker maintains a stack of pending entries (DFS). When a directory
 * is popped, its children are pushed in reverse-sorted order so iteration
 * proceeds in lexicographic order per directory. Each push records the
 * directory's depth and its ignore-stack marker, so when we finish a
 * directory we can pop the per-directory gitignore rules cleanly.
 */
#include "walker.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

/* ---------- pending stack ---------- */

typedef enum {
    PE_FILE = 0,
    PE_DIR_ENTER,    /* push gitignore rules, scan children */
    PE_DIR_LEAVE     /* pop gitignore rules */
} pe_kind_t;

typedef struct {
    pe_kind_t   kind;
    char       *abs;        /* full path (heap) */
    char       *rel;        /* relative to nearest root (heap; forward slashes) */
    uint64_t    size;
    int         depth;
    int         ignore_marker;
    int         is_root_arg;
} pending_t;

/* Visited-directory record for --follow cycle detection.
 *
 * On POSIX we key on (dev, ino); on Win32 on (volume_serial, file_index).
 * Linear search is fine because real filesystems rarely have deep symlink
 * webs, and typical scans see < a few hundred dirs even with --follow. */
typedef struct {
#ifdef _WIN32
    unsigned long long vol;
    unsigned long long idx;
#else
    unsigned long long dev;
    unsigned long long ino;
#endif
} visited_id_t;

struct lf_walker_s {
    lf_ignore_t *ig;
    int          recursive;
    int          max_depth;
    int          follow;

    pending_t   *stack;
    size_t       n, cap;

    /* Visited-set for --follow loop avoidance. */
    visited_id_t *visited;
    size_t        n_visited, cap_visited;

    unsigned      errors;

    /* Current entry's owned strings (returned to caller). */
    char *cur_path;
    char *cur_rel;
};

static int visited_id_for(const char *abs_path, visited_id_t *id) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, abs_path, -1, NULL, 0);
    if (n <= 0) return -1;
    wchar_t *wp = (wchar_t *)lf_xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, abs_path, -1, wp, n);
    HANDLE h = CreateFileW(wp, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return -1;
    BY_HANDLE_FILE_INFORMATION info;
    int ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) return -1;
    id->vol = (unsigned long long)info.dwVolumeSerialNumber;
    id->idx = ((unsigned long long)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    return 0;
#else
    struct stat st;
    if (stat(abs_path, &st) != 0) return -1;
    id->dev = (unsigned long long)st.st_dev;
    id->ino = (unsigned long long)st.st_ino;
    return 0;
#endif
}

static int visited_seen(const lf_walker_t *w, const visited_id_t *id) {
    for (size_t i = 0; i < w->n_visited; i++) {
#ifdef _WIN32
        if (w->visited[i].vol == id->vol && w->visited[i].idx == id->idx) return 1;
#else
        if (w->visited[i].dev == id->dev && w->visited[i].ino == id->ino) return 1;
#endif
    }
    return 0;
}

static void visited_add(lf_walker_t *w, visited_id_t id) {
    if (w->n_visited == w->cap_visited) {
        w->cap_visited = w->cap_visited ? w->cap_visited * 2 : 32;
        w->visited = (visited_id_t *)lf_xrealloc(w->visited,
                                                 w->cap_visited * sizeof(*w->visited));
    }
    w->visited[w->n_visited++] = id;
}

static void push(lf_walker_t *w, pending_t pe) {
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->stack = (pending_t *)lf_xrealloc(w->stack, w->cap * sizeof(pending_t));
    }
    w->stack[w->n++] = pe;
}

/* ---------- path helpers ---------- */

static char *join_path(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0) return lf_xstrdup(b);
    int need_sep = !lf_is_sep(a[la - 1]);
    char *out = (char *)lf_xmalloc(la + lb + 2);
    memcpy(out, a, la);
    size_t i = la;
    if (need_sep) out[i++] = '/';
    memcpy(out + i, b, lb);
    out[i + lb] = '\0';
    return out;
}

static char *join_rel(const char *a, const char *b) {
    if (!a || !*a) return lf_xstrdup(b);
    size_t la = strlen(a), lb = strlen(b);
    char *out = (char *)lf_xmalloc(la + lb + 2);
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '\0';
    return out;
}

#ifdef _WIN32
/* UTF-16 <-> UTF-8 helpers. Returned buffers are heap-owned. */
static wchar_t *utf8_to_utf16(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)lf_xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *utf16_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)lf_xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
#endif

/* qsort string comparator (stable lexicographic). */
static int cmp_str_desc(const void *a, const void *b) {
    /* For DFS lexicographic ordering we push reversed → output is lex-asc. */
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

/* List children of `dir_abs`. Outputs heap arrays of basenames + types.
 * is_dir[i] = 1 directory, 0 regular file, -1 skipped (other). size[i] =
 * file size (0 for dirs). Returns 0 on success. */
static int list_dir(const char *dir_abs, int follow,
                    char ***names_out, int **isdir_out, uint64_t **size_out, size_t *n_out) {
    *names_out = NULL; *isdir_out = NULL; *size_out = NULL; *n_out = 0;
    char **names = NULL; int *isdir = NULL; uint64_t *sizes = NULL;
    size_t n = 0, cap = 0;

#ifdef _WIN32
    char *pat = (char *)lf_xmalloc(strlen(dir_abs) + 4);
    sprintf(pat, "%s\\*", dir_abs);
    wchar_t *wpat = utf8_to_utf16(pat);
    free(pat);
    if (!wpat) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
        char *name = utf16_to_utf8(fd.cFileName);
        if (!name) continue;
        int dirflag;
        uint64_t sz = 0;
        DWORD attrs = fd.dwFileAttributes;
        if (!follow && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) { free(name); continue; }
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            dirflag = 1;
        } else {
            dirflag = 0;
            sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = (char **)lf_xrealloc(names, cap * sizeof(*names));
            isdir = (int *)lf_xrealloc(isdir, cap * sizeof(*isdir));
            sizes = (uint64_t *)lf_xrealloc(sizes, cap * sizeof(*sizes));
        }
        names[n] = name; isdir[n] = dirflag; sizes[n] = sz; n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir_abs);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
        char *full = join_path(dir_abs, de->d_name);
        struct stat st;
        int rc = follow ? stat(full, &st) : lstat(full, &st);
        if (rc != 0) { free(full); continue; }
        int dirflag;
        uint64_t sz = 0;
        if (S_ISLNK(st.st_mode) && !follow) { free(full); continue; }
        if (S_ISDIR(st.st_mode))      dirflag = 1;
        else if (S_ISREG(st.st_mode)) { dirflag = 0; sz = (uint64_t)st.st_size; }
        else                          { free(full); continue; }
        free(full);
        char *name = lf_xstrdup(de->d_name);
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = (char **)lf_xrealloc(names, cap * sizeof(*names));
            isdir = (int *)lf_xrealloc(isdir, cap * sizeof(*isdir));
            sizes = (uint64_t *)lf_xrealloc(sizes, cap * sizeof(*sizes));
        }
        names[n] = name; isdir[n] = dirflag; sizes[n] = sz; n++;
    }
    closedir(d);
#endif

    /* Sort descending so DFS pops in ascending order. */
    /* Simple structure-of-arrays sort via index array. */
    if (n > 1) {
        size_t *idx = (size_t *)lf_xmalloc(n * sizeof(size_t));
        for (size_t i = 0; i < n; i++) idx[i] = i;
        /* Manual insertion sort by name desc — n is typically small enough. */
        for (size_t i = 1; i < n; i++) {
            size_t k = idx[i];
            size_t j = i;
            while (j > 0 && strcmp(names[idx[j - 1]], names[k]) < 0) {
                idx[j] = idx[j - 1]; j--;
            }
            idx[j] = k;
        }
        char **n2 = (char **)lf_xmalloc(n * sizeof(*n2));
        int *d2 = (int *)lf_xmalloc(n * sizeof(*d2));
        uint64_t *s2 = (uint64_t *)lf_xmalloc(n * sizeof(*s2));
        for (size_t i = 0; i < n; i++) { n2[i] = names[idx[i]]; d2[i] = isdir[idx[i]]; s2[i] = sizes[idx[i]]; }
        free(names); free(isdir); free(sizes); free(idx);
        names = n2; isdir = d2; sizes = s2;
    }
    (void)cmp_str_desc;

    *names_out = names; *isdir_out = isdir; *size_out = sizes; *n_out = n;
    return 0;
}

/* ---------- public API ---------- */

lf_walker_t *lf_walk_open(const char **paths, size_t n_paths,
                          lf_ignore_t *ig,
                          int recursive, int max_depth, int follow) {
    lf_walker_t *w = (lf_walker_t *)lf_xcalloc(1, sizeof(*w));
    w->ig         = ig;
    w->recursive  = recursive;
    w->max_depth  = max_depth;
    w->follow     = follow;

    /* Push roots in reverse so first arg processed first. */
    for (size_t i = n_paths; i > 0; i--) {
        const char *p = paths[i - 1];
#ifdef _WIN32
        wchar_t *wp = utf8_to_utf16(p);
        DWORD attrs = wp ? GetFileAttributesW(wp) : INVALID_FILE_ATTRIBUTES;
        free(wp);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "ambil: cannot access '%s'\n", p);
            w->errors++;
            continue;
        }
        int is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        uint64_t sz = 0;
        if (!is_dir) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            wp = utf8_to_utf16(p);
            if (wp && GetFileAttributesExW(wp, GetFileExInfoStandard, &fa)) {
                sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            }
            free(wp);
        }
#else
        struct stat st;
        int rc = follow ? stat(p, &st) : lstat(p, &st);
        if (rc != 0) {
            fprintf(stderr, "ambil: cannot access '%s': %s\n", p, strerror(errno));
            w->errors++;
            continue;
        }
        int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        uint64_t sz = is_dir ? 0 : (uint64_t)st.st_size;
#endif
        pending_t pe;
        memset(&pe, 0, sizeof(pe));
        pe.abs = lf_xstrdup(p);
        pe.rel = lf_xstrdup(is_dir ? "" : p);
        pe.size = sz;
        pe.depth = 0;
        pe.is_root_arg = 1;
        if (is_dir && recursive) {
            pe.kind = PE_DIR_ENTER;
        } else if (is_dir) {
            /* Non-recursive on a directory: skip (matches grep -d skip). */
            free(pe.abs); free(pe.rel);
            fprintf(stderr, "ambil: '%s' is a directory; pass --recursive (default) or a file\n", p);
            w->errors++;
            continue;
        } else {
            pe.kind = PE_FILE;
        }
        push(w, pe);
    }
    return w;
}

void lf_walk_close(lf_walker_t *w) {
    if (!w) return;
    for (size_t i = 0; i < w->n; i++) {
        free(w->stack[i].abs);
        free(w->stack[i].rel);
    }
    free(w->stack);
    free(w->visited);
    free(w->cur_path);
    free(w->cur_rel);
    free(w);
}

unsigned lf_walk_error_count(const lf_walker_t *w) {
    return w ? w->errors : 0;
}

int lf_walk_next(lf_walker_t *w, lf_walk_entry_t *out) {
    free(w->cur_path); w->cur_path = NULL;
    free(w->cur_rel);  w->cur_rel  = NULL;

    while (w->n > 0) {
        pending_t pe = w->stack[--w->n];
        if (pe.kind == PE_DIR_LEAVE) {
            lf_ignore_pop_dir(w->ig, pe.ignore_marker);
            free(pe.abs); free(pe.rel);
            continue;
        }
        if (pe.kind == PE_FILE) {
            const char *bn = lf_path_basename(pe.abs);
            if (!pe.is_root_arg && lf_ignore_match_file(w->ig, pe.rel, bn)) {
                free(pe.abs); free(pe.rel);
                continue;
            }
            w->cur_path = pe.abs;
            w->cur_rel  = pe.rel;
            out->path     = w->cur_path;
            out->rel_path = w->cur_rel;
            out->size     = pe.size;
            out->is_root_arg = pe.is_root_arg;
            return 1;
        }
        /* PE_DIR_ENTER: load gitignore, push leave marker, push children. */
        const char *bn = lf_path_basename(pe.abs);
        const char *rel_for_match = (pe.rel && pe.rel[0]) ? pe.rel : bn;
        if (!pe.is_root_arg && lf_ignore_match_dir(w->ig, rel_for_match, bn)) {
            free(pe.abs); free(pe.rel);
            continue;
        }
        if (w->max_depth >= 0 && pe.depth > w->max_depth) {
            free(pe.abs); free(pe.rel);
            continue;
        }
        /* --follow loop guard: skip directories we've already entered. */
        if (w->follow) {
            visited_id_t vid;
            if (visited_id_for(pe.abs, &vid) == 0) {
                if (visited_seen(w, &vid)) {
                    fprintf(stderr, "ambil: skipping symlink loop at '%s'\n", pe.abs);
                    free(pe.abs); free(pe.rel);
                    continue;
                }
                visited_add(w, vid);
            }
        }

        int marker = lf_ignore_push_dir(w->ig, pe.abs, pe.rel ? pe.rel : "");

        /* Push DIR_LEAVE first (so it runs after children). */
        pending_t leave = {0};
        leave.kind = PE_DIR_LEAVE;
        leave.abs  = lf_xstrdup("");
        leave.rel  = lf_xstrdup("");
        leave.ignore_marker = marker;
        push(w, leave);

        char **names; int *isdir; uint64_t *sizes; size_t n;
        int lrc = list_dir(pe.abs, w->follow, &names, &isdir, &sizes, &n);
        if (lrc != 0) {
            fprintf(stderr, "ambil: cannot read directory '%s': %s\n",
                    pe.abs, strerror(errno));
            w->errors++;
        }
        if (lrc == 0) {
            for (size_t i = 0; i < n; i++) {
                pending_t c = {0};
                c.abs = join_path(pe.abs, names[i]);
                c.rel = join_rel(pe.rel, names[i]);
                c.size = sizes[i];
                c.depth = pe.depth + 1;
                c.is_root_arg = 0;
                if (isdir[i]) {
                    if (w->recursive) c.kind = PE_DIR_ENTER;
                    else { free(c.abs); free(c.rel); free(names[i]); continue; }
                } else {
                    c.kind = PE_FILE;
                }
                push(w, c);
                free(names[i]);
            }
            free(names); free(isdir); free(sizes);
        }
        free(pe.abs); free(pe.rel);
    }
    return 0;
}
