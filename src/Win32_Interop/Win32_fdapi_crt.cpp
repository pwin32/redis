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

#include "Win32_fdapi_crt.h"
#include "Win32_Common.h"
#include "Win32_Error.h"
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int crt_pipe(int *pfds, unsigned int psize, int textmode) {
    return _pipe(pfds, psize, textmode);
}

int crt_close(int fd) {
    return _close(fd);
}

int crt_read(int fd, void *buffer, unsigned int count) {
    return _read(fd, buffer, count);
}

int crt_write(int fd, const void *buffer, unsigned int count) {
    return _write(fd, buffer, count);
}

int crt_open(const char *filename, int oflag, int pmode) {
    wchar_t *wide_filename = win32_utf8_path_to_wide(filename);
    int result;
    int saved_errno;
    if (wide_filename == NULL) return -1;
    result = _wopen(wide_filename, oflag, pmode);
    saved_errno = errno;
    free(wide_filename);
    errno = saved_errno;
    return result;
}

int crt_mkstemp(char *filename_template) {
    static volatile LONG sequence;
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t length;
    char *suffix;
    uint64_t state;

    if (filename_template == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(filename_template);
    if (length < 6) {
        errno = EINVAL;
        return -1;
    }
    suffix = filename_template + length - 6;
    if (memcmp(suffix, "XXXXXX", 6) != 0) {
        errno = EINVAL;
        return -1;
    }

    state = ((uint64_t)GetTickCount64() << 32) ^
            ((uint64_t)GetCurrentProcessId() << 16) ^
            (uint64_t)GetCurrentThreadId() ^
            (uint64_t)(unsigned long)InterlockedIncrement(&sequence);
    for (int attempt = 0; attempt < 256; attempt++) {
        for (int index = 0; index < 6; index++) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            suffix[index] = alphabet[state % (sizeof(alphabet) - 1)];
        }
        int fd = crt_open(filename_template,
                          _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY |
                              _O_NOINHERIT,
                          _S_IREAD | _S_IWRITE);
        if (fd != -1) return fd;
        if (errno != EEXIST) return -1;
        state += (uint64_t)attempt + UINT64_C(0x9e3779b97f4a7c15);
    }
    errno = EEXIST;
    return -1;
}

int crt_open_osfhandle(intptr_t osfhandle, int flags) {
    return _open_osfhandle(osfhandle, flags);
}

intptr_t crt_get_osfhandle(int fd) {
    return _get_osfhandle(fd);
}

int crt_setmode(int fd, int mode) {
    return ::_setmode(fd, mode);
}

size_t crt_fwrite(const void *buffer, size_t size, size_t count, FILE *file) {
    // fwrite() somehow locks its view of the buffer. If during a fork operation the buffer has not been loaded into the forkee's process space,
    // the VEH will be called to load the missing pages. Although the page gets loaded, fwrite() will not see the loaded page. The result is
    // that fwrite will fail with errno set to ERROR_INVALID_USER_BUFFER. The fix is to force the buffer into memory before fwrite(). This only
    // impacts writes that straddle page boundaries.
    if (size != 0 && count > SIZE_MAX / size) {
        errno = EINVAL;
        return 0;
    }
    EnsureMemoryIsMapped(buffer, size * count);
    return ::fwrite(buffer, size, count, file);
}

int crt_fclose(FILE* file) {
    return ::fclose(file);
}

int crt_fileno(FILE* file) {
    return ::_fileno(file);
}

int crt_isatty(int fd) {
    return _isatty(fd);
}

int crt_access(const char *pathname, int mode) {
    wchar_t *wide_pathname = win32_utf8_path_to_wide(pathname);
    int result;
    int saved_errno;
    if (wide_pathname == NULL) return -1;
    result = _waccess(wide_pathname, mode);
    saved_errno = errno;
    free(wide_pathname);
    errno = saved_errno;
    return result;
}

__int64 crt_lseek64(int fd, __int64 offset, int origin) {
    return _lseeki64(fd, offset, origin);
}
