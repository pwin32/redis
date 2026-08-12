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
#include "Win32_APIs.h"
#include "Win32_Error.h"

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
    const char *system_message = win32_system_strerror((int)code);

    if (target == NULL) target = "(null)";
    if (system_message == NULL || system_message[0] == '\0') {
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
    wchar_t *wide_path = win32_utf8_path_to_wide(file);
    DWORD code;

    if (wide_path == NULL) return NULL;
    module = LoadLibraryExW(wide_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    code = module == NULL ? GetLastError() : ERROR_SUCCESS;
    win32_free(wide_path);
    if (module == NULL) SetLastError(code);
    return module;
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
    void *result = NULL;

    clear_error();
    if (handle == NULL || name == NULL || name[0] == '\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        save_error("GetProcAddress", name, ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (win32_get_proc_address((HMODULE)handle, name, &result,
                               sizeof(result)) != 0) {
        DWORD code = GetLastError();
        save_error("GetProcAddress", name, code);
        return NULL;
    }
    return result;
}

char *dlerror(void) {
    if (!error_pending) return NULL;
    error_pending = 0;
    return error_buffer;
}
