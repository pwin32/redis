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

#pragma once

#include <Windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    extern BOOL g_IsForkedProcess;

    typedef enum operationType {
        otINVALID = 0,
        otRDB = 1,
        otAOF = 2,
        otSocket = 3
    } OperationType;

    typedef enum operationStatus {
        osUNSTARTED = 0,
        osINPROGRESS = 1,
        osCOMPLETE = 2,
        osFAILED = 3
    } OperationStatus;

    typedef enum startupStatus {
        ssFAILED = 0,                 // Something went wrong, exit program with error.
        ssCONTINUE_AS_PARENT = 1,     // Parent qfork initialization complete, continue as parent instance. Call QForkShutdown when exiting.
        ssCHILD_EXIT = 2              // Child completed operation. Call QForkShutdown and exit.
    } StartupStatus;

    /* Module subsystem globals that live outside the QFork heap. The pointed
     * objects themselves are allocated in the mapped Redis heap; copying this
     * POD reconnects the fresh persistence process to that snapshot after its
     * module DLL images have been restored. */
    typedef struct redisModuleForkData {
        LPVOID modules;
        LPVOID authCallbacks;
        LPVOID unblockedClients;
        LPVOID tempClients;
        size_t tempClientCap;
        size_t tempClientCount;
        size_t tempClientMinCount;
        LPVOID keyspaceSubscribers;
        LPVOID postExecUnitJobs;
        LPVOID commandFilters;
        LPVOID eventListeners;
        LPVOID eventLoopOneShots;
        LPVOID timers;
        long long aeTimer;
        LPVOID clusterReceivers[UINT8_MAX];
        /* Redis Functions metadata roots also live in process-static storage
         * rather than redisServer. The library/source metadata is mapped and
         * needed by RDB/AOF serialization; the embedded Lua engine state is
         * CRT-owned and remains unavailable in the fresh QFork child. */
        LPVOID functionsEngines;
        LPVOID functionsLibCtx;
        size_t functionsEngineCacheMemory;
    } RedisModuleForkData;

    /* ACL subsystem roots that live in process-static storage. The pointed
     * objects are allocated in the mapped Redis heap, so the fresh QFork
     * persistence process must reconnect these roots to the parent snapshot. */
    typedef struct redisACLForkData {
        LPVOID users;
        LPVOID defaultUser;
        LPVOID usersToLoad;
        LPVOID aclLog;
        long long aclLogEntryCount;
        LPVOID commandId;
        unsigned long nextid;
    } RedisACLForkData;

    /* Core roots that are initialized outside redisServer and point into the
     * mapped Redis heap. Process-local runtimes such as Lua are deliberately
     * excluded and remain unavailable in the disposable persistence child. */
    typedef struct redisCoreForkData {
        LPVOID configs;
        LPVOID asmManager;
    } RedisCoreForkData;

    /* Keep the QFork control block bounded while allowing the complete 7.2
     * shared-object table to be copied by value. */
#define REDIS_QFORK_MAX_SHARED_DATA_SIZE (128 * 1024)

    void ACLGetForkData(RedisACLForkData *data);
    void ACLSetForkData(const RedisACLForkData *data);

    /* Validate that a native module image can be restored safely enough for
     * callbacks executed inside the disposable QFork persistence child. */
    BOOL QForkValidateModuleImage(
        void *handle,
        const wchar_t *path,
        const char *name);

    // For parent process use only
    pid_t BeginForkOperation_Rdb(
        int req,
        char* fileName,
        const void *rdbSaveInfo,
        size_t rdbSaveInfoSize,
        int rdbFlags,
        LPVOID redisData,
        int sizeOfRedisData,
        uint8_t *dictHashSeed,
        LPVOID modules);

    pid_t BeginForkOperation_Aof(
        char* fileName,
        LPVOID redisData,
        int sizeOfRedisData,
        uint8_t *dictHashSeed,
        LPVOID modules);

    pid_t BeginForkOperation_Socket(
        int req,
        const void *rdbSaveInfo,
        size_t rdbSaveInfoSize,
        int rdb_pipe_write_fd,
        int safe_to_exit_pipe_fd,
        LPVOID redisData,
        int sizeOfRedisData,
        uint8_t *dictHashSeed,
        LPVOID modules);

    void BeginForkOperation_Socket_Duplicate(DWORD dwProcessId);

    OperationStatus GetForkOperationStatus();
    BOOL EndForkOperation(int * pExitCode);
    BOOL AbortForkOperation();

    /* Bind the current parent thread to the post-startup jemalloc arena whose
     * extents are backed by the QFork heap. This is a no-op before QFork is
     * ready and in child/tool/non-persistent processes. Returns zero on
     * success or an errno-style jemalloc error code. */
    int QForkEnsureCurrentThreadJemallocArena(void);

    /* A QFork child maps the parent's Redis heap with copy-on-write, but it
     * starts with a fresh jemalloc metadata tree.  Inherited mapped blocks
     * therefore must not be passed to jemalloc free/usable-size routines in
     * the disposable child. */
    BOOL QForkIsInheritedHeapAddress(const void *ptr);

    LPVOID AllocHeapBlock(LPVOID addr, size_t size, BOOL zero);
    BOOL PurgePages(LPVOID addr, size_t length);
    BOOL FreeHeapBlock(LPVOID addr, size_t size);

#ifndef NO_QFORKIMPL
#ifdef QFORK_MAIN_IMPL
    int redis_main(int argc, char** argv);
#else
#define main redis_main
#endif
#endif

#ifdef __cplusplus
}
#endif
