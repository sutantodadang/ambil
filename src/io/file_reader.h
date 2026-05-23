/*
 * file_reader.h — cross-platform mmap file access and line iteration.
 *
 * Provides a portable mmap wrapper (Win32 CreateFileMapping / POSIX mmap)
 * and a callback-driven line iterator. All subcommands (grep, cat, wc,
 * head, tail) share this module for file I/O.
 *
 * OWNERSHIP: data pointers from lf_mmap_open() are valid until
 * lf_mmap_close(). Line pointers from lf_for_each_line() point into
 * the mmap'd region — do not use after close.
 */
#ifndef AMBIL_FILE_READER_H
#define AMBIL_FILE_READER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque platform-specific mmap state. Callers should not access fields
 * directly — use lf_mmap_data() / lf_mmap_size() if needed. */
typedef struct {
    const char *data;
    size_t      size;
#ifdef _WIN32
    void       *mapping_handle;  /* HANDLE */
    int         fd;
#else
    int         fd;
#endif
} lf_mmap_t;

/* Open and memory-map a file for reading. Handles UTF-8 to UTF-16
 * conversion on Windows. Returns 0 on success, -1 on error (prints
 * error to stderr). Empty files succeed with data=NULL, size=0. */
int  lf_mmap_open(const char *path, lf_mmap_t *m);

/* Unmap and close. Safe to call on zero-initialised structs (no-op). */
void lf_mmap_close(lf_mmap_t *m);

/* Accessors for mmap'd region. */
static inline const char *lf_mmap_data(const lf_mmap_t *m) { return m->data; }
static inline size_t      lf_mmap_size(const lf_mmap_t *m) { return m->size; }

/* ---------- line iteration ---------- */

/* Callback for each line. lineno is 1-indexed. len is the line length
 * after stripping a trailing '\r' (for CRLF files). The line pointer
 * is valid for the duration of the callback only.
 *
 * Return non-zero to stop iteration early (e.g. --files-with-matches
 * found one match; --lines N:M range exceeded). */
typedef int (*lf_line_cb_t)(void *userdata, uint64_t lineno,
                             const char *line, size_t len);

/* Iterate over every line in [data, data+size). For each line, call cb.
 *
 * Lines are terminated by '\n'. A trailing '\r' before '\n' is stripped
 * from the reported length (the raw data still contains it). If the file
 * does not end with '\n', the final chunk is reported as the last line.
 *
 * Returns the total number of lines processed, or -1 if cb returned a
 * negative value (early stop). */
int64_t lf_for_each_line(const char *data, size_t size,
                         lf_line_cb_t cb, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* AMBIL_FILE_READER_H */
