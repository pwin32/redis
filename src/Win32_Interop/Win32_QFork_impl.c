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

#include "..\server.h"
#include "..\cluster_asm.h"
#include "Win32_Portability.h"
#include "Win32_QFork.h"
#include "Win32_QFork_impl.h"
#include "Win32_RedisLog.h"

void moduleSetForkData(void *data);
size_t keyMetaForkDataSize(void);
int keyMetaCopyForkData(void *data, size_t size);
int keyMetaSetForkData(const void *data, size_t size);
int rdbSaveRioWithEOFMark(int req, rio *rdb, int *error, rdbSaveInfo *rsi);
int slotSnapshotSaveRio(int req, rio *rdb, int *error);

size_t RedisSharedForkDataSize(void) {
    return sizeof(shared);
}

BOOL RedisCopySharedForkData(void *data, size_t size) {
    if (data == NULL || size != sizeof(shared)) return FALSE;
    memcpy(data, &shared, sizeof(shared));
    return TRUE;
}

BOOL RedisGetCoreForkData(RedisCoreForkData *data) {
    memset(data, 0, sizeof(*data));
    data->configs = configGetQForkData();
    data->asmManager = asmGetQForkState();
    data->keyMetaDataSize = keyMetaForkDataSize();
    if (data->keyMetaDataSize > sizeof(data->keyMetaData) ||
        keyMetaCopyForkData(data->keyMetaData,
                            data->keyMetaDataSize) != C_OK)
    {
        data->keyMetaDataSize = 0;
        return FALSE;
    }
    return TRUE;
}

static rdbSaveInfo *copyRdbSaveInfo(rdbSaveInfo *dst, const void *src,
                                    size_t src_size)
{
    if (src_size == 0) return NULL;
    if (src == NULL || src_size != sizeof(*dst)) {
        serverLog(LL_WARNING,
            "QFork RDB metadata ABI mismatch: got %llu bytes, expected %llu",
            (unsigned long long)src_size,
            (unsigned long long)sizeof(*dst));
        return (rdbSaveInfo *)(uintptr_t)-1;
    }
    memcpy(dst, src, sizeof(*dst));
    return dst;
}

BOOL SetupRedisGlobals(LPVOID redisData, size_t redisDataSize,
    uint8_t *dictHashSeed, const RedisACLForkData *redisACL,
    const RedisCoreForkData *redisCore, LPVOID sharedData,
    size_t sharedDataSize, LPVOID redisModules, size_t usedMemory)
{
#ifndef NO_QFORKIMPL
    if (redisData == NULL || redisDataSize != sizeof(server) ||
        redisACL == NULL || redisCore == NULL || redisModules == NULL ||
        sharedData == NULL || sharedDataSize != sizeof(shared))
    {
        serverLog(LL_WARNING,
            "QFork global ABI mismatch: server=%llu/%llu shared=%llu/%llu",
            (unsigned long long)redisDataSize,
            (unsigned long long)sizeof(server),
            (unsigned long long)sharedDataSize,
            (unsigned long long)sizeof(shared));
        return FALSE;
    }

    memcpy(&server, redisData, redisDataSize);
    memcpy(&shared, sharedData, sharedDataSize);
    /* SetupLogging() runs before the QFork child restores the copied server
     * state, leaving the fresh process at the logger's warning default.
     * Apply the parent's configured verbosity before persistence code logs. */
    setLogVerbosityLevel(server.verbosity);
    dictSetHashFunctionSeed(dictHashSeed);
    ACLSetForkData(redisACL);
    configSetQForkData((dict *)redisCore->configs);
    asmSetQForkState(redisCore->asmManager);
    moduleSetForkData(redisModules);
    if (keyMetaSetForkData(redisCore->keyMetaData,
                           redisCore->keyMetaDataSize) != C_OK)
    {
        serverLog(LL_WARNING,
                  "QFork key metadata registry ABI mismatch: got %llu bytes, expected %llu",
                  (unsigned long long)redisCore->keyMetaDataSize,
                  (unsigned long long)keyMetaForkDataSize());
        return FALSE;
    }
    zmalloc_set_used_memory(usedMemory);
    crc64_init();
    /* QFork children are fresh processes, so executable-image globals are not
     * inherited with the mapped Redis heap. Redis 7.2's module callbacks call
     * through getMonotonicUs; initialize that process-local function pointer
     * before any persistence event can enter moduleCreateContext(). */
    monotonicInit();
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;
    server.main_thread_id = pthread_self();
    /* Live clients and their connection/event-loop ownership are process
     * local.  Never expose the copied parent registries to persistence-child
     * module callbacks.  Temporary module clients are unlinked and continue
     * to be created from the mapped database/configuration snapshot. */
    server.current_client = NULL;
    server.executing_client = NULL;
    server.clients = listCreate();
    server.clients_index = raxNew();
    server.clients_to_close = listCreate();
    server.clients_pending_write = listCreate();
    server.clients_pending_read = listCreate();
    server.clients_timeout_table = raxNew();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.unblocked_clients = listCreate();
    server.ready_keys = listCreate();
    server.tracking_pending_keys = listCreate();
    server.pending_push_messages = listCreate();
    server.clients_waiting_acks = listCreate();
    server.postponed_clients = listCreate();
    server.master = NULL;
    server.cached_master = NULL;
    server.repl_transfer_s = NULL;
    server.repl_rdb_transfer_s = NULL;
    server.repl_rdb_ch_state = REPL_RDB_CH_STATE_NONE;
    server.repl_main_ch_state = REPL_MAIN_CH_NONE;
    server.module_pipe[0] = -1;
    server.module_pipe[1] = -1;
    server.cluster_config_file_lock_fd = -1;
    /* Parent FDAPI descriptor numbers are process-local. Until child-info
     * pipe handles are passed explicitly, disable this optional metrics path
     * so it cannot collide with the AOF descriptors opened in the child. */
    server.child_info_pipe[0] = -1;
    server.child_info_pipe[1] = -1;
    clusterSetQForkState();
    if (connTypeInitialize() != C_OK) {
        serverLog(LL_WARNING,
                  "QFork could not initialize child connection types");
        return FALSE;
    }
    rehydrateCommandTableForQFork();
#endif
    return TRUE;
}

