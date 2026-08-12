#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define FDAPI_NOCRTREDEFS
#include "Win32_Interop/Win32_FDAPI.h"
#include "Win32_Interop/Win32_Common.h"
#include "Win32_Interop/Win32_Error.h"
#include <unistd.h>

extern "C" {
#include "Win32_Interop/Win32_Signal_Process.h"
#include "Win32_Interop/Win32_PThread.h"
FILE *replace_fopen(const char *path, const char *mode);
FILE *replace_freopen(const char *path, const char *mode, FILE *stream);
FILE *replace_popen(const char *command, const char *mode);
int replace_remove(const char *path);
int replace_system(const char *command);
int replace_unlink(const char *path);
int replace_mkdir(const char *path);
int replace_rmdir(const char *path);
int replace_stat64(const char *path, struct __stat64 *buffer);
int replace_link(const char *src, const char *dest);
int replace_rename(const char *src, const char *dest);
int replace_random(void);
int win32_secure_random_bytes(void *buffer, size_t length);
int win32_llp64_interop_test(void);
}

static bool emulate_modern_windows = false;
static int failures = 0;
static bool checked_windows_6_0 = false;
static bool checked_windows_6_2 = false;

namespace Globals {
size_t pageSize = 0;
}

void EnsureMemoryIsMapped(const void *, size_t) {
}

bool IsWindowsVersionAtLeast(WORD major, WORD minor, WORD service_pack) {
    if (major == 6 && minor == 0 && service_pack == 0) {
        checked_windows_6_0 = true;
    }
    if (major == 6 && minor == 2 && service_pack == 0) {
        checked_windows_6_2 = true;
    }
    return emulate_modern_windows;
}

extern "C" void serverLog(int, const char *, ...) {
}

extern "C" BOOL ParseAndPrintANSIString(HANDLE, LPCVOID, DWORD, LPDWORD) {
    return FALSE;
}

static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

struct secure_random_context {
    unsigned char bytes[64];
    int result;
};

static void *fill_secure_random(void *argument) {
    secure_random_context *context =
        static_cast<secure_random_context *>(argument);
    context->result = win32_secure_random_bytes(context->bytes,
                                                sizeof(context->bytes));
    return NULL;
}

static void test_secure_random() {
    secure_random_context contexts[4] = {};
    pthread_t threads[4] = {};

    check(win32_secure_random_bytes(NULL, 0) == 0,
          "zero-length secure random requests should succeed");
    for (size_t index = 0; index < 4; index++) {
        check(pthread_create(&threads[index], NULL, fill_secure_random,
                             &contexts[index]) == 0,
              "concurrent secure random thread should start");
    }
    for (size_t index = 0; index < 4; index++) {
        if (threads[index] != 0)
            check(pthread_join(threads[index], NULL) == 0,
                  "concurrent secure random thread should join");
        bool all_zero = true;
        for (size_t byte = 0; byte < sizeof(contexts[index].bytes); byte++) {
            if (contexts[index].bytes[byte] != 0) all_zero = false;
        }
        check(contexts[index].result == 0 && !all_zero,
              "secure random output should succeed and contain entropy");
        if (index != 0) {
            check(std::memcmp(contexts[0].bytes, contexts[index].bytes,
                              sizeof(contexts[0].bytes)) != 0,
                  "independent secure random outputs should differ");
        }
    }

    for (int index = 0; index < 32; index++) {
        int value = replace_random();
        check(value >= 0 && value <= INT_MAX,
              "replace_random should return a nonnegative 31-bit value");
    }
}

static void test_dns_ascii_policy() {
    struct addrinfo hints = {};
    struct addrinfo *result = reinterpret_cast<struct addrinfo *>(1);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    check(getaddrinfo("host-\xc3\xa9", "6379", &hints, &result) ==
              EAI_NONAME && result == NULL,
          "non-ASCII DNS nodes should be rejected before Winsock");
    result = reinterpret_cast<struct addrinfo *>(1);
    check(getaddrinfo("localhost", "port-\xc3\xa9", &hints, &result) ==
              EAI_SERVICE && result == NULL,
          "non-ASCII DNS services should be rejected before Winsock");
}

