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

#include "Win32_types.h"
#include "Win32_RedisLog.h"
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <process.h>
#include <time.h>
#include "Win32Fixes.h"
#include "Win32_Error.h"
#include "Win32_EventLog.h"
#include "Win32_Time.h"
#include "Win32_ANSI.h"
#include <assert.h>

static const char ellipsis[] = "[...]";
static const char ellipsisWithNewLine[] = "[...]\n";
static int verbosity = LL_WARNING;
static HANDLE hLogFile = INVALID_HANDLE_VALUE;
static int isStdout = 0;
static char* logFilename = NULL;

void setLogVerbosityLevel(int level)
{
    verbosity = level;
}

const char* getLogFilename() {
    if (logFilename == NULL || logFilename[0] == '\0') {
        return "stdout";
    } else {
        return logFilename;
    }
}

/* We keep the file handle open to improve performance.
 * This assumes that calls to serverLog and setLogFile will not happen
 * concurrently.
 */
void setLogFile(const char* filename)
{
    const char *requested = filename == NULL ? "" : filename;
    size_t requestedLength = strlen(requested);
    char *newLogFilename = (char*) malloc(requestedLength + 1);
    HANDLE newLogFile;
    int newIsStdout;

    if (newLogFilename == NULL) {
        serverLog(LL_WARNING, "memory allocation failure");
        return;
    }
    memcpy(newLogFilename, requested, requestedLength + 1);

    if (requested[0] == '\0' || _stricmp(requested, "stdout") == 0) {
        newLogFile = GetStdHandle(STD_OUTPUT_HANDLE);
        newIsStdout = 1;
    } else {
        wchar_t *widePath = win32_utf8_path_to_wide(requested);
        if (widePath == NULL) {
            fprintf(stderr, "Could not convert logfile path from UTF-8: %s\n",
                    requested);
            free(newLogFilename);
            return;
        }

        /* Passing FILE_APPEND_DATA without FILE_WRITE_DATA is essential for
         * getting atomic appends across processes. */
        newLogFile = CreateFileW(widePath,
                                 FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL,
                                 OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);
        win32_free(widePath);

        if (newLogFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            fprintf(stderr, "Could not open logfile %s: Windows error %lu\n",
                    requested, (unsigned long)err);
            free(newLogFilename);
            return;
        }
        newIsStdout = 0;
    }

    if (hLogFile != INVALID_HANDLE_VALUE && !isStdout) CloseHandle(hLogFile);
    free(logFilename);
    logFilename = newLogFilename;
    hLogFile = newLogFile;
    isStdout = newIsStdout;
}
    
void serverLogRaw(int level, const char *msg) {
    const char *c = ".-*#";
    DWORD dwBytesWritten;
    /* The complete message needs to be passed to WriteFile at once, to ensure
     * atomicity of log entries across processes.
     * So we format the complete message into a buffer first.
     * Any output that doesn't fit the size of this buffer will be truncated.
     */
    char buf[LOG_MAX_LEN];
    const char *completeMessage;
    DWORD completeMessageLength;
    int rawmode = (level & LL_RAW);

    level &= 0xff; /* clear flags */
    if (level < verbosity) return;

    if (hLogFile == INVALID_HANDLE_VALUE) return;

    if (rawmode) {
        size_t rawLength = strlen(msg);
        completeMessage = msg;
        completeMessageLength = rawLength > (size_t)MAXDWORD ?
                                MAXDWORD : (DWORD)rawLength;
    } else {
        int vlen;
        size_t off = 0;
        time_t secs;
        unsigned int usecs;
        struct tm * now ;

        completeMessage = buf; 
        secs = gettimeofdaysecs(&usecs);
        now = localtime(&secs);
        vlen = snprintf(buf + off, sizeof(buf) - off, "[%d] ", (int)_getpid());
        if (vlen < 0 || (size_t)vlen >= sizeof(buf) - off) goto truncated;
        off += (size_t)vlen;
        size_t time_len = strftime(buf + off, sizeof(buf) - off,
                                   "%d %b %H:%M:%S.", now);
        if (time_len == 0) goto truncated;
        off += time_len;
        vlen = snprintf(buf + off, sizeof(buf) - off, "%03d %c ", usecs / 1000, c[level]);
        if (vlen < 0 || (size_t)vlen >= sizeof(buf) - off) goto truncated;
        off += (size_t)vlen;
        vlen = snprintf(buf + off, sizeof(buf) - off, "%s\n", msg);
        if (vlen < 0 || (size_t)vlen >= sizeof(buf) - off) goto truncated;
        completeMessageLength = (DWORD)(off + (size_t)vlen);
        goto write_message;

truncated:
        /* Keep the buffer valid even when MinGW's C99 snprintf returns the
         * required length rather than the MS CRT's historical -1. */
        memcpy(buf + sizeof(buf)-sizeof(ellipsisWithNewLine),
               ellipsisWithNewLine, sizeof(ellipsisWithNewLine));
        completeMessageLength = sizeof(buf)-1;
    }

write_message:
    if (isStdout) {
        DWORD consoleMode;
        if (GetConsoleMode(hLogFile, &consoleMode)) {
            ParseAndPrintANSIString(hLogFile, completeMessage,
                                    completeMessageLength, &dwBytesWritten);
        } else {
            WriteFile(hLogFile, completeMessage, completeMessageLength,
                      &dwBytesWritten, NULL);
        }
    } else {
        WriteFile(hLogFile, completeMessage, completeMessageLength,
                  &dwBytesWritten, NULL);
    }

    /* FlushFileBuffers() ensures that all data and metadata is written to disk, but it's effect
     * on performance is severe.
     */
#ifdef FLUSH_LOG_WRITES
    FlushFileBuffers(hLogFile);
#endif

    if (IsEventLogEnabled() == 1) {
        WriteEventLog(msg);
    }
}

static void serverLogV(int level, const char *fmt, va_list ap) {
    char msg[LOG_MAX_LEN];
    int vlen;

    if ((level&0xff) < verbosity) return;

    vlen = vsnprintf(msg, sizeof(msg), fmt, ap);

    /* The MS CRT implementation of vsnprintf/snprintf returns -1 if the formatted output doesn't fit the buffer,
     * in addition to when an encoding error occurs. Proceeding with a zero-terminated ellipsis at the end of the
     * buffer seems a better option than not logging this message at all.
     */
    if (vlen < 0 || (size_t)vlen >= sizeof(msg)) {
        memcpy(msg + sizeof(msg) - sizeof(ellipsis), ellipsis, sizeof(ellipsis));
    }

    serverLogRaw(level,msg);
}

/* Keep the legacy Windows entry point for the interop layer. */
void serverLog(int level, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    serverLogV(level, fmt, ap);
    va_end(ap);
}

/* Redis 6.2 core uses this entry point behind the serverLog macro. */
void _serverLog(int level, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    serverLogV(level, fmt, ap);
    va_end(ap);
}

/* Keep the 7.4 variadic handler contract. Windows does not use the POSIX
 * async-signal-safe implementation, so route the formatted message through
 * the native logger just as the ordinary serverLog entry point does. */
void serverLogFromHandler(int level, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    serverLogV(level, fmt, ap);
    va_end(ap);
}
