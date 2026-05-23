#include "platform_stat.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "util.h"

/* FILETIME is 100ns intervals since 1601-01-01.
 * Unix epoch is 1970-01-01. Offset = 11644473600 seconds. */
#ifdef _WIN32
int64_t lf_filetime_to_epoch(const void *ft_ptr) {
    const FILETIME *ft = (const FILETIME *)ft_ptr;
    ULARGE_INTEGER uli;
    uli.LowPart = ft->dwLowDateTime;
    uli.HighPart = ft->dwHighDateTime;
    return (int64_t)(uli.QuadPart / 10000000ULL) - 11644473600LL;
}

uint32_t lf_attrs_to_mode(uint32_t attrs) {
    uint32_t m = 0;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        m = (uint32_t)(_S_IFDIR | 0555);
    else
        m = (uint32_t)(_S_IFREG | 0444);
    if (!(attrs & FILE_ATTRIBUTE_READONLY))
        m |= 0222;
    return m;
}
#endif

int lf_stat(const char *path, lf_stat_t *st, int follow_symlinks) {
    (void)follow_symlinks;
    memset(st, 0, sizeof(*st));

#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) return -1;
    wchar_t *wp = (wchar_t *)lf_xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, n);

    WIN32_FILE_ATTRIBUTE_DATA fa;
    int ok = GetFileAttributesExW(wp, GetFileExInfoStandard, &fa);
    free(wp);
    if (!ok) return -1;

    DWORD attrs = fa.dwFileAttributes;
    st->is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    st->is_file = !st->is_dir;
    st->is_symlink = 0;
    st->size = st->is_dir ? 0 :
               ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    st->mtime = lf_filetime_to_epoch(&fa.ftLastWriteTime);
    st->mode = lf_attrs_to_mode(attrs);
    return 0;
#else
    struct stat sb;
    int rc = follow_symlinks ? stat(path, &sb) : lstat(path, &sb);
    if (rc != 0) return -1;

    st->size = (uint64_t)sb.st_size;
    st->mtime = (int64_t)sb.st_mtime;
    st->mode = (uint32_t)sb.st_mode;
    st->is_dir = S_ISDIR(sb.st_mode) ? 1 : 0;
    st->is_file = S_ISREG(sb.st_mode) ? 1 : 0;
    st->is_symlink = S_ISLNK(sb.st_mode) ? 1 : 0;
    return 0;
#endif
}

int lf_fstat(int fd, lf_stat_t *st) {
    memset(st, 0, sizeof(*st));

#ifdef _WIN32
    /* Fallback: we don't have the path, so just get size + type.
     * mtime will be 0. For ambil's use case (already-opened files in
     * walker) this path isn't typically hit — lf_stat is preferred. */
    struct _stat64 sb;
    if (_fstat64(fd, &sb) != 0) return -1;
    st->size = (uint64_t)sb.st_size;
    st->mode = (uint32_t)sb.st_mode;
    st->is_dir = (sb.st_mode & _S_IFDIR) ? 1 : 0;
    st->is_file = (sb.st_mode & _S_IFREG) ? 1 : 0;
    st->is_symlink = 0;
    return 0;
#else
    struct stat sb;
    if (fstat(fd, &sb) != 0) return -1;

    st->size = (uint64_t)sb.st_size;
    st->mtime = (int64_t)sb.st_mtime;
    st->mode = (uint32_t)sb.st_mode;
    st->is_dir = S_ISDIR(sb.st_mode) ? 1 : 0;
    st->is_file = S_ISREG(sb.st_mode) ? 1 : 0;
    st->is_symlink = S_ISLNK(sb.st_mode) ? 1 : 0;
    return 0;
#endif
}
