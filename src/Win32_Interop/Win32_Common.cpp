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
#include "Win32_Common.h"
#include <stdint.h>

namespace Globals
{
    size_t pageSize = 0;
}

/* This function is used to force the VEH on the entire size of the buffer length,
 * in the event that the buffer crosses the memory page boundaries */
void EnsureMemoryIsMapped(const void *buffer, size_t size) {
    //[tporadowski/#62] when this is called from main process - Globals::pageSize is not initialized,
    //                  so prevent EXCEPTION_INT_DIVIDE_BY_ZERO
    if (buffer == NULL || size == 0 || Globals::pageSize == 0) {
        return;
    }

    uintptr_t firstByte = (uintptr_t)buffer;
    if (size - 1 > UINTPTR_MAX - firstByte) return;
    uintptr_t lastByte = firstByte + size - 1;
    uintptr_t firstPage = firstByte - (firstByte % Globals::pageSize);
    uintptr_t lastPage = lastByte - (lastByte % Globals::pageSize);

    // Use 'volatile' to make sure the compiler doesn't remove the memory access
    for (uintptr_t page = firstPage;; page += Globals::pageSize) {
        volatile char value = *(volatile char *)page;
        (void)value;
        if (page == lastPage || UINTPTR_MAX - page < Globals::pageSize) break;
    }
}

bool IsWindowsVersionAtLeast(WORD wMajorVersion, WORD wMinorVersion, WORD wServicePackMajor) {
    OSVERSIONINFOEXW osvi = {};
    DWORDLONG const dwlConditionMask = VerSetConditionMask(
        VerSetConditionMask(
        VerSetConditionMask(
        0, VER_MAJORVERSION, VER_GREATER_EQUAL),
        VER_MINORVERSION, VER_GREATER_EQUAL),
        VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    osvi.dwMajorVersion = wMajorVersion;
    osvi.dwMinorVersion = wMinorVersion;
    osvi.wServicePackMajor = wServicePackMajor;

    return VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, dwlConditionMask) != FALSE;
}
