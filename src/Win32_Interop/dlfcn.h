/*
 * Native Windows dynamic-loader adapter for Redis for Windows.
 *
 * Copyright (c) 2026 Redis for Windows contributors
 * All rights reserved.
 *
 * Redistribution and use are governed by the BSD-3-Clause notice in
 * WINDOWS-NOTICES.txt.
 */

#ifndef REDIS_WIN32_DLFCN_H
#define REDIS_WIN32_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Redis modules only require eager, process-local loading on Windows. */
#define RTLD_LAZY   0
#define RTLD_NOW    0
#define RTLD_GLOBAL (1 << 1)
#define RTLD_LOCAL  (1 << 2)

void *dlopen(const char *file, int mode);
int dlclose(void *handle);
void *dlsym(void *handle, const char *name);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_WIN32_DLFCN_H */