static void test_error_translation() {
    sigset_t signals;
    sigset_t old_signals = 0;

    check(win32_errno_from_system_error(WSAEWOULDBLOCK) == EAGAIN,
          "WSAEWOULDBLOCK should map to POSIX EAGAIN");
    check(win32_errno_from_system_error(WSAEINPROGRESS) == EINPROGRESS,
          "WSAEINPROGRESS should map to POSIX EINPROGRESS");
    check(win32_errno_from_system_error(WSAECONNRESET) == ECONNRESET,
          "WSAECONNRESET should map to POSIX ECONNRESET");
    check(win32_errno_from_system_error(WSAEINTR) == EINTR,
          "WSAEINTR should map to POSIX EINTR");
    check(win32_errno_from_system_error(ERROR_OPERATION_ABORTED) == ECANCELED,
          "an aborted Windows operation should map to POSIX ECANCELED");
    check(win32_errno_from_system_error(0x7fffffff) == EIO,
          "unknown Windows errors should map to POSIX EIO");
    check(std::strcmp(wsa_strerror(EACCES), std::strerror(EACCES)) == 0,
          "POSIX errno formatting must not reinterpret errno as a Win32 code");
    const char *native_error = win32_system_strerror(ERROR_ACCESS_DENIED);
    check(native_error != NULL && native_error[0] != '\0',
          "native Windows errors should retain a UTF-8 system formatter");
    check(sizeof(((redis_rusage_timeval *)0)->tv_sec) == sizeof(int64_t),
          "Windows CPU accounting seconds should be 64-bit");
    check(sizeof(((redis_rusage_timeval *)0)->tv_usec) == sizeof(int64_t),
          "Windows CPU accounting microseconds should be 64-bit");

    sigemptyset(&signals);
    check(sigaddset(&signals, SIGUSR2) != 0 &&
              sigismember(&signals, SIGUSR2) != 0,
          "Windows signal sets should represent SIGUSR2 safely");
    sigdelset(&signals, SIGUSR2);
    check(sigismember(&signals, SIGUSR2) == 0,
          "Windows signal sets should remove SIGUSR2 safely");

    errno = EBUSY;
    check(pthread_sigmask(SIG_BLOCK, &signals, &old_signals) == 0 &&
              errno == EBUSY,
          "the Windows pthread signal-mask no-op should preserve errno");
    check(pthread_sigmask(999, &signals, &old_signals) == EINVAL,
          "pthread_sigmask should return POSIX error numbers directly");
}