int do_rdbSave(int req, char* filename, const void *rdb_save_info,
               size_t rdb_save_info_size, int rdbflags)
{
#ifndef NO_QFORKIMPL
    rdbSaveInfo rsi;
    rdbSaveInfo *rsiptr = copyRdbSaveInfo(&rsi, rdb_save_info,
                                          rdb_save_info_size);
    if (rsiptr == (rdbSaveInfo *)(uintptr_t)-1) return C_ERR;

    server.in_fork_child = CHILD_TYPE_RDB;
    server.child_pid = GetCurrentProcessId();
    server.child_type = CHILD_TYPE_RDB;
    server.rdb_child_type = RDB_CHILD_TYPE_DISK;
    updateDictResizePolicy();
    redisSetProcTitle("redis-rdb-bgsave");
    redisSetCpuAffinity(server.bgsave_cpulist);

    if (rdbSave(req, filename, rsiptr, rdbflags) != C_OK) {
        serverLog(LL_WARNING,"rdbSave failed in qfork: %s", strerror(errno));
        return C_ERR;
    }
    sendChildCowInfo(CHILD_INFO_TYPE_RDB_COW_SIZE, "RDB");
#endif
    return C_OK;
}

int do_aofSave(char* filename)
{
#ifndef NO_QFORKIMPL
    int rewriteAppendOnlyFile(char *filename);

    server.in_fork_child = CHILD_TYPE_AOF;
    server.child_pid = GetCurrentProcessId();
    server.child_type = CHILD_TYPE_AOF;
    updateDictResizePolicy();
    redisSetProcTitle("redis-aof-rewrite");
    redisSetCpuAffinity(server.aof_rewrite_cpulist);

    /* Redis 7.2's multi-part AOF rewrite does not stream parent-side
     * differences into the child.  The parent rotates to a new incremental
     * file before QFork and the child writes only the immutable base snapshot.
     * Parent FDAPI descriptor numbers are process-local, so invalidate the
     * copied append descriptor in the fresh child. */
    server.aof_fd = -1;
    if (rewriteAppendOnlyFile(filename) != C_OK) {
        serverLog(LL_WARNING, "rewriteAppendOnlyFile failed in qfork: %s", strerror(errno));
        return C_ERR;
    }
    sendChildCowInfo(CHILD_INFO_TYPE_AOF_COW_SIZE, "AOF rewrite");
#endif
    return C_OK;
}

int do_socketSave(int req, const void *rdb_save_info,
                  size_t rdb_save_info_size, int rdb_pipe_write_fd,
                  int safe_to_exit_pipe_fd)
{
#ifndef NO_QFORKIMPL
    int retval;
    char dummy;
    ssize_t nread;
    rio rdb;
    rdbSaveInfo rsi;
    rdbSaveInfo *rsiptr = copyRdbSaveInfo(&rsi, rdb_save_info,
                                          rdb_save_info_size);
    if (rsiptr == (rdbSaveInfo *)(uintptr_t)-1) return C_ERR;

    server.in_fork_child = CHILD_TYPE_RDB;
    server.child_pid = GetCurrentProcessId();
    server.child_type = CHILD_TYPE_RDB;
    server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
    server.rdb_pipe_read = -1;
    server.rdb_child_exit_pipe = -1;
    updateDictResizePolicy();
    redisSetProcTitle("redis-rdb-to-slaves");
    redisSetCpuAffinity(server.bgsave_cpulist);

    rioInitWithFd(&rdb, rdb_pipe_write_fd);
    if (req & SLAVE_REQ_SLOTS_SNAPSHOT)
        retval = slotSnapshotSaveRio(req, &rdb, NULL);
    else
        retval = rdbSaveRioWithEOFMark(req, &rdb, NULL, rsiptr);
    if (retval == C_OK && rioFlush(&rdb) == 0)
        retval = C_ERR;
    if (retval == C_OK)
        sendChildCowInfo(CHILD_INFO_TYPE_RDB_COW_SIZE, "RDB");
    else
        serverLog(LL_WARNING, "Diskless RDB save failed in qfork: %s",
                  strerror(errno));

    rioFreeFd(&rdb);
    close(rdb_pipe_write_fd);

    /* Keep the QFork snapshot alive until the parent drains the RDB stream.
     * The read returns EOF when the parent closes its exit-gate writer. */
    do {
        nread = read(safe_to_exit_pipe_fd, &dummy, 1);
    } while (nread == -1 && errno == EINTR);
    UNUSED(nread);
    close(safe_to_exit_pipe_fd);
    return retval;
#else
    UNUSED(req);
    UNUSED(rdb_save_info);
    UNUSED(rdb_save_info_size);
    UNUSED(rdb_pipe_write_fd);
    UNUSED(safe_to_exit_pipe_fd);
    return C_ERR;
#endif
}
