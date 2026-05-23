/*
 * platform_stat.h — cross-platform file metadata.
 *
 * Provides a consistent struct across POSIX and Win32 so callers
 * (walker, ls, find) can access size, modification time, and type
 * without platform #ifdef in every consumer.
 */
#ifndef AMBIL_PLATFORM_STAT_H
#define AMBIL_PLATFORM_STAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t size;
    int64_t  mtime;     /* Unix epoch seconds */
    uint32_t mode;      /* POSIX mode bits (synthesised on Win32) */
    int      is_dir;
    int      is_file;
    int      is_symlink;
} lf_stat_t;

/* Stat a path. follow_symlinks=1 uses stat(), 0 uses lstat().
 * Returns 0 on success, -1 on error. */
int lf_stat(const char *path, lf_stat_t *st, int follow_symlinks);

/* Stat an open fd (POSIX fstat; on Win32 falls back to path-based). */
int lf_fstat(int fd, lf_stat_t *st);

#ifdef _WIN32
/* Conversion helpers for Win32 callers that already have FindFirstFileW /
 * GetFileAttributesExW results and want to avoid an extra lf_stat syscall. */
int64_t  lf_filetime_to_epoch(const void *ft);
uint32_t lf_attrs_to_mode(uint32_t attrs);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMBIL_PLATFORM_STAT_H */
