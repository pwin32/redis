#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

typedef struct WideBuffer {
    wchar_t *data;
    size_t length;
    size_t capacity;
} WideBuffer;

static int buffer_reserve(WideBuffer *buffer, size_t additional) {
    size_t required;
    size_t capacity;
    wchar_t *data;

    if (additional > SIZE_MAX - buffer->length - 1) return 0;
    required = buffer->length + additional + 1;
    if (required <= buffer->capacity) return 1;

    capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*data)) return 0;

    data = (wchar_t *)realloc(buffer->data, capacity * sizeof(*data));
    if (data == NULL) return 0;
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append_char(WideBuffer *buffer, wchar_t value) {
    if (!buffer_reserve(buffer, 1)) return 0;
    buffer->data[buffer->length++] = value;
    buffer->data[buffer->length] = L'\0';
    return 1;
}

static int buffer_append_repeat(WideBuffer *buffer, wchar_t value,
                                size_t count) {
    size_t i;

    if (!buffer_reserve(buffer, count)) return 0;
    for (i = 0; i < count; i++) buffer->data[buffer->length++] = value;
    buffer->data[buffer->length] = L'\0';
    return 1;
}

static int buffer_append_string(WideBuffer *buffer, const wchar_t *value) {
    size_t length = wcslen(value);

    if (!buffer_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->length, value,
           length * sizeof(*buffer->data));
    buffer->length += length;
    buffer->data[buffer->length] = L'\0';
    return 1;
}

/* Quote one argument using the Microsoft C runtime command-line rules. */
static int buffer_append_argument(WideBuffer *buffer, const wchar_t *argument) {
    const wchar_t *cursor;
    size_t backslashes = 0;
    int needs_quotes = argument[0] == L'\0';

    for (cursor = argument; !needs_quotes && *cursor != L'\0'; cursor++) {
        if (iswspace(*cursor) || *cursor == L'"') needs_quotes = 1;
    }
    if (!needs_quotes) return buffer_append_string(buffer, argument);

    if (!buffer_append_char(buffer, L'"')) return 0;
    for (cursor = argument; *cursor != L'\0'; cursor++) {
        if (*cursor == L'\\') {
            backslashes++;
            continue;
        }

        if (*cursor == L'"') {
            if (backslashes > (SIZE_MAX - 1) / 2) return 0;
            if (!buffer_append_repeat(buffer, L'\\', backslashes * 2 + 1) ||
                !buffer_append_char(buffer, L'"')) {
                return 0;
            }
        } else {
            if (!buffer_append_repeat(buffer, L'\\', backslashes) ||
                !buffer_append_char(buffer, *cursor)) {
                return 0;
            }
        }
        backslashes = 0;
    }

    if (backslashes > SIZE_MAX / 2 ||
        !buffer_append_repeat(buffer, L'\\', backslashes * 2) ||
        !buffer_append_char(buffer, L'"')) {
        return 0;
    }
    return 1;
}

static wchar_t *build_command_line(int argc, wchar_t **argv, int first) {
    WideBuffer buffer = {0};
    int index;

    SetLastError(ERROR_SUCCESS);
    for (index = first; index < argc; index++) {
        if (index != first && !buffer_append_char(&buffer, L' ')) goto error;
        if (!buffer_append_argument(&buffer, argv[index])) goto error;
    }

    /* CreateProcessW is limited to 32,767 characters including the NUL. */
    if (buffer.length >= 32767) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        goto error;
    }
    return buffer.data;

error:
    free(buffer.data);
    if (GetLastError() == ERROR_SUCCESS) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

static HANDLE open_append_file(const wchar_t *path,
                               SECURITY_ATTRIBUTES *attributes) {
    return CreateFileW(path,
                       FILE_APPEND_DATA | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       attributes,
                       OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

static void report_error(const char *operation, DWORD error) {
    fprintf(stderr, "%s failed with Windows error %lu\n",
            operation, (unsigned long)error);
}

static int parse_process_id(const wchar_t *text, DWORD *process_id) {
    wchar_t *end = NULL;
    unsigned long value;
    const wchar_t *cursor;

    if (text == NULL || text[0] == L'\0') return 0;
    for (cursor = text; *cursor != L'\0'; cursor++) {
        if (!iswdigit(*cursor)) return 0;
    }

    errno = 0;
    value = wcstoul(text, &end, 10);
    if (errno != 0 || end == text || *end != L'\0' ||
        value == 0 || value > MAXDWORD) {
        return 0;
    }
    *process_id = (DWORD)value;
    return 1;
}

static HANDLE open_process_for_query(DWORD process_id) {
    return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                       FALSE,
                       process_id);
}

static HANDLE open_process_for_command_line(DWORD process_id) {
    return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                       FALSE,
                       process_id);
}

