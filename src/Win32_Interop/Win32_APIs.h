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

#ifndef WIN32_INTEROP_APIS_H
#define WIN32_INTEROP_APIS_H

#include "Win32_types.h"
#include <Windows.h>
#include <limits.h>
#include <stdio.h>      // for rename
#include <sys/stat.h>

// API replacement for non-fd stdio functions
#define fseeko      _fseeki64
#define ftello      _ftelli64
#ifndef __MINGW32__
#define snprintf    _snprintf
#endif
#define strcasecmp  _stricmp
#define strtoll     _strtoi64

#ifdef _WIN64
#define strtol      _strtoi64
#define strtoul     _strtoui64
#endif

/* Keep long delays out of the 32-bit Sleep() argument.  Sleep(INFINITE)
 * never returns, so the implementation also chunks MAXDWORD-sized waits. */
#ifdef __cplusplus
extern "C" {
#endif
unsigned int win32_sleep(PORT_LONGLONG seconds);
int win32_usleep(PORT_LONGLONG usec);
#ifdef __cplusplus
}
#endif
#define sleep(x) win32_sleep((PORT_LONGLONG)(x))
#undef usleep
#define usleep(x) win32_usleep((PORT_LONGLONG)(x))


/* following defined to choose little endian byte order */
#define __i386__ 1
#if !defined(va_copy)
#define va_copy(d,s)  d = (s)
#endif

#ifndef __RTL_GENRANDOM
#define __RTL_GENRANDOM 1
typedef BOOLEAN(WINAPI *RtlGenRandomFunc)(void *RandomBuffer,
                                          ULONG RandomBufferLength);
#endif

#ifdef __cplusplus
extern "C" {
#endif

int win32_secure_random_bytes(void *buffer, size_t length);

#define random()    replace_random()
#define rand()      replace_random()
#ifdef RAND_MAX
#undef RAND_MAX
#endif
#define RAND_MAX INT_MAX
#define srandom     srand
int replace_random();

#define rename(a,b) replace_rename(a,b)
int replace_rename(const char *src, const char *dest);

/* Create a hard link using the Win32 path API while preserving the POSIX
 * link(oldpath, newpath) argument order expected by Redis. */
int replace_link(const char *src, const char *dest);

/* UTF-8 path wrappers for CRT interfaces which otherwise use the active
 * Windows code page. */
FILE *replace_fopen(const char *path, const char *mode);
FILE *replace_freopen(const char *path, const char *mode, FILE *stream);
FILE *replace_popen(const char *command, const char *mode);
int replace_remove(const char *path);
int replace_system(const char *command);
int replace_unlink(const char *path);
int replace_mkdir(const char *path);
int replace_rmdir(const char *path);
int replace_chmod(const char *path, int mode);
int replace_stat64(const char *path, struct __stat64 *buffer);

int truncate(const char *path, PORT_LONGLONG length);

#ifdef __cplusplus
}
#endif

#define lseek lseek64

#endif
