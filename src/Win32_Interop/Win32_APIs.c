/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Win32_APIs.h"
#include "Win32_Error.h"
#include <errno.h>
#include <stdlib.h>

RtlGenRandomFunc RtlGenRandom;

static void win32_sleep_milliseconds(PORT_ULONGLONG milliseconds) {
    const DWORD max_finite_sleep = MAXDWORD - 1;

    while (milliseconds > max_finite_sleep) {
        Sleep(max_finite_sleep);
        milliseconds -= max_finite_sleep;
    }
    Sleep((DWORD)milliseconds);
}

unsigned int win32_sleep(PORT_LONGLONG seconds) {
    const PORT_LONGLONG max_chunk_seconds = (MAXDWORD - 1) / 1000;

    if (seconds <= 0) return 0;

    while (seconds > max_chunk_seconds) {
        Sleep((DWORD)(max_chunk_seconds * 1000));
        seconds -= max_chunk_seconds;
    }
    Sleep((DWORD)(seconds * 1000));
    return 0;
}

int win32_usleep(PORT_LONGLONG usec) {
    if (usec == 1) {
        Sleep(0);
    } else if (usec > 0) {
        win32_sleep_milliseconds((PORT_ULONGLONG)usec / 1000);
    }
    return 0;
}

/* Replace MS C rtl rand which is 15bit with 32 bit */
int replace_random() {
    unsigned int x = 0;
    if (RtlGenRandom == NULL) {
        // Load proc if not loaded
        HMODULE lib = LoadLibraryW(L"advapi32.dll");
        RtlGenRandom = (RtlGenRandomFunc) GetProcAddress(lib, "SystemFunction036");
        if (RtlGenRandom == NULL) return 1;
    }
    RtlGenRandom(&x, sizeof(unsigned int));
    return (int) (x >> 1);
}

/* Rename which works on Windows when file exists */
int replace_rename(const char *src, const char *dst) {
    int retries = 50;
    DWORD error;
    wchar_t *wide_src = win32_utf8_path_to_wide(src);
    wchar_t *wide_dst;

    if (wide_src == NULL) return -1;
    wide_dst = win32_utf8_path_to_wide(dst);
    if (wide_dst == NULL) {
        free(wide_src);
        return -1;
    }

    while (1) {
        /* Redis persistence renames must remain atomic.  COPY_ALLOWED turns a
         * cross-volume rename into copy+delete and can expose a partial RDB,
         * AOF, manifest, or rewritten configuration after a crash. */
        if (MoveFileExW(wide_src, wide_dst,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            free(wide_src);
            free(wide_dst);
            return 0;
        }

        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED &&
            error != ERROR_SHARING_VIOLATION &&
            error != ERROR_LOCK_VIOLATION) {
            break;
        }
        if (--retries == 0) break;
        Sleep(10);
    }

    errno = translate_sys_error((int)error);
    if (errno == -9999) errno = EIO;
    free(wide_src);
    free(wide_dst);
    return -1;
}

int replace_link(const char *src, const char *dst) {
    wchar_t *wide_src = win32_utf8_path_to_wide(src);
    wchar_t *wide_dst;

    if (wide_src == NULL) return -1;
    wide_dst = win32_utf8_path_to_wide(dst);
    if (wide_dst == NULL) {
        free(wide_src);
        return -1;
    }

    if (CreateHardLinkW(wide_dst, wide_src, NULL)) {
        free(wide_src);
        free(wide_dst);
        return 0;
    }

    errno = translate_sys_error((int)GetLastError());
    if (errno == -9999) errno = EIO;
    free(wide_src);
    free(wide_dst);
    return -1;
}

FILE *replace_fopen(const char *path, const char *mode) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    wchar_t *wide_mode;
    FILE *file;
    int saved_errno;

    if (wide_path == NULL) return NULL;
    wide_mode = win32_utf8_to_wide(mode);
    if (wide_mode == NULL) {
        free(wide_path);
        return NULL;
    }
    file = _wfopen(wide_path, wide_mode);
    saved_errno = errno;
    free(wide_mode);
    free(wide_path);
    errno = saved_errno;
    return file;
}

FILE *replace_freopen(const char *path, const char *mode, FILE *stream) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    wchar_t *wide_mode;
    FILE *file;
    int saved_errno;

    if (wide_path == NULL) return NULL;
    wide_mode = win32_utf8_to_wide(mode);
    if (wide_mode == NULL) {
        free(wide_path);
        return NULL;
    }
    file = _wfreopen(wide_path, wide_mode, stream);
    saved_errno = errno;
    free(wide_mode);
    free(wide_path);
    errno = saved_errno;
    return file;
}

FILE *replace_popen(const char *command, const char *mode) {
    wchar_t *wide_command = win32_utf8_to_wide(command);
    wchar_t *wide_mode;
    FILE *file;
    int saved_errno;

    if (wide_command == NULL) return NULL;
    wide_mode = win32_utf8_to_wide(mode);
    if (wide_mode == NULL) {
        free(wide_command);
        return NULL;
    }
    file = _wpopen(wide_command, wide_mode);
    saved_errno = errno;
    free(wide_mode);
    free(wide_command);
    errno = saved_errno;
    return file;
}

