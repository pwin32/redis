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


#ifndef WIN32_INTEROPA_ANSI_H
#define WIN32_INTEROPA_ANSI_H

#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

    BOOL ParseAndPrintANSIString(HANDLE hDev, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten);
#if defined(__GNUC__) || defined(__clang__)
#define WIN32_ANSI_PRINTF_ATTR(format_index, first_arg) \
    __attribute__((format(gnu_printf, format_index, first_arg)))
#else
#define WIN32_ANSI_PRINTF_ATTR(format_index, first_arg)
#endif

    int ANSI_printf(const char *format, ...) WIN32_ANSI_PRINTF_ATTR(1, 2);
    int ANSI_vprintf(const char *format, va_list args);
    int ANSI_fprintf(FILE *stream, const char *format, ...) WIN32_ANSI_PRINTF_ATTR(2, 3);
    int ANSI_vfprintf(FILE *stream, const char *format, va_list args);
    int ANSI_fputs(const char *string, FILE *stream);

    // include this file after stdio.h in order to redirect printf to the one that supports ANSI escape sequences
#ifndef WIN32_ANSI_NO_STDIO_REDIRECT
#define printf(...) ANSI_printf(__VA_ARGS__)
#define vprintf(...) ANSI_vprintf(__VA_ARGS__)
#define fprintf(...) ANSI_fprintf(__VA_ARGS__)
#define vfprintf(...) ANSI_vfprintf(__VA_ARGS__)
#define fputs(...) ANSI_fputs(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif

#endif