static void test_utf8_filesystem() {
    const char unicode_component[] =
        "redis-interop-\xe8\xb7\xaf\xe5\xbe\x84-\xf0\x9f\x98\x80";
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "-%lu-%lu",
                  (unsigned long)GetCurrentProcessId(),
                  (unsigned long)GetTickCount());
    std::string root = std::string(unicode_component) + suffix;
    std::string filename = root + "/\xe6\x95\xb0\xe6\x8d\xae.txt";
    std::string hardlink = root + "/\xe9\x93\xbe\xe6\x8e\xa5.txt";
    std::string renamed = root + "/\xe9\x87\x8d\xe5\x91\xbd\xe5\x90\x8d.txt";
    std::string nested_dir = root + "/group-a";
    std::string nested_file = nested_dir +
        "/\xe9\x85\x8d\xe7\xbd\xae-x.conf";
    std::vector<std::string> directories;
    FILE *file = NULL;
    char *full_filename = NULL;
    char *full_nested_file = NULL;
    char *full_root = NULL;
    char *saved_cwd = NULL;
    char *changed_cwd = NULL;
    char **matches = NULL;
    size_t match_count = 0;
    bool found_name = false;
    bool found_glob = false;
    struct __stat64 statbuf = {};

    check(replace_mkdir(root.c_str()) == 0,
          "UTF-8 directory creation should succeed");
    if (replace_stat64(root.c_str(), &statbuf) != 0) goto cleanup;
    directories.push_back(root);

    file = replace_fopen(filename.c_str(), "wb");
    check(file != NULL, "UTF-8 file creation should succeed");
    if (file != NULL) {
        const char payload[] = "unicode-path-payload";
        check(std::fwrite(payload, 1, sizeof(payload) - 1, file) ==
                  sizeof(payload) - 1,
              "UTF-8 file should accept data");
        check(std::fclose(file) == 0, "UTF-8 file should close cleanly");
        file = NULL;
    }
    check(replace_stat64(filename.c_str(), &statbuf) == 0 &&
              statbuf.st_size == 20,
          "UTF-8 file should be visible through wide stat");

    file = replace_fopen(filename.c_str(), "r");
    check(file != NULL, "UTF-8 file should reopen through wide fopen");
    if (file != NULL) {
        file = replace_freopen(filename.c_str(), "rb", file);
        check(file != NULL, "UTF-8 file should reopen through wide freopen");
        if (file != NULL) {
            check(std::fclose(file) == 0,
                  "wide freopen stream should close cleanly");
            file = NULL;
        }
    }

    full_filename = win32_get_full_path_utf8(filename.c_str());
    check(full_filename != NULL &&
              std::strstr(full_filename, unicode_component) != NULL,
          "GetFullPathNameW result should round-trip as UTF-8");

    {
        std::string reserved_path = root + "/NUL.txt";
        wchar_t *wide_reserved =
            win32_utf8_path_to_wide(reserved_path.c_str());
        check(wide_reserved != NULL &&
                  std::wcsncmp(wide_reserved, L"\\\\?\\", 4) == 0,
              "reserved DOS path components should use verbatim wide paths");
        free(wide_reserved);

        char *before_reserved_cwd = win32_get_current_directory_utf8();
        errno = 0;
        check(win32_set_current_directory_utf8(reserved_path.c_str()) == -1 &&
                  errno == ENAMETOOLONG,
              "working directories should reject verbatim-only components");
        char *after_reserved_cwd = win32_get_current_directory_utf8();
        check(before_reserved_cwd != NULL && after_reserved_cwd != NULL &&
                  std::strcmp(before_reserved_cwd, after_reserved_cwd) == 0,
              "rejected verbatim working directory should leave CWD unchanged");
        free(before_reserved_cwd);
        free(after_reserved_cwd);
    }

    {
        win32_utf8_dir *dir = win32_opendir_utf8(root.c_str());
        check(dir != NULL, "UTF-8 directory enumeration should open");
        if (dir != NULL) {
            const char *name;
            while ((name = win32_readdir_utf8(dir)) != NULL) {
                if (std::strcmp(name, "\xe6\x95\xb0\xe6\x8d\xae.txt") == 0)
                    found_name = true;
            }
            check(win32_closedir_utf8(dir) == 0,
                  "UTF-8 directory enumeration should close");
        }
    }
    check(found_name, "wide directory enumeration should preserve UTF-8 names");

    {
        std::string pattern = root + "/*";
        check(win32_glob_utf8(pattern.c_str(), &matches, &match_count) == 0,
              "wide glob should succeed on a UTF-8 directory");
        for (size_t i = 0; i < match_count; i++) {
            if (full_filename != NULL &&
                std::strcmp(matches[i], full_filename) == 0)
                found_glob = true;
        }
        check(found_glob, "wide glob should return the UTF-8 filename");
        win32_globfree_utf8(matches, match_count);
        matches = NULL;
        match_count = 0;
    }

    check(replace_mkdir(nested_dir.c_str()) == 0,
          "nested UTF-8 glob directory creation should succeed");
    if (replace_stat64(nested_dir.c_str(), &statbuf) == 0)
        directories.push_back(nested_dir);
    file = replace_fopen(nested_file.c_str(), "wb");
    check(file != NULL, "nested UTF-8 glob file creation should succeed");
    if (file != NULL) {
        check(std::fclose(file) == 0,
              "nested UTF-8 glob file should close cleanly");
        file = NULL;
    }
    full_nested_file = win32_get_full_path_utf8(nested_file.c_str());
    {
        std::string pattern = root +
            "/group-[ab]/\xe9\x85\x8d\xe7\xbd\xae-?.conf";
        bool found_nested = false;
        check(win32_glob_utf8(pattern.c_str(), &matches, &match_count) == 0,
              "wide glob should expand wildcard directory components");
        for (size_t i = 0; i < match_count; i++) {
            if (full_nested_file != NULL &&
                std::strcmp(matches[i], full_nested_file) == 0)
                found_nested = true;
        }
        check(match_count == 1 && found_nested,
              "wide glob should implement bracket and question-mark matching");
        win32_globfree_utf8(matches, match_count);
        matches = NULL;
        match_count = 0;
    }

    check(replace_link(filename.c_str(), hardlink.c_str()) == 0,
          "UTF-8 hard-link creation should succeed");
    check(replace_rename(hardlink.c_str(), renamed.c_str()) == 0,
          "UTF-8 rename should succeed");
    check(replace_stat64(renamed.c_str(), &statbuf) == 0,
          "renamed UTF-8 hard link should exist");

    {
        std::string deep = root;
        for (int i = 0; i < 12; i++) {
            char component[48];
            std::snprintf(component, sizeof(component),
                          "/segment-%02d-abcdefghijklmnop", i);
            deep += component;
            check(replace_mkdir(deep.c_str()) == 0,
                  "extended-length directory creation should succeed");
            if (replace_stat64(deep.c_str(), &statbuf) != 0) break;
            directories.push_back(deep);
        }
        check(deep.size() >= MAX_PATH,
              "interop test should cross the legacy MAX_PATH boundary");
        std::string deep_file = deep + "/\xe6\xb7\xb1\xe5\xb1\x82.txt";
        file = replace_fopen(deep_file.c_str(), "wb");
        check(file != NULL,
              "extended-length UTF-8 file creation should succeed");
        if (file != NULL) {
            check(std::fclose(file) == 0,
                  "extended-length UTF-8 file should close cleanly");
            file = NULL;
            check(replace_stat64(deep_file.c_str(), &statbuf) == 0,
                  "extended-length UTF-8 file should be stat-able");
        }
        check(replace_unlink(deep_file.c_str()) == 0,
              "extended-length UTF-8 file should be removable");

        char *before_long_cwd = win32_get_current_directory_utf8();
        errno = 0;
        check(before_long_cwd != NULL &&
                  win32_set_current_directory_utf8(deep.c_str()) == -1 &&
                  errno == ENAMETOOLONG,
              "process working directory should reject paths at MAX_PATH");
        char *after_long_cwd = win32_get_current_directory_utf8();
        check(before_long_cwd != NULL && after_long_cwd != NULL &&
                  std::strcmp(before_long_cwd, after_long_cwd) == 0,
              "rejected long working directory should leave CWD unchanged");
        free(before_long_cwd);
        free(after_long_cwd);
    }

    {
        std::string missing = root + "/missing-\xe6\x96\x87\xe4\xbb\xb6";
        errno = 0;
        int fd = open(missing.c_str(), _O_RDONLY, 0);
        check(fd == -1 && errno == ENOENT,
              "wide FD open should preserve ENOENT");
        if (fd != -1) FDAPI_close(fd);
    }

    {
        const char invalid_utf8[] = {'b', 'a', 'd', '-', (char)0xff, '\0'};
        errno = 0;
        wchar_t *wide = win32_utf8_to_wide(invalid_utf8);
        check(wide == NULL && errno == EILSEQ,
              "invalid UTF-8 should be rejected at the Windows boundary");
        free(wide);

        errno = 0;
        check(replace_popen(invalid_utf8, "r") == NULL && errno == EILSEQ,
              "wide popen should reject invalid UTF-8 without execution");
        errno = 0;
        check(replace_system(invalid_utf8) == -1 && errno == EILSEQ,
              "wide system should reject invalid UTF-8 without execution");
    }

    {
        const wchar_t env_name[] = L"REDIS_INTEROP_UTF8";
        const wchar_t env_value[] = L"\x8def\x5f84-\xd83d\xde00";
        check(SetEnvironmentVariableW(env_name, env_value) != 0,
              "Unicode environment test value should be set");
        char *value = win32_getenv_utf8("REDIS_INTEROP_UTF8");
        check(value != NULL &&
                  std::strcmp(value,
                              "\xe8\xb7\xaf\xe5\xbe\x84-\xf0\x9f\x98\x80") == 0,
              "wide getenv should return strict UTF-8");
        free(value);

        char *cached = win32_getenv_utf8_cached("REDIS_INTEROP_UTF8");
        check(cached != NULL &&
                  std::strcmp(cached,
                              "\xe8\xb7\xaf\xe5\xbe\x84-\xf0\x9f\x98\x80") == 0,
              "cached wide getenv should return UTF-8");
        check(SetEnvironmentVariableW(env_name, L"updated") != 0,
              "Unicode environment test value should update");
        cached = win32_getenv_utf8_cached("REDIS_INTEROP_UTF8");
        check(cached != NULL && std::strcmp(cached, "updated") == 0,
              "cached wide getenv should refresh without leaking lookups");
        SetEnvironmentVariableW(env_name, NULL);
        check(win32_getenv_utf8_cached("REDIS_INTEROP_UTF8") == NULL,
              "cached wide getenv should observe variable removal");

        errno = 0;
        SetLastError(ERROR_SUCCESS);
        check(win32_getenv_utf8_cached(NULL) == NULL && errno == EINVAL &&
                  GetLastError() == ERROR_INVALID_PARAMETER,
              "cached wide getenv should reject a null variable name");
    }

    saved_cwd = win32_get_current_directory_utf8();
    full_root = win32_get_full_path_utf8(root.c_str());
    check(saved_cwd != NULL && full_root != NULL,
          "wide current-directory helpers should return UTF-8 paths");
    if (saved_cwd != NULL && full_root != NULL) {
        check(win32_set_current_directory_utf8(root.c_str()) == 0,
              "wide current-directory setter should accept UTF-8");
        changed_cwd = win32_get_current_directory_utf8();
        check(changed_cwd != NULL && std::strcmp(changed_cwd, full_root) == 0,
              "wide current-directory getter should preserve UTF-8");
        check(win32_set_current_directory_utf8(saved_cwd) == 0,
              "wide current-directory setter should restore the directory");
    }

