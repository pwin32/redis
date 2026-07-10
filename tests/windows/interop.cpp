#include <cerrno>
#include <cstdio>
#include <cstring>

#define FDAPI_NOCRTREDEFS
#include "Win32_Interop/Win32_FDAPI.h"
#include "Win32_Interop/Win32_Common.h"

extern "C" {
#include "Win32_Interop/Win32_Signal_Process.h"
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

extern "C" void set_errno_from_last_error(void) {
}

static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
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
    check(inet_ntop(AF_INET6, &ipv6_roundtrip, small, sizeof(small)) == NULL,
          "inet_ntop should reject a short output buffer");

    if (!emulate_modern_windows) {
        errno = 0;
        check(inet_ntop(AF_UNSPEC, &ipv4, text, sizeof(text)) == NULL &&
                  errno == EAFNOSUPPORT,
              "legacy inet_ntop should reject unsupported families");
    }

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

int main(int argc, char **argv) {
    if (argc != 2 ||
        (std::strcmp(argv[1], "--legacy") != 0 &&
         std::strcmp(argv[1], "--modern") != 0)) {
        std::fprintf(stderr, "usage: %s --legacy|--modern\n", argv[0]);
        return 2;
    }

    emulate_modern_windows = std::strcmp(argv[1], "--modern") == 0;
    test_address_conversion();
    test_getrusage();

    if (failures != 0) {
        std::fprintf(stderr, "%d interop test(s) failed\n", failures);
        return 1;
    }

    std::printf("ALL INTEROP TESTS PASSED (%s)\n",
                emulate_modern_windows ? "modern" : "legacy");
    return 0;
}
