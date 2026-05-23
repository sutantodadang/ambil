#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
/* Force Win8+ APIs (PrefetchVirtualMemory, GetActiveProcessorCount). We still
 * resolve PrefetchVirtualMemory dynamically so the binary loads on older OS. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#elif _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sched.h>
#endif
#endif

/* ---------- CPU count ------------------------------------------------------ */

int lf_plat_detect_cpus(void) {
    int n = 1;

#if defined(_WIN32)
    n = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n <= 0) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        n = (int)info.dwNumberOfProcessors;
    }
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(cpu_set_t), &set) == 0) {
        int cnt = CPU_COUNT(&set);
        if (cnt > 0) n = cnt;
    } else {
        long sc = sysconf(_SC_NPROCESSORS_ONLN);
        if (sc > 0) n = (int)sc;
    }
#else
    long sc = sysconf(_SC_NPROCESSORS_ONLN);
    if (sc > 0) n = (int)sc;
#endif

    if (n < 1)    n = 1;
    if (n > 1024) n = 1024;
    return n;
}

/* ---------- prefetch sequential ------------------------------------------- */

void lf_plat_prefetch_seq(const void *p, size_t len) {
#ifdef _WIN32
    /* PrefetchVirtualMemory was added in Windows 8. Resolve dynamically so we
     * still load on older Windows. Define the struct + signature locally so we
     * don't depend on MinGW header age. */
    typedef struct {
        PVOID  VirtualAddress;
        SIZE_T NumberOfBytes;
    } LF_MEM_RANGE_ENTRY;
    typedef BOOL (WINAPI *PrefetchVMFn)(HANDLE, ULONG_PTR, LF_MEM_RANGE_ENTRY *, ULONG);

    static int           probed = 0;
    static PrefetchVMFn  fn_pvm = NULL;
    if (!probed) {
        HMODULE h = GetModuleHandleA("kernel32.dll");
        if (h) {
            /* Win32 ABI guarantees this cast is safe; ISO C just dislikes it. */
            FARPROC raw = GetProcAddress(h, "PrefetchVirtualMemory");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
            fn_pvm = (PrefetchVMFn)raw;
#pragma GCC diagnostic pop
        }
        probed = 1;
    }
    if (fn_pvm) {
        LF_MEM_RANGE_ENTRY entry;
        entry.VirtualAddress = (PVOID)p;
        entry.NumberOfBytes  = (SIZE_T)len;
        fn_pvm(GetCurrentProcess(), 1, &entry, 0);
    }
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    /* Round p down to page boundary, expand len to cover. */
    long pgsz = sysconf(_SC_PAGE_SIZE);
    if (pgsz <= 0) pgsz = 4096;
    size_t align   = (size_t)((uintptr_t)p & (size_t)((uintptr_t)pgsz - 1));
    const void *ap = (const void *)((const char *)p - align);
    size_t alen    = len + align;
#if defined(MADV_SEQUENTIAL)
    (void)madvise((void *)ap, alen, MADV_SEQUENTIAL);
#elif defined(POSIX_MADV_SEQUENTIAL)
    (void)posix_madvise((void *)ap, alen, POSIX_MADV_SEQUENTIAL);
#else
    (void)ap; (void)alen;
#endif
#else
    (void)p; (void)len;
#endif
}