cleanup:
    if (file != NULL) std::fclose(file);
    free(changed_cwd);
    free(full_root);
    free(saved_cwd);
    free(full_filename);
    free(full_nested_file);
    win32_globfree_utf8(matches, match_count);
    if (replace_stat64(renamed.c_str(), &statbuf) == 0)
        check(replace_remove(renamed.c_str()) == 0,
              "wide remove should delete a UTF-8 path");
    replace_unlink(hardlink.c_str());
    replace_unlink(nested_file.c_str());
    replace_unlink(filename.c_str());
    for (std::vector<std::string>::reverse_iterator it = directories.rbegin();
         it != directories.rend(); ++it) {
        check(replace_rmdir(it->c_str()) == 0,
              "interop test directory cleanup should succeed");
    }
}

static void *return_thread_argument(void *argument) {
    return argument;
}

static void test_pthread_join_result() {
    int marker = 0x5a17;
    pthread_attr_t attributes;
    size_t stack_size = 0;
    pthread_t thread = 0;
    void *result = NULL;

    check(pthread_attr_init(&attributes) == 0 &&
              pthread_attr_getstacksize(&attributes, &stack_size) == 0 &&
              stack_size == 0,
          "pthread attributes should initialize with the default stack size");
    check(pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024) == 0 &&
              pthread_attr_getstacksize(&attributes, &stack_size) == 0 &&
              stack_size == 8 * 1024 * 1024,
          "pthread stack-size attributes should round trip on Win64");
    check(pthread_create(&thread, &attributes, return_thread_argument, &marker) == 0,
          "pthread_create should retain a joinable Windows handle");
    if (thread == 0) return;
    check(pthread_join(thread, &result) == 0,
          "pthread_join should wait for the real Windows thread handle");
    check(result == &marker,
          "pthread_join should return the thread routine result");
}

struct pthread_identity_context {
    HANDLE start_event;
    volatile LONG completed;
    pthread_t observed_self;
};

static void *capture_pthread_identity(void *argument) {
    pthread_identity_context *context =
        static_cast<pthread_identity_context *>(argument);

    WaitForSingleObject(context->start_event, INFINITE);
    context->observed_self = pthread_self();
    InterlockedExchange(&context->completed, 1);
    return argument;
}

static bool wait_for_thread_completion(pthread_identity_context *context) {
    for (int attempts = 0; attempts < 500; attempts++) {
        if (InterlockedCompareExchange(&context->completed, 0, 0) != 0)
            return true;
        Sleep(10);
    }
    return false;
}