static HANDLE open_process_for_control(DWORD process_id) {
    return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                           PROCESS_SUSPEND_RESUME,
                       FALSE,
                       process_id);
}

static int process_handle_is_alive(HANDLE process) {
    DWORD exit_code;

    if (!GetExitCodeProcess(process, &exit_code)) return 0;
    return exit_code == STILL_ACTIVE;
}

static wchar_t *query_process_image(HANDLE process) {
    const DWORD capacity = 32768;
    DWORD length = capacity;
    wchar_t *path = (wchar_t *)malloc(capacity * sizeof(*path));

    if (path == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!QueryFullProcessImageNameW(process, 0, path, &length)) {
        free(path);
        return NULL;
    }
    path[length] = L'\0';
    return path;
}

static wchar_t *normalize_path(const wchar_t *path) {
    DWORD required;
    DWORD length;
    wchar_t *normalized;
    size_t index;

    required = GetFullPathNameW(path, 0, NULL, NULL);
    if (required == 0) return NULL;
    if (required == MAXDWORD) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    normalized = (wchar_t *)malloc(((size_t)required + 1) *
                                   sizeof(*normalized));
    if (normalized == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    length = GetFullPathNameW(path, required + 1, normalized, NULL);
    if (length == 0 || length > required) {
        free(normalized);
        return NULL;
    }

    for (index = 0; normalized[index] != L'\0'; index++) {
        if (normalized[index] == L'/') normalized[index] = L'\\';
    }
    return normalized;
}

static int process_image_matches(HANDLE process,
                                 const wchar_t *expected_image) {
    wchar_t *actual = NULL;
    wchar_t *actual_normalized = NULL;
    wchar_t *expected_normalized = NULL;
    int matches = 0;

    actual = query_process_image(process);
    if (actual == NULL) goto cleanup;
    actual_normalized = normalize_path(actual);
    if (actual_normalized == NULL) goto cleanup;
    expected_normalized = normalize_path(expected_image);
    if (expected_normalized == NULL) goto cleanup;

    matches = _wcsicmp(actual_normalized, expected_normalized) == 0;

cleanup:
    free(expected_normalized);
    free(actual_normalized);
    free(actual);
    return matches;
}

static int query_command_is_alive(int argc, wchar_t **argv) {
    DWORD process_id;
    HANDLE process;
    int alive;

    if (argc != 3 || !parse_process_id(argv[2], &process_id)) {
        fprintf(stderr, "usage: redis-test-launcher.exe --is-alive PID\n");
        return 2;
    }

    process = open_process_for_query(process_id);
    if (process == NULL) return 1;
    alive = process_handle_is_alive(process);
    CloseHandle(process);
    return alive ? 0 : 1;
}

static int query_command_is_owned(int argc, wchar_t **argv) {
    DWORD process_id;
    HANDLE process;
    int index;

    if (argc < 4 || !parse_process_id(argv[2], &process_id)) {
        fprintf(stderr,
                "usage: redis-test-launcher.exe --is-owned PID "
                "expected-executable [... ]\n");
        return 2;
    }

    process = open_process_for_query(process_id);
    if (process == NULL) return 1;
    if (!process_handle_is_alive(process)) {
        CloseHandle(process);
        return 1;
    }
    for (index = 3; index < argc; index++) {
        if (process_image_matches(process, argv[index])) {
            CloseHandle(process);
            return 0;
        }
    }
    CloseHandle(process);
    return 1;
}

static int query_command_image(int argc, wchar_t **argv) {
    DWORD process_id;
    HANDLE process;
    wchar_t *path;
    int result = 1;

    if (argc != 3 || !parse_process_id(argv[2], &process_id)) {
        fprintf(stderr, "usage: redis-test-launcher.exe --image PID\n");
        return 2;
    }

    process = open_process_for_query(process_id);
    if (process == NULL) return 1;
    if (!process_handle_is_alive(process)) goto cleanup;
    path = query_process_image(process);
    if (path == NULL) goto cleanup;
    if (wprintf(L"%ls\n", path) >= 0 && fflush(stdout) == 0) result = 0;
    free(path);

cleanup:
    CloseHandle(process);
    return result;
}

typedef NTSTATUS (NTAPI *NtProcessControlFunction)(HANDLE process);

static int control_process(DWORD process_id,
                           const wchar_t *expected_image,
                           const char *function_name) {
    HANDLE process;
    HMODULE ntdll;
    FARPROC raw_function;
    NtProcessControlFunction function;
    NTSTATUS status;

    process = open_process_for_control(process_id);
    if (process == NULL) return 1;
    if (!process_handle_is_alive(process) ||
        !process_image_matches(process, expected_image)) {
        CloseHandle(process);
        return 1;
    }

    ntdll = GetModuleHandleW(L"ntdll.dll");
    raw_function = ntdll == NULL ? NULL :
        GetProcAddress(ntdll, function_name);
    function = NULL;
    if (raw_function != NULL)
        memcpy(&function, &raw_function, sizeof(function));
    if (function == NULL) {
        CloseHandle(process);
        return 1;
    }
    status = function(process);
    CloseHandle(process);
    return status == 0 ? 0 : 1;
}

static int process_command_line_has_qfork(HANDLE process) {
    typedef NTSTATUS (NTAPI *NtQueryInformationProcessFunction)(
        HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    NtQueryInformationProcessFunction query_information = NULL;
    PROCESS_BASIC_INFORMATION basic;
    PEB peb;
    RTL_USER_PROCESS_PARAMETERS parameters;
    wchar_t *text = NULL;
    ULONG return_length = 0;
    SIZE_T bytes_read = 0;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC raw_function = ntdll == NULL ? NULL :
        GetProcAddress(ntdll, "NtQueryInformationProcess");
    int found = 0;

    if (raw_function != NULL)
        memcpy(&query_information, &raw_function, sizeof(query_information));
    if (query_information == NULL ||
        query_information(process, ProcessBasicInformation, &basic,
                          sizeof(basic), &return_length) != 0 ||
        basic.PebBaseAddress == NULL ||
        !ReadProcessMemory(process, basic.PebBaseAddress, &peb,
                           sizeof(peb), &bytes_read) ||
        peb.ProcessParameters == NULL ||
        !ReadProcessMemory(process, peb.ProcessParameters, &parameters,
                           sizeof(parameters), &bytes_read) ||
        parameters.CommandLine.Buffer == NULL ||
        parameters.CommandLine.Length == 0) {
        return 0;
    }

    text = (wchar_t *)malloc((size_t)parameters.CommandLine.Length +
                             sizeof(wchar_t));
    if (text == NULL ||
        !ReadProcessMemory(process, parameters.CommandLine.Buffer, text,
                           parameters.CommandLine.Length, &bytes_read)) {
        free(text);
        return 0;
    }
    text[parameters.CommandLine.Length / sizeof(wchar_t)] = L'\0';

    for (size_t index = 0; text[index] != L'\0'; index++) {
        if (_wcsnicmp(text + index, L"--qfork", 7) == 0 &&
            (index == 0 || iswspace(text[index - 1])) &&
            (text[index + 7] == L'\0' || iswspace(text[index + 7]))) {
            found = 1;
            break;
        }
    }
    free(text);
    return found;
}

static int find_qfork_child(DWORD parent_id,
                            const wchar_t *expected_image,
                            DWORD *child_id) {
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    DWORD match = 0;
    int matches = 0;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 1;

    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            HANDLE process;
            if ((DWORD)entry.th32ParentProcessID != parent_id) continue;
            process = open_process_for_command_line(entry.th32ProcessID);
            if (process == NULL) continue;
            if (process_handle_is_alive(process) &&
                process_image_matches(process, expected_image) &&
                process_command_line_has_qfork(process)) {
                match = entry.th32ProcessID;
                matches++;
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (matches != 1) return 1;
    *child_id = match;
    return 0;
}

static int query_command_find_qfork_child(int argc, wchar_t **argv) {
    DWORD parent_id;
    DWORD child_id;

    if (argc != 4 || !parse_process_id(argv[2], &parent_id)) {
        fprintf(stderr,
                "usage: redis-test-launcher.exe --find-qfork-child PID "
                "expected-executable\n");
        return 2;
    }
    if (find_qfork_child(parent_id, argv[3], &child_id)) return 1;
    if (printf("%lu\n", (unsigned long)child_id) < 0 ||
        fflush(stdout) != 0) {
        return 1;
    }
    return 0;
}

static int query_command_control(int argc, wchar_t **argv,
                                 const char *function_name) {
    DWORD process_id;

    if (argc != 4 || !parse_process_id(argv[2], &process_id)) {
        fprintf(stderr,
                "usage: redis-test-launcher.exe --suspend|--resume PID "
                "expected-executable\n");
        return 2;
    }
    return control_process(process_id, argv[3], function_name);
}

static int parse_unsigned_value(const wchar_t *text,
                                unsigned long maximum,
                                unsigned long *value) {
    wchar_t *end = NULL;
    unsigned long parsed;
    const wchar_t *cursor;

    if (text == NULL || text[0] == L'\0') return 0;
    for (cursor = text; *cursor != L'\0'; cursor++) {
        if (!iswdigit(*cursor)) return 0;
    }

    errno = 0;
    parsed = wcstoul(text, &end, 10);
    if (errno != 0 || end == text || *end != L'\0' || parsed > maximum)
        return 0;
    *value = parsed;
    return 1;
}

static char *wide_to_utf8(const wchar_t *value) {
    int required = WideCharToMultiByte(CP_UTF8, 0, value, -1,
                                       NULL, 0, NULL, NULL);
    char *converted;

    if (required <= 0) return NULL;
    converted = (char *)malloc((size_t)required);
    if (converted == NULL) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1,
                            converted, required, NULL, NULL) <= 0) {
        free(converted);
        return NULL;
    }
    return converted;
}

static int socket_send_all(SOCKET socket, const char *buffer, size_t length) {
    while (length != 0) {
        int chunk = length > INT_MAX ? INT_MAX : (int)length;
        int sent = send(socket, buffer, chunk, 0);
        if (sent == SOCKET_ERROR || sent == 0) return 0;
        buffer += sent;
        length -= (size_t)sent;
    }
    return 1;
}

static int socket_receive_line(SOCKET socket, char *line, size_t capacity) {
    size_t length = 0;

    while (length + 1 < capacity) {
        char byte;
        int received = recv(socket, &byte, 1, 0);
        if (received != 1) return 0;
        line[length++] = byte;
        if (length >= 2 && line[length - 2] == '\r' && line[length - 1] == '\n') {
            line[length] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Connect with a deliberately tiny receive buffer, issue a named GET without
 * reading for hold_ms, then drain and validate the complete bulk reply. */
static int query_command_slow_reader(int argc, wchar_t **argv) {
    WSADATA winsock;
    SOCKET socket = INVALID_SOCKET;
    struct sockaddr_in address;
    unsigned long port, receive_buffer, hold_ms, expected_bytes, repeat;
    DWORD io_timeout = 30000;
    char *name = NULL;
    char *key = NULL;
    char *request = NULL;
    char line[128];
    char buffer[65536];
    size_t request_capacity;
    unsigned long remaining;
    int result = 1;

    if (argc != 10 ||
        !parse_unsigned_value(argv[3], 65535, &port) || port == 0 ||
        !parse_unsigned_value(argv[6], INT_MAX, &receive_buffer) ||
        receive_buffer == 0 ||
        !parse_unsigned_value(argv[7], MAXDWORD, &hold_ms)) {
        fprintf(stderr,
                "usage: redis-test-launcher.exe --slow-reader HOST PORT "
                "NAME KEY RECEIVE_BUFFER HOLD_MS EXPECTED_BYTES REPEAT\n");
        return 2;
    }

    if (!parse_unsigned_value(argv[8], MAXDWORD, &expected_bytes) ||
        !parse_unsigned_value(argv[9], 4096, &repeat) || repeat == 0) {
        fprintf(stderr,
                "usage: redis-test-launcher.exe --slow-reader HOST PORT "
                "NAME KEY RECEIVE_BUFFER HOLD_MS EXPECTED_BYTES REPEAT\n");
        return 2;
    }

    name = wide_to_utf8(argv[4]);
    key = wide_to_utf8(argv[5]);
    if (name == NULL || key == NULL) goto cleanup;

    size_t get_capacity = strlen(key) + 64;
    if (repeat > (SIZE_MAX - strlen(name) - 256) / get_capacity)
        goto cleanup;
    request_capacity = strlen(name) + 256 + repeat * get_capacity;
    request = (char *)malloc(request_capacity);
    if (request == NULL) goto cleanup;
    int request_length = snprintf(
        request, request_capacity,
        "*3\r\n$6\r\nCLIENT\r\n$7\r\nSETNAME\r\n$%llu\r\n%s\r\n",
        (unsigned long long)strlen(name), name);
    if (request_length < 0 || (size_t)request_length >= request_capacity)
        goto cleanup;
    size_t request_length_total = (size_t)request_length;
    for (unsigned long index = 0; index < repeat; index++) {
        request_length = snprintf(
            request + request_length_total,
            request_capacity - request_length_total,
            "*2\r\n$3\r\nGET\r\n$%llu\r\n%s\r\n",
            (unsigned long long)strlen(key), key);
        if (request_length < 0 ||
            (size_t)request_length >= request_capacity - request_length_total)
            goto cleanup;
        request_length_total += (size_t)request_length;
    }

    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) goto cleanup;
    socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                        NULL, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) goto winsock_cleanup;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                   (const char *)&receive_buffer,
                   sizeof(receive_buffer)) == SOCKET_ERROR ||
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&io_timeout,
                   sizeof(io_timeout)) == SOCKET_ERROR ||
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&io_timeout,
                   sizeof(io_timeout)) == SOCKET_ERROR) {
        goto winsock_cleanup;
    }

    ZeroMemory(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    if (InetPtonW(AF_INET, argv[2], &address.sin_addr) != 1 ||
        connect(socket, (const struct sockaddr *)&address,
                sizeof(address)) == SOCKET_ERROR ||
        !socket_send_all(socket, request, request_length_total)) {
        goto winsock_cleanup;
    }

    Sleep((DWORD)hold_ms);

    if (!socket_receive_line(socket, line, sizeof(line)) ||
        strcmp(line, "+OK\r\n") != 0) {
        goto winsock_cleanup;
    }

    for (unsigned long reply_index = 0; reply_index < repeat; reply_index++) {
        if (!socket_receive_line(socket, line, sizeof(line)) || line[0] != '$')
            goto winsock_cleanup;

        char *end = NULL;
        errno = 0;
        unsigned long bulk_length = strtoul(line + 1, &end, 10);
        if (errno != 0 || end == line + 1 || strcmp(end, "\r\n") != 0 ||
            bulk_length != expected_bytes) {
            goto winsock_cleanup;
        }

        remaining = bulk_length;
        while (remaining != 0) {
            int wanted = remaining > sizeof(buffer) ?
                         (int)sizeof(buffer) : (int)remaining;
            int received = recv(socket, buffer, wanted, 0);
            int index;
            if (received <= 0) goto winsock_cleanup;
            for (index = 0; index < received; index++) {
                if (buffer[index] != 'x') goto winsock_cleanup;
            }
            remaining -= (unsigned long)received;
        }
        if (!socket_receive_line(socket, line, sizeof(line)) ||
            strcmp(line, "\r\n") != 0) {
            goto winsock_cleanup;
        }
    }

    printf("drained_replies=%lu bytes=%lu\n", repeat, expected_bytes);
    fflush(stdout);
    result = 0;

winsock_cleanup:
    if (socket != INVALID_SOCKET) closesocket(socket);
    WSACleanup();
cleanup:
    free(request);
    free(key);
    free(name);
    return result;
}

