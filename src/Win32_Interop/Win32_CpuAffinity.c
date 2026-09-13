/*
 * Windows CPU-list parsing and strict, single-group thread affinity.
 * See REDISCONTRIBUTIONS.txt for licensing information.
 */
/* Legacy Visual Studio projects still set an XP SDK target. Request the
 * Windows 7 processor-group declarations in this translation unit. */
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "Win32_CpuAffinity.h"
#include "Win32_RedisLog.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

static int cpuListNumber(const char **cursor, uint32_t *number,
                         const char **error) {
    const char *p = *cursor;
    uint32_t value = 0;
    if (*p < '0' || *p > '9') {
        *error = "expected a decimal CPU number or range stride";
        return -1;
    }
    do {
        unsigned int digit = (unsigned int)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10) {
            *error = "CPU number or range stride overflows 32 bits";
            return -1;
        }
        value = value * 10 + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *number = value;
    return 0;
}

int win32ParseCpuList(const char *cpulist, const KAFFINITY *active_masks,
                     size_t group_count, GROUP_AFFINITY *affinity,
                     const char **error) {
    const char *p = cpulist;
    GROUP_AFFINITY result = {0};
    memset(affinity, 0, sizeof(*affinity));
    *error = NULL;
    if (p == NULL || *p == '\0') return 0;

    for (;;) {
        uint32_t first, last, stride = 1;
        if (cpuListNumber(&p, &first, error) != 0) return -1;
        last = first;
        if (*p == '-') {
            p++;
            if (cpuListNumber(&p, &last, error) != 0) return -1;
            if (last < first) {
                *error = "CPU ranges must be ascending";
                return -1;
            }
            if (*p == ':') {
                p++;
                if (cpuListNumber(&p, &stride, error) != 0) return -1;
                if (stride == 0) {
                    *error = "CPU range stride must be greater than zero";
                    return -1;
                }
            }
        }
        if (*p != '\0' && *p != ',') {
            *error = "expected a comma or the end of the CPU list";
            return -1;
        }

        /* Bound the iteration to one group before walking the selected CPUs.
         * A stepped range need not select its upper endpoint. */
        uint32_t selected_last = first + ((last - first) / stride) * stride;
        uint32_t group = first / 64;
        if (group != selected_last / 64 ||
            (result.Mask != 0 && result.Group != group)) {
            *error = "a CPU list must stay within one processor group";
            return -1;
        }
        if (group >= group_count || group > USHRT_MAX) {
            *error = "selected CPU is not active";
            return -1;
        }
        result.Group = (WORD)group;
        for (uint32_t cpu = first;; cpu += stride) {
            unsigned int index = cpu % 64;
            if (index >= sizeof(KAFFINITY) * CHAR_BIT) {
                *error = "selected CPU cannot be represented by this Windows build";
                return -1;
            }
            KAFFINITY bit = (KAFFINITY)1 << index;
            if ((active_masks[group] & bit) == 0) {
                *error = "selected CPU is not active";
                return -1;
            }
            result.Mask |= bit;
            if (selected_last - cpu < stride) break;
        }
        if (*p == '\0') break;
        p++; /* The next iteration rejects empty and trailing elements. */
    }
    *affinity = result;
    return 0;
}

static int resolveCpuAffinity(const char *cpulist, GROUP_AFFINITY *affinity,
                              const char **error, DWORD *windows_error) {
    DWORD bytes = 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *topology = NULL;
    KAFFINITY *active_masks = NULL;
    int result = -1;
    memset(affinity, 0, sizeof(*affinity));
    *error = NULL;
    *windows_error = ERROR_SUCCESS;
    if (cpulist == NULL || *cpulist == '\0') return 0;

    /* Query each process independently. Nothing here is retained in QFork's
     * heap snapshot, and the default path does not query or change affinity. */
    if (GetLogicalProcessorInformationEx(RelationGroup, NULL, &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        *windows_error = GetLastError();
        *error = "cannot query active processor groups";
        return -1;
    }
    topology = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (topology == NULL) {
        *windows_error = ERROR_NOT_ENOUGH_MEMORY;
        *error = "cannot allocate processor topology";
        return -1;
    }
    if (!GetLogicalProcessorInformationEx(RelationGroup, topology, &bytes)) {
        *windows_error = GetLastError();
        *error = "cannot query active processor groups";
        goto cleanup;
    }

    size_t header = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group) +
                    offsetof(GROUP_RELATIONSHIP, GroupInfo);
    if (bytes < header || topology->Size < header || topology->Size > bytes ||
        topology->Relationship != RelationGroup ||
        topology->Group.ActiveGroupCount == 0 ||
        topology->Group.ActiveGroupCount > topology->Group.MaximumGroupCount ||
        topology->Group.ActiveGroupCount >
            (topology->Size - header) / sizeof(PROCESSOR_GROUP_INFO)) {
        *windows_error = ERROR_INVALID_DATA;
        *error = "invalid processor-group topology";
        goto cleanup;
    }
    WORD group_count = topology->Group.ActiveGroupCount;
    active_masks = HeapAlloc(GetProcessHeap(), 0,
                            group_count * sizeof(*active_masks));
    if (active_masks == NULL) {
        *windows_error = ERROR_NOT_ENOUGH_MEMORY;
        *error = "cannot allocate processor masks";
        goto cleanup;
    }
    for (WORD group = 0; group < group_count; group++)
        active_masks[group] = topology->Group.GroupInfo[group].ActiveProcessorMask;
    result = win32ParseCpuList(cpulist, active_masks, group_count, affinity, error);
    if (result != 0) *windows_error = ERROR_INVALID_PARAMETER;

cleanup:
    if (active_masks != NULL) HeapFree(GetProcessHeap(), 0, active_masks);
    HeapFree(GetProcessHeap(), 0, topology);
    return result;
}

int win32ValidateCpuAffinity(const char *cpulist, const char **error,
                            DWORD *windows_error) {
    GROUP_AFFINITY affinity;
    return resolveCpuAffinity(cpulist, &affinity, error, windows_error);
}

void win32SetCpuAffinity(const char *cpulist) {
    GROUP_AFFINITY affinity;
    const char *error;
    DWORD windows_error;
    if (resolveCpuAffinity(cpulist, &affinity, &error, &windows_error) == 0) {
        if (affinity.Mask == 0) return;
        if (SetThreadGroupAffinity(GetCurrentThread(), &affinity, NULL)) return;
        windows_error = GetLastError();
        error = "SetThreadGroupAffinity failed";
    }
    serverLog(LL_WARNING,
              "Cannot apply CPU affinity '%s': %s (Windows error %lu); "
              "retaining the current thread affinity.",
              cpulist, error, (unsigned long)windows_error);
}