int replace_remove(const char *path) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    int result;
    int saved_errno;

    if (wide_path == NULL) return -1;
    result = _wremove(wide_path);
    saved_errno = errno;
    free(wide_path);
    errno = saved_errno;
    return result;
}

int replace_system(const char *command) {
    wchar_t *wide_command;
    int result;
    int saved_errno;

    if (command == NULL) return _wsystem(NULL);
    wide_command = win32_utf8_to_wide(command);
    if (wide_command == NULL) return -1;
    result = _wsystem(wide_command);
    saved_errno = errno;
    free(wide_command);
    errno = saved_errno;
    return result;
}

int replace_unlink(const char *path) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    int result;
    int saved_errno;
    if (wide_path == NULL) return -1;
    result = _wunlink(wide_path);
    saved_errno = errno;
    free(wide_path);
    errno = saved_errno;
    return result;
}

int replace_mkdir(const char *path) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    int result;
    int saved_errno;
    if (wide_path == NULL) return -1;
    result = _wmkdir(wide_path);
    saved_errno = errno;
    free(wide_path);
    errno = saved_errno;
    return result;
}

int replace_rmdir(const char *path) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    int result;
    int saved_errno;
    if (wide_path == NULL) return -1;
    result = _wrmdir(wide_path);
    saved_errno = errno;
    free(wide_path);
    errno = saved_errno;
    return result;
}

int replace_chmod(const char *path, int mode) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    int result;
    int saved_errno;
    if (wide_path == NULL) return -1;
    result = _wchmod(wide_path, mode);
    saved_errno = errno;
    free(wide_path);
    errno = saved_errno;
    return result;
}

static __time64_t filetime_to_unix_seconds(FILETIME value) {
    const ULONGLONG windows_epoch = 116444736000000000ULL;
    ULARGE_INTEGER ticks;
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    if (ticks.QuadPart <= windows_epoch) return 0;
    return (__time64_t)((ticks.QuadPart - windows_epoch) / 10000000ULL);
}

int replace_stat64(const char *path, struct __stat64 *buffer) {
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    int result;
    int saved_errno;
    if (wide_path == NULL) return -1;
    result = _wstat64(wide_path, buffer);
    saved_errno = errno;
    if (result != 0 && wcsncmp(wide_path, L"\\\\?\\", 4) == 0) {
        BY_HANDLE_FILE_INFORMATION info;
        HANDLE handle = CreateFileW(wide_path, FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            if (GetFileInformationByHandle(handle, &info)) {
                memset(buffer, 0, sizeof(*buffer));
                buffer->st_mode =
                    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
                    (_S_IFDIR | _S_IREAD | _S_IEXEC) :
                    (_S_IFREG | _S_IREAD);
                if (!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
                    buffer->st_mode |= _S_IWRITE;
                buffer->st_nlink = (short)(info.nNumberOfLinks > SHRT_MAX ?
                                           SHRT_MAX : info.nNumberOfLinks);
                buffer->st_ino = (_ino_t)(info.nFileIndexLow & 0xffff);
                buffer->st_size =
                    ((__int64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                buffer->st_atime = filetime_to_unix_seconds(info.ftLastAccessTime);
                buffer->st_mtime = filetime_to_unix_seconds(info.ftLastWriteTime);
                buffer->st_ctime = filetime_to_unix_seconds(info.ftCreationTime);
                result = 0;
                saved_errno = 0;
            } else {
                DWORD error = GetLastError();
                saved_errno = translate_sys_error((int)error);
                if (saved_errno == -9999) saved_errno = EIO;
            }
            CloseHandle(handle);
        } else {
            saved_errno = translate_sys_error((int)GetLastError());
            if (saved_errno == -9999) saved_errno = EIO;
        }
    }
    free(wide_path);
    errno = saved_errno;
    return result;
}

int truncate(const char *path, PORT_LONGLONG length) {
    LARGE_INTEGER newSize;
    wchar_t *wide_path = win32_utf8_path_to_wide(path);
    HANDLE toTruncate;

    if (wide_path == NULL) return -1;
    toTruncate = CreateFileW(wide_path,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_WRITE | FILE_SHARE_READ,
                                    NULL,
                                    OPEN_EXISTING,
                                    0,
                                    NULL);
    free(wide_path);
    if (toTruncate != INVALID_HANDLE_VALUE) {
        int result = 0;
        newSize.QuadPart = length;
        if (FALSE == (SetFilePointerEx(toTruncate, newSize, NULL, FILE_BEGIN)
                      && SetEndOfFile(toTruncate))) {
            set_errno_from_last_error();
            result = -1;
        }
        CloseHandle(toTruncate);
        return result;
    } else {
        set_errno_from_last_error();
        return -1;
    }
}
