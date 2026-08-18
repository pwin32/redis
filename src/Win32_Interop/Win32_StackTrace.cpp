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

#include "Win32_StackTrace.h"
#include "Win32_RedisLog.h"
#include "Win32_Error.h"
#include <DbgHelp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

/* Symbol names are not filesystem paths and are not subject to MAX_PATH.
 * Keep a fixed crash-safe buffer large enough for DbgHelp's documented symbol
 * name limit so the exception handler never depends on a heap allocation. */
static unsigned char symbolBuffer[sizeof(IMAGEHLP_SYMBOL64) + MAX_SYM_NAME];
static IMAGEHLP_SYMBOL64* pSymbol = (IMAGEHLP_SYMBOL64*)symbolBuffer;
static IMAGEHLP_LINE64 line;
static volatile LONG processingException = 0;
static LPTOP_LEVEL_EXCEPTION_FILTER defaultTopLevelExceptionHandler = NULL;

static const char* exceptionDescription(const DWORD& code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        default: return "UNKNOWN EXCEPTION";
    }
}

/* Returns the index of the last backslash in the file path */
const char *GetFilenameStart(const char* path) {
    const char *backslash;
    const char *slash;
    if (path == NULL) return "(unknown module)";
    backslash = strrchr(path, '\\');
    slash = strrchr(path, '/');
    if (slash != NULL && (backslash == NULL || slash > backslash))
        backslash = slash;
    return backslash == NULL ? path : backslash + 1;
}

#ifdef _WIN64
void LogStackTrace() {
    BOOL            result;
    HANDLE          thread;
    HANDLE          process;
    CONTEXT         context;
    STACKFRAME64    stack;
    DWORD64         dw64Displacement;
    DWORD           dwDisplacement;

    memset(&stack, 0, sizeof(STACKFRAME64));
    RtlCaptureContext(&context);
    process = GetCurrentProcess();
    thread = GetCurrentThread();
    dw64Displacement = 0;
    stack.AddrPC.Offset = context.Rip;
    stack.AddrPC.Mode = AddrModeFlat;
    stack.AddrStack.Offset = context.Rsp;
    stack.AddrStack.Mode = AddrModeFlat;
    stack.AddrFrame.Offset = context.Rbp;
    stack.AddrFrame.Mode = AddrModeFlat;

    for (;;) {
        result = StackWalk64(
            IMAGE_FILE_MACHINE_AMD64,
            process,
            thread,
            &stack,
            &context,
            NULL,
            SymFunctionTableAccess64,
            SymGetModuleBase64,
            NULL
            );

        if (!result) break;

        memset(symbolBuffer, 0, sizeof(symbolBuffer));
        memset(&line, 0, sizeof(line));
        pSymbol->MaxNameLength = MAX_SYM_NAME;
        pSymbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

        SymGetSymFromAddr64(process, stack.AddrPC.Offset, &dw64Displacement, pSymbol);
        SymGetLineFromAddr64(process, stack.AddrPC.Offset, &dwDisplacement, &line);

        DWORD64 moduleBase = SymGetModuleBase64(process, stack.AddrPC.Offset);
        char *modulePath = moduleBase ?
            win32_get_module_filename_for_handle_utf8((void *)(uintptr_t)moduleBase) :
            NULL;

        //serverLog(LL_WARNING | LL_RAW, "%s!%s(%s:%d)(0x%08LX, 0x%08LX, 0x%08LX, 0x%08LX)\n",
        serverLog(LL_WARNING | LL_RAW, "%s!%s(%s:%d)(0x%08I64X, 0x%08I64X, 0x%08I64X, 0x%08I64X)\n",
            GetFilenameStart(modulePath),
            pSymbol->Name,
            line.FileName != NULL ? line.FileName : "(unknown source)",
            line.LineNumber,
            stack.Params[0],
            stack.Params[1],
            stack.Params[2],
            stack.Params[3]
            );
        win32_free(modulePath);
    }
}
#else
void LogStackTrace() {}
#endif

void StackTraceInfo() {
    serverLog(LL_WARNING, "--- STACK TRACE");
    LogStackTrace();
}

void ServerInfo() {
    /* Use the core crash-report path so its temporary SDS values are freed and
     * the client-list diagnostics are included consistently. */
    logServerInfo();
}

void BugReportEnd(){
    serverLogRaw(LL_WARNING,
        "\n=== REDIS BUG REPORT END. Make sure to include from START to END. ===\n\n"
        "       Please report this bug by following the instructions at:\n\n"
        "     https://github.com/pwin32/redis/issues\n\n"
        "    Suspect RAM error? Use redis-server --test-memory to verify it.\n\n"
        );
}

LONG WINAPI UnhandledExceptiontHandler(PEXCEPTION_POINTERS info) {
    if (InterlockedCompareExchange(&processingException, 1, 0) == 0) {
        bool headerLogged = false;
        try {
            const char* exDescription = "Exception code not available";
            if (info != NULL && info->ExceptionRecord != NULL &&
                info->ExceptionRecord->ExceptionCode != 0) {
                exDescription = exceptionDescription(info->ExceptionRecord->ExceptionCode);
            }

            // Call antirez routine to log the start of the bug report
            bugReportStart();
            headerLogged = true;
            serverLog(LL_WARNING, "--- %s", exDescription);
            if (info != NULL && info->ExceptionRecord != NULL) {
                ULONG_PTR faultAddress = 0;
                if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                    info->ExceptionRecord->NumberParameters >= 2) {
                    faultAddress = info->ExceptionRecord->ExceptionInformation[1];
                }
                serverLog(LL_WARNING,
                    "--- exception context: code=0x%08x address=%p fault=%p",
                    info->ExceptionRecord->ExceptionCode,
                    info->ExceptionRecord->ExceptionAddress,
                    (void*) faultAddress);
            }
            StackTraceInfo();
            ServerInfo();
        }
        catch (...) {}

        if (headerLogged) {
            BugReportEnd();
        }

        if (defaultTopLevelExceptionHandler != NULL && info != NULL) {
            defaultTopLevelExceptionHandler(info);
        }

        InterlockedExchange(&processingException, 0);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* Handler to trap abort() calls */
extern "C" void AbortHandler(int signal_number) {
    bugReportStart();
    serverLog(LL_WARNING, "--- ABORT");
    StackTraceInfo();
    BugReportEnd();
}

void InitSymbols() {
    // Preload symbols so they will be available in case of out-of-memory exception
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);
}

void StackTraceInit(void) {
    InitSymbols();
    // Global handler for unhandled exceptions
    defaultTopLevelExceptionHandler = SetUnhandledExceptionFilter(UnhandledExceptiontHandler);
    // Handler for abort()
    signal(SIGABRT, &AbortHandler);
}
