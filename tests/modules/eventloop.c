/* This module contains four tests :
 * 1- test.sanity    : Basic tests for argument validation mostly.
 * 2- test.sendbytes : Creates a pipe and registers its fds to the event loop,
 *                     one end of the pipe for read events and the other end for
 *                     the write events. On writable event, data is written. On
 *                     readable event data is read. Repeated until all data is
 *                     received.
 * 3- test.iteration : A test for BEFORE_SLEEP and AFTER_SLEEP callbacks.
 *                     Counters are incremented each time these events are
 *                     fired. They should be equal and increment monotonically.
 * 4- test.oneshot   : Test for oneshot API
 */

#include "redismodule.h"
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif
#include <memory.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
static int eventloopPipe(int pipefds[2]) {
    return RedisModule_Win32Pipe(pipefds);
}

static long long eventloopRead(int fd, void *buf, size_t count) {
    return RedisModule_Win32Read(fd, buf, count);
}

static long long eventloopWrite(int fd, const void *buf, size_t count) {
    return RedisModule_Win32Write(fd, buf, count);
}

static int eventloopClose(int fd) {
    return RedisModule_Win32Close(fd);
}
#else
static int eventloopPipe(int pipefds[2]) {
    return pipe(pipefds);
}

static long long eventloopRead(int fd, void *buf, size_t count) {
    return read(fd, buf, count);
}

static long long eventloopWrite(int fd, const void *buf, size_t count) {
    return write(fd, buf, count);
}

static int eventloopClose(int fd) {
    return close(fd);
}
#endif

int fds[2] = {-1, -1};
long long buf_size;
char *src;
long long src_offset;
char *dst;
long long dst_offset;

RedisModuleBlockedClient *sendbytes_bc;
RedisModuleCtx *sendbytes_reply_ctx;
RedisModuleBlockedClient *oneshot_bc;
RedisModuleCtx *oneshot_reply_ctx;

static void cleanupSendbytesIO(void) {
    if (fds[0] >= 0) {
        RedisModule_EventLoopDel(fds[0], REDISMODULE_EVENTLOOP_READABLE);
        eventloopClose(fds[0]);
        fds[0] = -1;
    }
    if (fds[1] >= 0) {
        RedisModule_EventLoopDel(fds[1], REDISMODULE_EVENTLOOP_WRITABLE);
        eventloopClose(fds[1]);
        fds[1] = -1;
    }

    if (src != NULL) {
        RedisModule_Free(src);
        src = NULL;
    }
    if (dst != NULL) {
        RedisModule_Free(dst);
        dst = NULL;
    }
}

static int failSendbytesSetup(RedisModuleCtx *ctx, const char *error) {
    cleanupSendbytesIO();

    if (sendbytes_reply_ctx != NULL) {
        RedisModule_ReplyWithError(sendbytes_reply_ctx, error);
        RedisModule_FreeThreadSafeContext(sendbytes_reply_ctx);
        sendbytes_reply_ctx = NULL;
    } else {
        RedisModule_ReplyWithError(ctx, error);
    }

    if (sendbytes_bc != NULL) {
        RedisModule_UnblockClient(sendbytes_bc, NULL);
        sendbytes_bc = NULL;
    }
    return REDISMODULE_OK;
}

void onReadable(int fd, void *user_data, int mask) {
    REDISMODULE_NOT_USED(mask);

    RedisModule_Assert(strcmp(user_data, "userdataread") == 0);

    while (1) {
        long long rd = eventloopRead(fd, dst + dst_offset, buf_size - dst_offset);
        if (rd <= 0)
            return;
        dst_offset += rd;

        /* Received all bytes */
        if (dst_offset == buf_size) {
            if (memcmp(src, dst, (size_t)buf_size) == 0)
                RedisModule_ReplyWithSimpleString(sendbytes_reply_ctx, "OK");
            else
                RedisModule_ReplyWithError(sendbytes_reply_ctx, "ERR bytes mismatch");

            cleanupSendbytesIO();

            RedisModule_FreeThreadSafeContext(sendbytes_reply_ctx);
            sendbytes_reply_ctx = NULL;
            RedisModule_UnblockClient(sendbytes_bc, NULL);
            sendbytes_bc = NULL;
            return;
        }
    };
}