static void test_pthread_identity() {
    pthread_identity_context first = {};
    pthread_identity_context second = {};
    pthread_t first_thread = 0;
    pthread_t second_thread = 0;
    void *result = NULL;

    first.start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    second.start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(first.start_event != NULL && second.start_event != NULL,
          "pthread identity test events should be created");
    if (first.start_event == NULL || second.start_event == NULL) goto cleanup;

    check(pthread_create(&first_thread, NULL, capture_pthread_identity, &first) == 0,
          "first pthread identity thread should be created");
    if (first_thread == 0) goto cleanup;
    SetEvent(first.start_event);
    check(wait_for_thread_completion(&first),
          "first pthread identity thread should complete");
    check(first.observed_self == first_thread,
          "pthread_self should match the opaque pthread_create identity");

    /* Keep the first completed thread unjoined while creating the second.
     * Windows may recycle numeric thread IDs after exit, so pthread_t must be
     * backed by the retained join record rather than GetCurrentThreadId(). */
    check(pthread_create(&second_thread, NULL, capture_pthread_identity, &second) == 0,
          "second pthread identity thread should be created");
    if (second_thread != 0) {
        check(second_thread != first_thread,
              "completed unjoined pthread identities should remain distinct");
        SetEvent(second.start_event);
        check(wait_for_thread_completion(&second),
              "second pthread identity thread should complete");
        check(second.observed_self == second_thread,
              "pthread_self should match the second opaque pthread identity");
    }

cleanup:
    if (second_thread != 0) {
        check(pthread_join(second_thread, &result) == 0,
              "second pthread identity thread should join");
        check(result == &second,
              "second pthread identity join should return its context");
    }
    if (first_thread != 0) {
        result = NULL;
        check(pthread_join(first_thread, &result) == 0,
              "first pthread identity thread should join after the second exits");
        check(result == &first,
              "first pthread identity join should return its context");
    }
    if (second.start_event != NULL) CloseHandle(second.start_event);
    if (first.start_event != NULL) CloseHandle(first.start_event);
}

static void test_llp64_widths() {
    check(win32_llp64_interop_test() != 0,
          "Win64 logical counters and cursors should retain bits above 32");
}

static void test_address_conversion() {
    struct in_addr ipv4 = {};
    struct in_addr ipv4_roundtrip = {};
    struct in6_addr ipv6 = {};
    struct in6_addr ipv6_roundtrip = {};
    char text[INET6_ADDRSTRLEN] = {};

    check(inet_pton(AF_INET, "192.0.2.1", &ipv4) == 1,
          "inet_pton should parse IPv4");
    check(inet_ntop(AF_INET, &ipv4, text, sizeof(text)) == text,
          "inet_ntop should format IPv4");
    check(std::strcmp(text, "192.0.2.1") == 0,
          "inet_ntop should produce the expected IPv4 text");
    check(inet_pton(AF_INET, text, &ipv4_roundtrip) == 1 &&
              std::memcmp(&ipv4, &ipv4_roundtrip, sizeof(ipv4)) == 0,
          "IPv4 text should round trip");

    std::memset(text, 0, sizeof(text));
    check(inet_pton(AF_INET6, "2001:db8::1", &ipv6) == 1,
          "inet_pton should parse IPv6");
    check(inet_ntop(AF_INET6, &ipv6, text, sizeof(text)) == text,
          "inet_ntop should format IPv6");
    check(std::strchr(text, '[') == NULL && std::strchr(text, ']') == NULL,
          "inet_ntop should not add IPv6 brackets");
    check(inet_pton(AF_INET6, text, &ipv6_roundtrip) == 1 &&
              std::memcmp(&ipv6, &ipv6_roundtrip, sizeof(ipv6)) == 0,
          "IPv6 text should round trip");

    check(inet_pton(AF_INET6, "not-an-address", &ipv6) == 0,
          "inet_pton should reject invalid IPv6 text");

    char small[4] = {};
    errno = 0;
    check(inet_ntop(AF_INET6, &ipv6_roundtrip, small, sizeof(small)) == NULL &&
              errno == ENOSPC,
          "inet_ntop should report ENOSPC for a short output buffer");

    errno = 0;
    check(inet_ntop(AF_UNSPEC, &ipv4, text, sizeof(text)) == NULL &&
              errno == EAFNOSUPPORT,
          "inet_ntop should reject unsupported families");
    errno = 0;
    check(inet_pton(AF_UNSPEC, "192.0.2.1", &ipv4) == -1 &&
              errno == EAFNOSUPPORT,
          "inet_pton should reject unsupported families");

    check(checked_windows_6_0 && checked_windows_6_2,
          "address conversion should query the Windows version gates");
}

