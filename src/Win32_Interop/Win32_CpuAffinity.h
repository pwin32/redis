/* Windows thread affinity for Redis CPU-list settings. */
#ifndef REDIS_WIN32_CPU_AFFINITY_H
#define REDIS_WIN32_CPU_AFFINITY_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU numbers are 64 * group + index, even when a group has fewer than 64
 * active processors. An empty list produces a zero mask (no affinity change).
 * These functions return 0 on success, -1 on failure. Error strings are static. */
int win32ParseCpuList(const char *cpulist, const KAFFINITY *active_masks,
                     size_t group_count, GROUP_AFFINITY *affinity,
                     const char **error);
int win32ValidateCpuAffinity(const char *cpulist, const char **error,
                            DWORD *windows_error);
void win32SetCpuAffinity(const char *cpulist);

#ifdef __cplusplus
}
#endif
#endif