int wmain(int argc, wchar_t **argv) {
    SECURITY_ATTRIBUTES attributes;
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = NULL;
    SIZE_T attribute_list_size = 0;
    HANDLE inherited_handles[3] = {
        INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE
    };
    wchar_t *command_line = NULL;
    DWORD error = ERROR_SUCCESS;
    int attribute_list_initialized = 0;
    int process_created = 0;
    int exit_code = 1;

    if (argc >= 2 && wcscmp(argv[1], L"--is-alive") == 0)
        return query_command_is_alive(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"--is-owned") == 0)
        return query_command_is_owned(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"--image") == 0)
        return query_command_image(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"--find-qfork-child") == 0)
        return query_command_find_qfork_child(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"--suspend") == 0)
        return query_command_control(argc, argv, "NtSuspendProcess");
    if (argc >= 2 && wcscmp(argv[1], L"--resume") == 0)
        return query_command_control(argc, argv, "NtResumeProcess");
    if (argc >= 2 && wcscmp(argv[1], L"--slow-reader") == 0)
        return query_command_slow_reader(argc, argv);

    if (argc < 5 || wcscmp(argv[3], L"--") != 0) {
        fprintf(stderr,
                "usage: redis-test-launcher.exe --is-alive PID\n"
                "       redis-test-launcher.exe --is-owned PID "
                "expected-executable\n"
                "       redis-test-launcher.exe --image PID\n"
                "       redis-test-launcher.exe --find-qfork-child PID "
                "expected-executable\n"
                "       redis-test-launcher.exe --suspend PID "
                "expected-executable\n"
                "       redis-test-launcher.exe --resume PID "
                "expected-executable\n"
                "       redis-test-launcher.exe --slow-reader HOST PORT "
                "NAME KEY RECEIVE_BUFFER HOLD_MS EXPECTED_BYTES REPEAT\n"
                "usage: redis-test-launcher.exe stdout stderr -- executable "
                "[argument ...]\n");
        return 2;
    }

    ZeroMemory(&attributes, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    inherited_handles[0] = CreateFileW(L"NUL",
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &attributes,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        NULL);
    if (inherited_handles[0] == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        report_error("opening NUL for standard input", error);
        goto cleanup;
    }

    inherited_handles[1] = open_append_file(argv[1], &attributes);
    if (inherited_handles[1] == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        report_error("opening the server stdout log", error);
        goto cleanup;
    }

    inherited_handles[2] = open_append_file(argv[2], &attributes);
    if (inherited_handles[2] == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        report_error("opening the server stderr log", error);
        goto cleanup;
    }

    command_line = build_command_line(argc, argv, 4);
    if (command_line == NULL) {
        error = GetLastError();
        report_error("building the child command line", error);
        goto cleanup;
    }

    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_list_size);
    if (attribute_list_size == 0) {
        error = GetLastError();
        report_error("sizing the inherited-handle list", error);
        goto cleanup;
    }

    attribute_list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
        GetProcessHeap(), 0, attribute_list_size);
    if (attribute_list == NULL) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        report_error("allocating the inherited-handle list", error);
        goto cleanup;
    }
    if (!InitializeProcThreadAttributeList(
            attribute_list, 1, 0, &attribute_list_size)) {
        error = GetLastError();
        report_error("initializing the inherited-handle list", error);
        goto cleanup;
    }
    attribute_list_initialized = 1;
    if (!UpdateProcThreadAttribute(
            attribute_list,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles,
            sizeof(inherited_handles),
            NULL,
            NULL)) {
        error = GetLastError();
        report_error("setting the inherited-handle list", error);
        goto cleanup;
    }

    ZeroMemory(&startup, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = inherited_handles[0];
    startup.StartupInfo.hStdOutput = inherited_handles[1];
    startup.StartupInfo.hStdError = inherited_handles[2];
    startup.lpAttributeList = attribute_list;

    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(argv[4],
                        command_line,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        NULL,
                        NULL,
                        &startup.StartupInfo,
                        &process)) {
        error = GetLastError();
        report_error("creating the hidden server process", error);
        goto cleanup;
    }
    process_created = 1;

    CloseHandle(process.hThread);
    process.hThread = NULL;
    if (printf("%lu\n", (unsigned long)process.dwProcessId) < 0 ||
        fflush(stdout) != 0) {
        fprintf(stderr, "writing the child process ID failed\n");
        TerminateProcess(process.hProcess, ERROR_BROKEN_PIPE);
        WaitForSingleObject(process.hProcess, 5000);
        goto cleanup;
    }
    exit_code = 0;

cleanup:
    if (process_created && process.hProcess != NULL)
        CloseHandle(process.hProcess);
    if (attribute_list_initialized)
        DeleteProcThreadAttributeList(attribute_list);
    if (attribute_list != NULL)
        HeapFree(GetProcessHeap(), 0, attribute_list);
    free(command_line);
    if (inherited_handles[2] != INVALID_HANDLE_VALUE)
        CloseHandle(inherited_handles[2]);
    if (inherited_handles[1] != INVALID_HANDLE_VALUE)
        CloseHandle(inherited_handles[1]);
    if (inherited_handles[0] != INVALID_HANDLE_VALUE)
        CloseHandle(inherited_handles[0]);
    SetLastError(error);
    return exit_code;
}