static void test_getrusage() {
    struct rusage usage = {};

    check(getrusage(RUSAGE_SELF, &usage) == 0,
          "getrusage RUSAGE_SELF should succeed");
    check(usage.ru_utime.tv_sec >= 0 && usage.ru_utime.tv_usec >= 0 &&
              usage.ru_utime.tv_usec < 1000000,
          "getrusage should return a valid user time");
    check(usage.ru_stime.tv_sec >= 0 && usage.ru_stime.tv_usec >= 0 &&
              usage.ru_stime.tv_usec < 1000000,
          "getrusage should return a valid system time");

    std::memset(&usage, 0xff, sizeof(usage));
    check(getrusage(RUSAGE_CHILDREN, &usage) == 0,
          "getrusage RUSAGE_CHILDREN should succeed");
    check(usage.ru_utime.tv_sec == 0 && usage.ru_utime.tv_usec == 0 &&
              usage.ru_stime.tv_sec == 0 && usage.ru_stime.tv_usec == 0,
          "childless Windows usage should be zero");

    errno = 0;
    check(getrusage(1234, &usage) == -1 && errno == EINVAL,
          "getrusage should reject an invalid selector");

    errno = 0;
    check(getrusage(RUSAGE_SELF, NULL) == -1 && errno == EFAULT,
          "getrusage should reject a null output pointer");
}

static void close_eventloop_pipe(int pipefds[2]) {
    if (pipefds[0] != -1) {
        FDAPI_close(pipefds[0]);
        pipefds[0] = -1;
    }
    if (pipefds[1] != -1) {
        FDAPI_close(pipefds[1]);
        pipefds[1] = -1;
    }
}

static void test_synthetic_fd_ftruncate() {
    int reservation_pipe[2] = {-1, -1};
    int fd = -1;
    int stale_fd = -1;
    char filename[] = "redis-interop-ftruncate-XXXXXX";
    const char payload[] = "0123456789abcdef";
    char byte = 0;
    struct __stat64 statbuf = {};

    /* Reserve two Redis descriptors without consuming CRT descriptors.  The
     * temporary file must therefore be accessed through the FDAPI mapping,
     * rather than by treating its Redis descriptor as a native CRT fd. */
    check(FDAPI_pipe_for_eventloop(reservation_pipe) == 0,
          "ftruncate test should reserve synthetic socket descriptors");
    if (reservation_pipe[0] == -1 || reservation_pipe[1] == -1) goto cleanup;

    fd = FDAPI_mkstemp(filename);
    check(fd != -1,
          "ftruncate test temporary file creation should succeed");
    if (fd == -1) goto cleanup;

    check(write(fd, payload, sizeof(payload) - 1) ==
              (ssize_t)(sizeof(payload) - 1),
          "synthetic file descriptor should accept the initial payload");
    check(fdapi_fstat64(fd, &statbuf) == 0 &&
              statbuf.st_size == (off_t)(sizeof(payload) - 1),
          "synthetic file descriptor should report the initial file size");

    check(lseek64(fd, 7, SEEK_SET) == 7,
          "ftruncate test should position the synthetic file descriptor");

    check(ftruncate(fd, 5) == 0,
          "ftruncate should shrink a file through the synthetic descriptor map");
    check(fdapi_fstat64(fd, &statbuf) == 0 && statbuf.st_size == 5,
          "ftruncate should persist the shortened synthetic file size");
    check(lseek64(fd, 0, SEEK_CUR) == 7,
          "ftruncate shrink should preserve the current file offset");

    check(ftruncate(fd, 32) == 0,
          "ftruncate should extend a file through the synthetic descriptor map");
    check(fdapi_fstat64(fd, &statbuf) == 0 && statbuf.st_size == 32,
          "ftruncate should persist the extended synthetic file size");
    check(lseek64(fd, 0, SEEK_CUR) == 7,
          "ftruncate extension should preserve the current file offset");

    errno = 0;
    check(lseek64(fd, -1, SEEK_SET) == -1 && errno == EINVAL,
          "signed lseek64 should preserve negative-offset errors");

    errno = 0;
    check(ftruncate(fd, -1) == -1 && errno == EINVAL,
          "ftruncate should reject a negative file size");

cleanup:
    if (fd != -1) {
        stale_fd = fd;
        check(FDAPI_close(fd) == 0,
              "ftruncate test temporary file should close cleanly");
        fd = -1;
        check(replace_unlink(filename) == 0,
              "ftruncate test temporary file should be removed");
    }
    if (stale_fd != -1) {
        errno = 0;
        check(write(stale_fd, &byte, 1) == -1 && errno == EBADF,
              "write should reject a stale synthetic file descriptor");
        errno = 0;
        check(read(stale_fd, &byte, 1) == -1 && errno == EBADF,
              "read should reject a stale synthetic file descriptor");
        errno = 0;
        check(fsync(stale_fd) == -1 && errno == EBADF,
              "fsync should reject a stale synthetic file descriptor");
        errno = 0;
        check(FDAPI_ftruncate(stale_fd, 0) == -1 && errno == EBADF,
              "ftruncate should reject a stale synthetic file descriptor");
    }
    close_eventloop_pipe(reservation_pipe);
}

