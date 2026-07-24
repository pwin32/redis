/*
 * Native Windows dynamic-loader adapter for Redis for Windows.
 *
 * Copyright (c) 2026 Redis for Windows contributors
 * All rights reserved.
 *
 * Redistribution and use are governed by the BSD-3-Clause notice in
 * WINDOWS-NOTICES.txt.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlfcn.h"

/* POSIX does not require dlerror() to be thread-safe. Redis calls these
 * wrappers from the main thread while loading and unloading modules, so a
 * single process-local buffer is sufficient and avoids introducing TLS into
 * the Windows/QFork loader path. */
static char error_buffer[2048];
static int error_pending;

static void clear_error(void) {
    error_buffer[0] = '\0';
    error_pending = 0;
}

static void save_error(const char *operation, const char *target, DWORD code) {
    char system_message[1024];
    DWORD length;

    system_message[0] = '\0';
    length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                                FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL, code,
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                            system_message, (DWORD)sizeof(system_message),
                            NULL);
    while (length > 0 &&
           (system_message[length - 1] == '\r' ||
            system_message[length - 1] == '\n'))
    {
        system_message[--length] = '\0';
    }

    if (target == NULL) target = "(null)";
    if (length == 0) {
        snprintf(error_buffer, sizeof(error_buffer),
                 "%s %s failed with Windows error %lu",
                 operation, target, (unsigned long)code);
    } else {
        snprintf(error_buffer, sizeof(error_buffer),
                 "%s %s failed: %s (Windows error %lu)",
                 operation, target, system_message, (unsigned long)code);
    }
    error_buffer[sizeof(error_buffer) - 1] = '\0';
    error_pending = 1;
}

static HMODULE load_library_utf8(const char *file) {
    HMODULE module;
    wchar_t *wide_path = NULL;
    int wide_length;
    int i;

    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      file, -1, NULL, 0);
    if (wide_length > 0) {
        wide_path = (wchar_t *)malloc((size_t)wide_length *
                                      sizeof(*wide_path));
        if (wide_path == NULL) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file, -1,
                                wide_path, wide_length) == 0)
        {
            DWORD code = GetLastError();
            free(wide_path);
            SetLastError(code);
            return NULL;
        }
        for (i = 0; wide_path[i] != L'\0'; i++) {
            if (wide_path[i] == L'/') wide_path[i] = L'\\';
        }
        module = LoadLibraryExW(wide_path, NULL,
                                LOAD_WITH_ALTERED_SEARCH_PATH);
        DWORD code = module == NULL ? GetLastError() : ERROR_SUCCESS;
        free(wide_path);
        if (module == NULL) SetLastError(code);
        return module;
    }

    /* Preserve compatibility with older configurations whose module path is
     * expressed in the active Windows code page rather than UTF-8. */
    return LoadLibraryExA(file, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void *dlopen(const char *file, int mode) {
    HMODULE module;
    UINT previous_error_mode;

    (void)mode;
    clear_error();
    if (file == NULL || file[0] == '\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        save_error("LoadLibrary", file, ERROR_INVALID_PARAMETER);
        return NULL;
    }

    previous_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    module = load_library_utf8(file);
    if (module == NULL) {
        DWORD code = GetLastError();
        SetErrorMode(previous_error_mode);
        save_error("LoadLibrary", file, code);
        return NULL;
    }
    SetErrorMode(previous_error_mode);
    return (void *)module;
}

int dlclose(void *handle) {
    clear_error();
    if (handle == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        save_error("FreeLibrary", NULL, ERROR_INVALID_HANDLE);
        return -1;
    }
    if (!FreeLibrary((HMODULE)handle)) {
        DWORD code = GetLastError();
        save_error("FreeLibrary", NULL, code);
        return -1;
    }
    return 0;
}

void *dlsym(void *handle, const char *name) {
    FARPROC symbol;

    clear_error();
    if (handle == NULL || name == NULL || name[0] == '\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        save_error("GetProcAddress", name, ERROR_INVALID_PARAMETER);
        return NULL;
    }

    symbol = GetProcAddress((HMODULE)handle, name);
    if (symbol == NULL) {
        DWORD code = GetLastError();
        save_error("GetProcAddress", name, code);
        return NULL;
    }

    return (void *)(uintptr_t)symbol;
}

char *dlerror(void) {
    if (!error_pending) return NULL;
    error_pending = 0;
    return error_buffer;
}