void onWritable(int fd, void *user_data, int mask) {
    REDISMODULE_NOT_USED(user_data);
    REDISMODULE_NOT_USED(mask);

    RedisModule_Assert(strcmp(user_data, "userdatawrite") == 0);

    while (1) {
        /* Check if we sent all data */
        if (src_offset >= buf_size)
            return;
        long long written = eventloopWrite(fd, src + src_offset, buf_size - src_offset);
        if (written <= 0) {
            return;
        }

        src_offset += written;
    };
}

/* Create a pipe(), register pipe fds to the event loop and send/receive data
 * using them. */
int sendbytes(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (RedisModule_StringToLongLong(argv[1], &buf_size) != REDISMODULE_OK ||
        buf_size <= 0 || (uint64_t)buf_size > (uint64_t)SIZE_MAX) {
        RedisModule_ReplyWithError(ctx, "Invalid integer value");
        return REDISMODULE_OK;
    }

    if (sendbytes_bc != NULL) {
        RedisModule_ReplyWithError(ctx, "ERR sendbytes test already running");
        return REDISMODULE_OK;
    }

    if (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) {
        RedisModule_ReplyWithError(ctx, "ERR sendbytes test cannot block in this context");
        return REDISMODULE_OK;
    }

    sendbytes_bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    if (sendbytes_bc == NULL)
        return failSendbytesSetup(ctx, "ERR failed to block client");

    sendbytes_reply_ctx = RedisModule_GetThreadSafeContext(sendbytes_bc);
    if (sendbytes_reply_ctx == NULL)
        return failSendbytesSetup(ctx, "ERR failed to create reply context");

    /* Allocate source buffer and write some random data */
    src = RedisModule_Calloc(1,(size_t)buf_size);
    src_offset = 0;
    memset(src, rand() % 0xFF, (size_t)buf_size);
    size_t prefix_len = strlen("randomtestdata");
    if (prefix_len > (size_t)buf_size) prefix_len = (size_t)buf_size;
    memcpy(src, "randomtestdata", prefix_len);

    dst = RedisModule_Calloc(1,(size_t)buf_size);
    dst_offset = 0;

    /* Create a pipe and register it to the event loop. */
    if (eventloopPipe(fds) < 0)
        return failSendbytesSetup(ctx, "ERR failed to create eventloop pipe");
#ifndef _WIN32
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0)
        return failSendbytesSetup(ctx, "ERR failed to make eventloop pipe nonblocking");
#endif

    if (RedisModule_EventLoopAdd(fds[0], REDISMODULE_EVENTLOOP_READABLE,
        onReadable, "userdataread") != REDISMODULE_OK)
        return failSendbytesSetup(ctx, "ERR failed to register readable event");
    if (RedisModule_EventLoopAdd(fds[1], REDISMODULE_EVENTLOOP_WRITABLE,
        onWritable, "userdatawrite") != REDISMODULE_OK)
        return failSendbytesSetup(ctx, "ERR failed to register writable event");
    return REDISMODULE_OK;
}