static void test_eventloop_pipe() {
    int pipefds[2] = {-1, -1};

    struct pollfd ignored = {};
    ignored.fd = (SOCKET)-1;
    ignored.events = POLLIN;
    ignored.revents = POLLNVAL;
    check(poll(&ignored, 1, 0) == 0 && ignored.revents == 0,
          "poll should ignore negative synthetic descriptors");

    struct pollfd invalid = {};
    invalid.fd = (SOCKET)INT_MAX;
    invalid.events = POLLIN;
    check(poll(&invalid, 1, 1000) == 1 &&
              invalid.revents == POLLNVAL,
          "poll should report invalid synthetic descriptors without waiting");

    check(FDAPI_pipe_for_eventloop(pipefds) == 0,
          "event-loop pipe creation should succeed");
    if (pipefds[0] == -1 || pipefds[1] == -1) return;

    void *read_state = NULL;
    void *write_state = NULL;
    check(FDAPI_GetSocketState(pipefds[0], &read_state) &&
              FDAPI_GetSocketState(pipefds[1], &write_state),
          "event-loop pipe endpoints should be socket descriptors");

    errno = 0;
    check(fsync(pipefds[0]) == -1 && errno == EINVAL,
          "fsync should reject a socket-backed synthetic descriptor");

    int flags = fcntl(pipefds[0], F_GETFL, 0);
    check(flags != -1,
          "event-loop pipe read flags should be available");
    if (flags == -1) {
        close_eventloop_pipe(pipefds);
        return;
    }

    int result = fcntl(pipefds[0], F_SETFL, flags | O_NONBLOCK);
    check(result == 0,
          "event-loop pipe read endpoint should become nonblocking");
    if (result != 0) {
        close_eventloop_pipe(pipefds);
        return;
    }

    unsigned char byte = 0;
    errno = 0;
    ssize_t nread = read(pipefds[0], &byte, 1);
    check(nread == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
          "empty nonblocking event-loop pipe should report EAGAIN");

    result = fcntl(pipefds[0], F_SETFL, flags & ~O_NONBLOCK);
    check(result == 0,
          "event-loop pipe read endpoint should return to blocking mode");
    if (result != 0) {
        close_eventloop_pipe(pipefds);
        return;
    }

    unsigned char payload[4096];
    unsigned char received[sizeof(payload)] = {};
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (unsigned char) ((i * 37) & 0xff);
    }

    size_t written = 0;
    while (written < sizeof(payload)) {
        ssize_t nwritten = write(pipefds[1], payload + written,
                                 sizeof(payload) - written);
        if (nwritten <= 0) break;
        written += (size_t) nwritten;
    }
    check(written == sizeof(payload),
          "event-loop pipe should accept the complete byte stream");

    fd_set readfds;
    struct timeval timeout = {0, 0};
    FD_ZERO(&readfds);
    FD_SET(pipefds[0], &readfds);
    result = select(pipefds[0] + 1, &readfds, NULL, NULL, &timeout);
    check(result == 1 && FD_ISSET(pipefds[0], &readfds),
          "select should restore ready synthetic descriptors to the result set");

    size_t received_len = 0;
    while (received_len < written) {
        ssize_t chunk = read(pipefds[0], received + received_len,
                             written - received_len);
        if (chunk <= 0) break;
        received_len += (size_t) chunk;
    }
    check(received_len == sizeof(payload) &&
              std::memcmp(payload, received, sizeof(payload)) == 0,
          "event-loop pipe should preserve byte-stream contents");

    int read_fd = pipefds[0];
    int write_fd = pipefds[1];
    result = FDAPI_close(write_fd);
    check(result == 0,
          "event-loop pipe writer should close cleanly");
    if (result != 0) {
        close_eventloop_pipe(pipefds);
        return;
    }
    pipefds[1] = -1;

    nread = read(read_fd, &byte, 1);
    check(nread == 0,
          "event-loop pipe reader should observe EOF after writer close");

    result = FDAPI_close(read_fd);
    check(result == 0,
          "event-loop pipe reader should close cleanly");
    if (result == 0) pipefds[0] = -1;

    read_state = write_state = NULL;
    check(!FDAPI_GetSocketState(read_fd, &read_state) &&
              !FDAPI_GetSocketState(write_fd, &write_state),
          "closed event-loop pipe descriptors should leave no socket mapping");

    FD_ZERO(&readfds);
    FD_SET(read_fd, &readfds);
    timeout.tv_sec = timeout.tv_usec = 0;
    errno = 0;
    check(select(read_fd + 1, &readfds, NULL, NULL, &timeout) == -1 &&
              errno == EBADF,
          "select should reject a stale synthetic descriptor");

    close_eventloop_pipe(pipefds);
}

static void test_iocp_blocking_transition() {
    int pipefds[2] = {-1, -1};
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    int result = -1;
    int flags = -1;
    DWORD timeout = 100;
    unsigned char byte = 0;
    DWORD started = 0;
    ssize_t nread = -1;
    DWORD elapsed = 0;

    check(iocp != NULL,
          "IOCP creation should succeed");
    check(FDAPI_pipe_for_eventloop(pipefds) == 0,
          "IOCP blocking test pipe creation should succeed");
    if (iocp == NULL || pipefds[0] == -1 || pipefds[1] == -1) goto cleanup;

    result = FDAPI_SocketAttachIOCP(pipefds[0], iocp);
    check(result == TRUE,
          "event-loop pipe reader should attach to IOCP");
    if (result != TRUE) goto cleanup;

    flags = fcntl(pipefds[0], F_GETFL, 0);
    check(flags != -1 && (flags & O_NONBLOCK) != 0,
          "IOCP attachment should record native nonblocking mode");
    if (flags == -1 || (flags & O_NONBLOCK) == 0) goto cleanup;

    result = fcntl(pipefds[0], F_SETFL, flags & ~O_NONBLOCK);
    check(result == 0,
          "IOCP-backed reader should switch to blocking mode");
    if (result != 0) goto cleanup;

    result = setsockopt(pipefds[0], SOL_SOCKET, SO_RCVTIMEO,
                        &timeout, sizeof(timeout));
    check(result == 0,
          "IOCP-backed reader should accept a Windows receive timeout");
    if (result != 0) goto cleanup;

    started = GetTickCount();
    errno = 0;
    nread = read(pipefds[0], &byte, 1);
    elapsed = GetTickCount() - started;
    check(nread == -1 && errno == ETIMEDOUT,
          "blocking IOCP-backed read should report receive timeout");
    check(elapsed >= 50,
          "blocking IOCP-backed read should wait instead of returning EAGAIN");

cleanup:
    close_eventloop_pipe(pipefds);
    if (iocp != NULL) CloseHandle(iocp);
}

