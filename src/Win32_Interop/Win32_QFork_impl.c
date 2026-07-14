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
#include "Win32_Portability.h"

void moduleSetForkData(void *data);
int rdbSaveRioWithEOFMark(rio *rdb, int *error, rdbSaveInfo *rsi);

void SetupRedisGlobals(LPVOID redisData, size_t redisDataSize, uint8_t *dictHashSeed,
    LPVOID redisModules, size_t usedMemory)
{
#ifndef NO_QFORKIMPL
    memcpy(&server, redisData, redisDataSize);
    dictSetHashFunctionSeed(dictHashSeed);
    moduleSetForkData(redisModules);
    zmalloc_set_used_memory(usedMemory);
    crc64_init();
    /* Parent FDAPI descriptor numbers are process-local. Until child-info
     * pipe handles are passed explicitly, disable this optional metrics path
     * so it cannot collide with the AOF descriptors opened in the child. */
    server.child_info_pipe[0] = -1;
    server.child_info_pipe[1] = -1;
#endif
}

int do_rdbSave(char* filename)
{
#ifndef NO_QFORKIMPL
    server.in_fork_child = CHILD_TYPE_RDB;
    server.child_pid = GetCurrentProcessId();
    server.child_type = CHILD_TYPE_RDB;
    server.rdb_child_type = RDB_CHILD_TYPE_DISK;
    updateDictResizePolicy();
    redisSetProcTitle("redis-rdb-bgsave");
    redisSetCpuAffinity(server.bgsave_cpulist);

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    if( rdbSave(filename, rsiptr) != C_OK ) {
        serverLog(LL_WARNING,"rdbSave failed in qfork: %s", strerror(errno));
        return C_ERR;
    }
    sendChildCowInfo(CHILD_INFO_TYPE_RDB_COW_SIZE, "RDB");
#endif
    return C_OK;
}

int do_aofSave(char* filename, int aof_pipe_read_ack, int aof_pipe_read_data, int aof_pipe_write_ack)
{
#ifndef NO_QFORKIMPL
    int rewriteAppendOnlyFile(char *filename);

    server.in_fork_child = CHILD_TYPE_AOF;
    server.child_pid = GetCurrentProcessId();
    server.child_type = CHILD_TYPE_AOF;
    updateDictResizePolicy();
    redisSetProcTitle("redis-aof-rewrite");
    redisSetCpuAffinity(server.aof_rewrite_cpulist);

    server.aof_pipe_write_ack_to_parent = aof_pipe_write_ack;
    server.aof_pipe_read_ack_from_parent = aof_pipe_read_ack;
    server.aof_pipe_read_data_from_parent = aof_pipe_read_data;
    server.aof_pipe_read_ack_from_child = -1;
    server.aof_pipe_write_ack_to_child = -1;
    server.aof_pipe_write_data_to_child = -1;
    if (rewriteAppendOnlyFile(filename) != C_OK) {
        serverLog(LL_WARNING, "rewriteAppendOnlyFile failed in qfork: %s", strerror(errno));
        return C_ERR;
    }
    sendChildCowInfo(CHILD_INFO_TYPE_AOF_COW_SIZE, "AOF rewrite");
#endif
    return C_OK;
}

int do_socketSave(int rdb_pipe_write_fd, int safe_to_exit_pipe_fd)
{
#ifndef NO_QFORKIMPL
    int retval;
    char dummy;
    ssize_t nread;
    rio rdb;
    rdbSaveInfo rsi, *rsiptr;

    server.in_fork_child = CHILD_TYPE_RDB;
    server.child_pid = GetCurrentProcessId();
    server.child_type = CHILD_TYPE_RDB;
    server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
    server.rdb_pipe_read = -1;
    server.rdb_child_exit_pipe = -1;
    updateDictResizePolicy();
    redisSetProcTitle("redis-rdb-to-slaves");
    redisSetCpuAffinity(server.bgsave_cpulist);

    rsiptr = rdbPopulateSaveInfo(&rsi);
    rioInitWithFd(&rdb, rdb_pipe_write_fd);
    retval = rdbSaveRioWithEOFMark(&rdb, NULL, rsiptr);
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
    UNUSED(rdb_pipe_write_fd);
    UNUSED(safe_to_exit_pipe_fd);
    return C_ERR;
#endif
}
