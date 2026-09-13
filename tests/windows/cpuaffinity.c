/* Exercise the production affinity backend on synthetic topology and Windows.
 * See REDISCONTRIBUTIONS.txt for licensing information. */
#define WIN32_LEAN_AND_MEAN
#include "Win32_Interop/Win32_CpuAffinity.h"
#include "Win32_Interop/Win32_RedisLog.h"
#include <tlhelp32.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int force_affinity_failure;
static int affinity_calls;
static int warning_count;
static int warning_level;
static char warning_text[1024];

static BOOL WINAPI testSetThreadGroupAffinity(HANDLE thread,
                                              const GROUP_AFFINITY *affinity,
                                              GROUP_AFFINITY *previous) {
    affinity_calls++;
    if (force_affinity_failure) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return SetThreadGroupAffinity(thread, affinity, previous);
}

static void testServerLog(int level, const char *format, ...) {
    va_list args;
    warning_count++;
    warning_level = level;
    va_start(args, format);
    vsnprintf(warning_text, sizeof(warning_text), format, args);
    va_end(args);
}

/* Compile the actual backend with just the setter and logger intercepted.
 * Successful requests still reach Windows. This makes the API failure branch
 * deterministic without adding fault-injection controls to redis-server. */
#define SetThreadGroupAffinity testSetThreadGroupAffinity
#define serverLog testServerLog
#include "../../src/Win32_Interop/Win32_CpuAffinity.c"
#undef serverLog
#undef SetThreadGroupAffinity

_Static_assert(sizeof(KAFFINITY) == 8, "the affinity tests require Windows x64");
static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void expectList(const char *list, const KAFFINITY *masks, size_t count,
                       WORD group, KAFFINITY mask) {
    GROUP_AFFINITY affinity;
    const char *error;
    int result = win32ParseCpuList(list, masks, count, &affinity, &error);
    if (result != 0 || error != NULL || affinity.Group != group ||
        affinity.Mask != mask) {
        fprintf(stderr, "FAIL: parse '%s': %s, group=%u mask=%llx\n",
                list ? list : "(null)", error ? error : "wrong affinity",
                affinity.Group, (unsigned long long)affinity.Mask);
        failures++;
    }
    check(affinity.Reserved[0] == 0 && affinity.Reserved[1] == 0 &&
          affinity.Reserved[2] == 0, "GROUP_AFFINITY reserved fields are zero");
}

static void testParser(void) {
    const KAFFINITY masks[] = {UINT64_MAX, UINT64_C(0x8000000100000002)};
    const KAFFINITY sparse[] = {5, 2};
    const char *invalid[] = {
        " ", " 0", "0 ", "0\n", "+0", "-1", "x", "0x1", "\xff",
        ",0", "0,", "0,,1", "0, 1", "0-", "2-1", "0:2", "0-1:",
        "0-1:0", "0-1:-1", "0-1:2:3", "0-1-2", "0;1", "0/1",
        "4294967296", "18446744073709551616", "0-4294967296",
        "0-1:4294967296", "0-4294967295", "4294967295", "0,65",
        "63-65", "64", "128", "0,128"
    };
    expectList(NULL, NULL, 0, 0, 0);
    expectList("", NULL, 0, 0, 0);
    expectList("0", masks, 2, 0, 1);
    expectList("0,2-6:2", masks, 2, 0, 0x55);
    expectList("0-7:2", masks, 2, 0, 0x55);
    expectList("0-0", masks, 2, 0, 1);
    expectList("00,0,2,2", masks, 2, 0, 5);
    expectList("0-63", masks, 2, 0, UINT64_MAX);
    expectList("32,63", masks, 2, 0, UINT64_C(0x8000000100000000));
    expectList("65,96,127", masks, 2, 1, masks[1]);
    expectList("127,65,96,65", masks, 2, 1, masks[1]);
    expectList("65-127:31", masks, 2, 1, masks[1]);
    expectList("0-3:2", sparse, 2, 0, 5);
    expectList("65", sparse, 2, 1, 2);
    expectList("0-63:4294967295", masks, 2, 0, 1);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        GROUP_AFFINITY affinity;
        const char *error;
        int result = win32ParseCpuList(invalid[i], masks, 2, &affinity, &error);
        if (result == 0 || error == NULL || affinity.Mask != 0) {
            fprintf(stderr, "FAIL: accepted invalid list '%s'\n", invalid[i]);
            failures++;
        }
    }
    GROUP_AFFINITY affinity;
    const char *error;
    check(win32ParseCpuList("1", sparse, 2, &affinity, &error) != 0,
          "inactive CPU within a populated group is rejected");
    check(win32ParseCpuList("0-2", sparse, 2, &affinity, &error) != 0 &&
          affinity.Mask == 0, "inactive CPU in a range leaves no partial mask");
}

static int currentCpus(unsigned int cpus[64]) {
    GROUP_AFFINITY affinity;
    if (!GetThreadGroupAffinity(GetCurrentThread(), &affinity)) return -1;
    int count = 0;
    for (unsigned int bit = 0; bit < 64; bit++) {
        if (affinity.Mask & ((KAFFINITY)1 << bit))
            cpus[count++] = 64U * affinity.Group + bit;
    }
    return count;
}