static void test_socket_duplication() {
    int data_pipe[2] = {-1, -1};
    int exit_pipe[2] = {-1, -1};
    int child_data_writer = -1;
    int child_exit_reader = -1;
    unsigned char byte = 0;
    WSAPROTOCOL_INFOW data_protocol = {};
    WSAPROTOCOL_INFOW exit_protocol = {};

    check(FDAPI_pipe_for_eventloop(data_pipe) == 0 &&
              FDAPI_pipe_for_eventloop(exit_pipe) == 0,
          "two event-loop pipes should be available for QFork duplication");
    if (data_pipe[0] == -1 || data_pipe[1] == -1 ||
        exit_pipe[0] == -1 || exit_pipe[1] == -1) {
        close_eventloop_pipe(data_pipe);
        close_eventloop_pipe(exit_pipe);
        return;
    }

    check(FDAPI_WSADuplicateSocket(data_pipe[1], GetCurrentProcessId(),
                                   &data_protocol) == 0,
          "QFork data writer duplication should succeed");
    check(FDAPI_WSADuplicateSocket(exit_pipe[0], GetCurrentProcessId(),
                                   &exit_protocol) == 0,
          "QFork exit reader duplication should succeed after the data writer");

    child_data_writer = FDAPI_WSASocket(FROM_PROTOCOL_INFO,
                                        FROM_PROTOCOL_INFO,
                                        FROM_PROTOCOL_INFO,
                                        &data_protocol, 0,
                                        WSA_FLAG_OVERLAPPED);
    child_exit_reader = FDAPI_WSASocket(FROM_PROTOCOL_INFO,
                                        FROM_PROTOCOL_INFO,
                                        FROM_PROTOCOL_INFO,
                                        &exit_protocol, 0,
                                        WSA_FLAG_OVERLAPPED);
    check(child_data_writer != -1 && child_exit_reader != -1,
          "QFork child sockets should be recreated from wide protocol info");
    if (child_data_writer == -1 || child_exit_reader == -1) goto cleanup;

    check(FDAPI_close(data_pipe[1]) == 0,
          "parent data writer should close after duplication");
    data_pipe[1] = -1;
    check(FDAPI_close(exit_pipe[0]) == 0,
          "parent exit reader should close after duplication");
    exit_pipe[0] = -1;

    byte = 0x5a;
    check(write(child_data_writer, &byte, 1) == 1,
          "duplicated data writer should send bytes");
    byte = 0;
    check(read(data_pipe[0], &byte, 1) == 1 && byte == 0x5a,
          "parent data reader should receive duplicated-writer bytes");

    check(FDAPI_close(exit_pipe[1]) == 0,
          "parent exit writer should close to release the child");
    exit_pipe[1] = -1;
    check(read(child_exit_reader, &byte, 1) == 0,
          "duplicated exit reader should observe parent EOF");

cleanup:
    if (child_data_writer != -1) {
        check(FDAPI_CloseDuplicatedSocket(child_data_writer) == TRUE,
              "duplicated data writer should close cleanly");
    }
    if (child_exit_reader != -1) {
        check(FDAPI_CloseDuplicatedSocket(child_exit_reader) == TRUE,
              "duplicated exit reader should close cleanly");
    }
    close_eventloop_pipe(data_pipe);
    close_eventloop_pipe(exit_pipe);
}

int main(int argc, char **argv) {
    if (argc != 2 ||
        (std::strcmp(argv[1], "--legacy") != 0 &&
         std::strcmp(argv[1], "--modern") != 0)) {
        std::fprintf(stderr, "usage: %s --legacy|--modern\n", argv[0]);
        return 2;
    }

    emulate_modern_windows = std::strcmp(argv[1], "--modern") == 0;
    test_secure_random();
    test_dns_ascii_policy();
    test_error_translation();
    test_utf8_filesystem();
    test_pthread_join_result();
    test_pthread_identity();
    test_llp64_widths();
    test_address_conversion();
    test_getrusage();
    test_synthetic_fd_ftruncate();
    test_eventloop_pipe();
    test_iocp_blocking_transition();
    test_socket_duplication();

    if (failures != 0) {
        std::fprintf(stderr, "%d interop test(s) failed\n", failures);
        return 1;
    }

    std::printf("ALL INTEROP TESTS PASSED (%s)\n",
                emulate_modern_windows ? "modern" : "legacy");
    return 0;
}