int sanity(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    int sanity_fds[2] = {-1, -1};
    if (eventloopPipe(sanity_fds) < 0) {
        RedisModule_ReplyWithError(ctx, "ERR failed to create eventloop pipe");
        return REDISMODULE_OK;
    }

    if (RedisModule_EventLoopAdd(sanity_fds[0], 9999999, onReadable, NULL)
        == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(-1, REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(99999999, REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(sanity_fds[0], REDISMODULE_EVENTLOOP_READABLE, NULL, NULL)
        == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR null callback should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(sanity_fds[0], 9999999, onReadable, NULL)
        == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(sanity_fds[0], REDISMODULE_EVENTLOOP_READABLE)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR del on non-registered fd should not fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(sanity_fds[0], 9999999) == REDISMODULE_OK ||
        errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(-1, REDISMODULE_EVENTLOOP_READABLE)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(99999999, REDISMODULE_EVENTLOOP_READABLE)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(sanity_fds[0], REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR Add failed");
        goto out;
    }
    if (RedisModule_EventLoopAdd(sanity_fds[0], REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR Adding same fd twice failed");
        goto out;
    }
    if (RedisModule_EventLoopDel(sanity_fds[0], REDISMODULE_EVENTLOOP_READABLE)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR Del failed");
        goto out;
    }
    if (RedisModule_EventLoopAddOneShot(NULL, NULL) == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR null callback should fail");
        goto out;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
out:
    RedisModule_EventLoopDel(sanity_fds[0], REDISMODULE_EVENTLOOP_READABLE |
                                             REDISMODULE_EVENTLOOP_WRITABLE);
    RedisModule_EventLoopDel(sanity_fds[1], REDISMODULE_EVENTLOOP_READABLE |
                                             REDISMODULE_EVENTLOOP_WRITABLE);
    eventloopClose(sanity_fds[0]);
    eventloopClose(sanity_fds[1]);
    return REDISMODULE_OK;
}

static long long beforeSleepCount;
static long long afterSleepCount;

int iteration(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    /* On each event loop iteration, eventloopCallback() is called. We increment
     * beforeSleepCount and afterSleepCount, so these two should be equal.
     * We reply with iteration count, caller can test if iteration count
     * increments monotonically */
    RedisModule_Assert(beforeSleepCount == afterSleepCount);
    RedisModule_ReplyWithLongLong(ctx, beforeSleepCount);
    return REDISMODULE_OK;
}

void oneshotCallback(void* arg)
{
    RedisModule_Assert(strcmp(arg, "userdata") == 0);
    RedisModule_ReplyWithSimpleString(oneshot_reply_ctx, "OK");
    RedisModule_FreeThreadSafeContext(oneshot_reply_ctx);
    oneshot_reply_ctx = NULL;
    RedisModule_UnblockClient(oneshot_bc, NULL);
    oneshot_bc = NULL;
}

int oneshot(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (oneshot_bc != NULL) {
        RedisModule_ReplyWithError(ctx, "ERR oneshot test already running");
        return REDISMODULE_OK;
    }

    if (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) {
        RedisModule_ReplyWithError(ctx, "ERR oneshot test cannot block in this context");
        return REDISMODULE_OK;
    }

    oneshot_bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    if (oneshot_bc == NULL) {
        RedisModule_ReplyWithError(ctx, "ERR failed to block client");
        return REDISMODULE_OK;
    }
    oneshot_reply_ctx = RedisModule_GetThreadSafeContext(oneshot_bc);
    if (oneshot_reply_ctx == NULL) {
        RedisModule_ReplyWithError(ctx, "ERR failed to create reply context");
        RedisModule_UnblockClient(oneshot_bc, NULL);
        oneshot_bc = NULL;
        return REDISMODULE_OK;
    }

    if (RedisModule_EventLoopAddOneShot(oneshotCallback, "userdata") != REDISMODULE_OK) {
        RedisModule_ReplyWithError(oneshot_reply_ctx, "ERR oneshot failed");
        RedisModule_FreeThreadSafeContext(oneshot_reply_ctx);
        oneshot_reply_ctx = NULL;
        RedisModule_UnblockClient(oneshot_bc, NULL);
        oneshot_bc = NULL;
    }
    return REDISMODULE_OK;
}

void eventloopCallback(struct RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(eid);
    REDISMODULE_NOT_USED(subevent);
    REDISMODULE_NOT_USED(data);

    RedisModule_Assert(eid.id == REDISMODULE_EVENT_EVENTLOOP);
    if (subevent == REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP)
        beforeSleepCount++;
    else if (subevent == REDISMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP)
        afterSleepCount++;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"eventloop",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* Test basics. */
    if (RedisModule_CreateCommand(ctx, "test.sanity", sanity, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* Register a command to create a pipe() and send data through it by using
     * event loop API. */
    if (RedisModule_CreateCommand(ctx, "test.sendbytes", sendbytes, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* Register a command to return event loop iteration count. */
    if (RedisModule_CreateCommand(ctx, "test.iteration", iteration, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "test.oneshot", oneshot, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_EventLoop,
        eventloopCallback) != REDISMODULE_OK) return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
