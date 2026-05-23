#ifndef AMBIL_PLATFORM_H
#define AMBIL_PLATFORM_H
#include <stddef.h>

/* Logical CPU count, container/cgroup aware on Linux. Clamped to [1, 1024]. */
int  lf_plat_detect_cpus(void);

/* Hint the OS to prefetch [p, p+len) as sequential. POSIX -> madvise(SEQUENTIAL).
 * Windows 8+ -> PrefetchVirtualMemory (resolved via GetProcAddress; no-op on older). */
void lf_plat_prefetch_seq(const void *p, size_t len);

#endif