static DWORD WINAPI testNativeAffinity(void *unused) {
    unsigned int cpus[64];
    GROUP_AFFINITY original = {0}, actual = {0}, narrowed = {0};
    const char *error;
    DWORD windows_error;
    char list[32];
    (void)unused;
    if (!GetThreadGroupAffinity(GetCurrentThread(), &original)) {
        check(0, "read disposable thread's initial affinity");
        return 1;
    }
    int count = currentCpus(cpus);
    if (count <= 0) {
        check(0, "find an allowed CPU on the native host");
        return 1;
    }
    snprintf(list, sizeof(list), "%u", cpus[0]);
    check(win32ValidateCpuAffinity(list, &error, &windows_error) == 0 &&
          error == NULL && windows_error == ERROR_SUCCESS,
          "native topology validates an allowed CPU");
    win32SetCpuAffinity(NULL);
    win32SetCpuAffinity("");
    check(affinity_calls == 0 && warning_count == 0,
          "unset and empty lists do not call the setter or warn");

    win32SetCpuAffinity(list);
    check(GetThreadGroupAffinity(GetCurrentThread(), &actual) &&
          actual.Group == cpus[0] / 64 &&
          actual.Mask == ((KAFFINITY)1 << (cpus[0] % 64)),
          "native readback matches the requested group and CPU");
    check(warning_count == 0, "successful native affinity does not warn");
    narrowed = actual;
    int previous_calls = affinity_calls;
    win32SetCpuAffinity("");
    check(GetThreadGroupAffinity(GetCurrentThread(), &actual) &&
          actual.Group == narrowed.Group && actual.Mask == narrowed.Mask &&
          affinity_calls == previous_calls, "empty list preserves narrowed affinity");

    snprintf(list, sizeof(list), "%u", cpus[count - 1]);
    force_affinity_failure = 1;
    win32SetCpuAffinity(list);
    force_affinity_failure = 0;
    check(warning_count == 1 && warning_level == LL_WARNING &&
          strstr(warning_text, list) != NULL &&
          strstr(warning_text, "SetThreadGroupAffinity failed") != NULL &&
          strstr(warning_text, "Windows error 5") != NULL,
          "an API failure warns with the CPU list and Windows error");
    check(GetThreadGroupAffinity(GetCurrentThread(), &actual) &&
          actual.Group == narrowed.Group && actual.Mask == narrowed.Mask,
          "API failure preserves the previous affinity");
    check(SetThreadGroupAffinity(GetCurrentThread(), &original, NULL),
          "restore the disposable thread's original affinity");
    return 0;
}

/* Read-only helper for the Tcl persistence tests. Count threads whose native
 * group/mask matches a requested list, without changing the target process. */
static int countThreads(const char *pid_string, const char *list) {
    char *end;
    errno = 0;
    unsigned long pid = strtoul(pid_string, &end, 10);
    if (errno || *end || pid == 0 || pid_string[0] < '0' || pid_string[0] > '9')
        return 2;
    GROUP_AFFINITY expected;
    const char *error;
    DWORD windows_error;
    if (resolveCpuAffinity(list, &expected, &error, &windows_error) != 0 ||
        expected.Mask == 0) return 2;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 1;
    THREADENTRY32 entry = {0};
    entry.dwSize = sizeof(entry);
    int matches = 0;
    BOOL more = Thread32First(snapshot, &entry);
    while (more) {
        if (entry.th32OwnerProcessID == pid) {
            HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE,
                                       entry.th32ThreadID);
            if (thread != NULL) {
                GROUP_AFFINITY actual;
                if (GetThreadGroupAffinity(thread, &actual) &&
                    actual.Group == expected.Group && actual.Mask == expected.Mask)
                    matches++;
                CloseHandle(thread);
            }
        }
        more = Thread32Next(snapshot, &entry);
    }
    DWORD snapshot_error = GetLastError();
    CloseHandle(snapshot);
    if (snapshot_error != ERROR_NO_MORE_FILES) return 1;
    printf("%d\n", matches);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--cpus") == 0) {
        unsigned int cpus[64];
        int count = currentCpus(cpus);
        if (count <= 0) return 1;
        for (int i = 0; i < count; i++) printf("%s%u", i ? " " : "", cpus[i]);
        puts("");
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--count") == 0)
        return countThreads(argv[2], argv[3]);
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--cpus | --count PID CPULIST]\n", argv[0]);
        return 2;
    }
    testParser();
    HANDLE thread = CreateThread(NULL, 0, testNativeAffinity, NULL, 0, NULL);
    check(thread != NULL, "create disposable native test thread");
    if (thread != NULL) {
        check(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
              "join disposable native test thread");
        CloseHandle(thread);
    }
    if (failures) return 1;
    puts("ALL CPU AFFINITY TESTS PASSED (synthetic groups and native readback)");
    return 0;
}
