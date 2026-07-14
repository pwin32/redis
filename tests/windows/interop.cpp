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

static void test_eventloop_pipe() {
    int pipefds[2] = {-1, -1};

    check(FDAPI_pipe_for_eventloop(pipefds) == 0,
          "event-loop pipe creation should succeed");
    if (pipefds[0] == -1 || pipefds[1] == -1) return;

    check(FDAPI_GetSocketStatePtr(pipefds[0]) != NULL &&
              FDAPI_GetSocketStatePtr(pipefds[1]) != NULL,
          "event-loop pipe endpoints should be socket descriptors");

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

    check(FDAPI_GetSocketStatePtr(read_fd) == NULL &&
              FDAPI_GetSocketStatePtr(write_fd) == NULL,
          "closed event-loop pipe descriptors should leave no socket mapping");

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
    if (child_data_writer != -1) FDAPI_close(child_data_writer);
    if (child_exit_reader != -1) FDAPI_close(child_exit_reader);
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
    test_address_conversion();
    test_getrusage();
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
