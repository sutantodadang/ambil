#include "file_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "util.h"
#include "platform.h"

int lf_mmap_open(const char *path, lf_mmap_t *m) {
    memset(m, 0, sizeof(*m));
#ifdef _WIN32
    m->fd = -1;
#endif

    int fd;
#ifdef _WIN32
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (n <= 0) {
            fprintf(stderr, "ambil: cannot translate path '%s'\n", path);
            return -1;
        }
        wchar_t *wp = (wchar_t *)lf_xmalloc((size_t)n * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, n);
        fd = _wopen(wp, _O_RDONLY | _O_BINARY);
        free(wp);
    }
#else
    fd = open(path, O_RDONLY);
#endif
    if (fd < 0) {
        fprintf(stderr, "ambil: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return -1;
    }

    size_t size = (size_t)st.st_size;
    if (size == 0) {
        m->data = NULL;
        m->size = 0;
#ifdef _WIN32
        m->fd = fd;
#else
        m->fd = fd;
#endif
        return 0;
    }

#ifdef _WIN32
    HANDLE os_file = (HANDLE)_get_osfhandle(fd);
    HANDLE mapping = CreateFileMappingW(os_file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) { _close(fd); return -1; }
    const char *p = (const char *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(mapping); _close(fd); return -1; }
    m->data = p;
    m->size = size;
    m->fd = fd;
    m->mapping_handle = mapping;
#else
    void *p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    m->data = (const char *)p;
    m->size = size;
    m->fd = fd;
#endif
    lf_plat_prefetch_seq(m->data, m->size);
    return 0;
}

void lf_mmap_close(lf_mmap_t *m) {
    if (!m) return;
#ifdef _WIN32
    if (m->data) UnmapViewOfFile(m->data);
    if (m->mapping_handle) CloseHandle((HANDLE)m->mapping_handle);
    if (m->fd >= 0) _close(m->fd);
#else
    if (m->data) munmap((void *)m->data, m->size);
    if (m->fd >= 0) close(m->fd);
#endif
    memset(m, 0, sizeof(*m));
#ifdef _WIN32
    m->fd = -1;
#endif
}

int64_t lf_for_each_line(const char *data, size_t size,
                         lf_line_cb_t cb, void *userdata) {
    if (!data || size == 0) return 0;

    uint64_t lineno = 0;
    size_t i = 0;
    while (i < size) {
        const char *nl = (const char *)memchr(data + i, '\n', size - i);
        size_t end = nl ? (size_t)(nl - data) : size;
        size_t llen = end - i;
        if (llen > 0 && data[i + llen - 1] == '\r') llen--;
        const char *line = data + i;
        lineno++;

        int rc = cb(userdata, lineno, line, llen);
        if (rc != 0) return rc < 0 ? -1 : (int64_t)lineno;

        i = nl ? (size_t)(nl - data) + 1 : size;
    }
    return (int64_t)lineno;
}
