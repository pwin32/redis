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

/* IOCP-based ae.c module  */

#include "win32_Interop/win32fixes.h"
#include "adlist.h"
#include "win32_Interop/win32_wsiocp.h"
#include "win32_Interop/Win32_FDAPI.h"
#include "win32_Interop/Win32_APIs.h"
#include "win32_Interop/Win32_Error.h"
#include "win32_Interop/Win32_RedisLog.h"

#define MAX_SOCKET_LOOKUP       65535
#define WRITE_REARM_BATCH       64

/* Use GetQueuedCompletionStatusEx if possible.
 * Try to load the function pointer dynamically.
 * If it is not available, use GetQueuedCompletionStatus */
typedef BOOL(WINAPI *sGetQueuedCompletionStatusEx)
            (HANDLE CompletionPort,
            LPOVERLAPPED_ENTRY lpCompletionPortEntries,
            ULONG ulCount,
            PULONG ulNumEntriesRemoved,
            DWORD dwMilliseconds,
            BOOL fAlertable);
sGetQueuedCompletionStatusEx pGetQueuedCompletionStatusEx;

/* Structure that keeps state of sockets and Completion port handle */
typedef struct aeApiState {
    HANDLE iocp;
    int setsize;
    ULONGLONG next_accept_rearm_ms;
    ULONGLONG next_accept_ready_ms;
    ULONGLONG next_write_rearm_ms;
    int accept_ready_cursor;
    int write_rearm_cursor;
} aeApiState;

/* Find matching value in list and remove. If found return TRUE */
BOOL removeMatchFromList(list *requestlist, void *value) {
    listNode *node;
    if (requestlist == NULL) {
        return FALSE;
    }
    node = listFirst(requestlist);
    while (node != NULL) {
        if (listNodeValue(node) == value) {
            listDelNode(requestlist, node);
            return TRUE;
        }
        node = listNextNode(node);
    }
    return FALSE;
}

/* Called by ae to initialize state */
static int aeApiCreate(aeEventLoop *eventLoop) {
    HMODULE kernel32_module;
    aeApiState *state = (aeApiState *) CallocMemoryNoCOW(sizeof(aeApiState));

    if (!state) return -1;

    // Create a single IOCP to be shared by all sockets
    state->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
                                         NULL,
                                         0,
                                         1);
    if (state->iocp == NULL) {
        FreeMemoryNoCOW(state);
        return -1;
    }

    pGetQueuedCompletionStatusEx = NULL;
    kernel32_module = GetModuleHandleW(L"kernel32.dll");
    if (kernel32_module != NULL)
        win32_get_proc_address(kernel32_module,
                               "GetQueuedCompletionStatusEx",
                               &pGetQueuedCompletionStatusEx,
                               sizeof(pGetQueuedCompletionStatusEx));

    state->setsize = eventLoop->setsize;
    eventLoop->apidata = state;
    WSIOCP_Init(state->iocp);
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    ((aeApiState *) (eventLoop->apidata))->setsize = setsize;
    return 0;
}

/* Termination */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    WSIOCP_Cleanup(state->iocp);
    CloseHandle(state->iocp);
    FreeMemoryNoCOW(state);
}

/* Monitor state changes for a socket */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    iocpSockState *sockstate = WSIOCP_GetSocketState(fd);
    if (sockstate == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (sockstate->masks & SOCKET_ATTACHED) {
        if (sockstate->completion_port != state->iocp) {
            errno = EINVAL;
            return -1;
        }
    } else {
        if (WSIOCP_SocketAttachToPort(fd, sockstate, state->iocp) != 0)
            return -1;
    }

    if (mask & AE_READABLE) {
        sockstate->masks |= AE_READABLE;
        if ((sockstate->masks & CONNECT_PENDING) == 0) {
            if (sockstate->masks & LISTEN_SOCK) {
                // Actually a listen. Do not treat as read
            } else {
                if ((sockstate->masks & READ_QUEUED) == 0) {
                    // Queue up a 0 byte read
                    if (WSIOCP_QueueNextRead(fd) != 0) return -1;
                }
            }
        }
    }
    if (mask & AE_WRITABLE) {
        sockstate->masks |= AE_WRITABLE;
        if ((sockstate->masks & CONNECT_PENDING) == 0) {
            // If no write active, then need to queue write ready
            if (sockstate->wreqs == 0) {
                if (WSIOCP_QueueWriteReady(fd) != 0)
                    return -1;
            }
        }
    }
    return 0;
}

/* Stop monitoring state changes for a socket */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    iocpSockState *sockstate = WSIOCP_GetExistingSocketState(fd);
    if (sockstate == NULL) {
        errno = EINVAL;
        return;
    }

    if (mask & AE_READABLE) sockstate->masks &= ~AE_READABLE;
    if (mask & AE_WRITABLE) {
        sockstate->masks &= ~AE_WRITABLE;
        WSIOCP_CancelWriteReady(fd);
    }
}

static int aeApiGetCompletion(aeApiState *state, OVERLAPPED_ENTRY *entry,
                              DWORD timeout) {
    memset(entry, 0, sizeof(*entry));

    if (pGetQueuedCompletionStatusEx != NULL) {
        ULONG numComplete = 0;
        BOOL rc = pGetQueuedCompletionStatusEx(state->iocp,
                                               entry,
                                               1,
                                               &numComplete,
                                               timeout,
                                               FALSE);
        return rc && numComplete == 1;
    }

    /* GetQueuedCompletionStatus returns FALSE both for a timeout and for a
     * failed overlapped operation.  A non-NULL OVERLAPPED identifies a real
     * completion and must still be processed. */
    BOOL rc = GetQueuedCompletionStatus(state->iocp,
                                        &entry->dwNumberOfBytesTransferred,
                                        &entry->lpCompletionKey,
                                        &entry->lpOverlapped,
                                        timeout);
    return rc || entry->lpOverlapped != NULL;
}

static void aeApiUnknownCompletion(iocpSockState *sockstate, int rfd) {
    if (sockstate != NULL && sockstate->unknownComplete == 0) {
        sockstate->unknownComplete = 1;
        serverLog(LL_WARNING,
                  "IOCP completion did not match fd=%d; ignoring late completion",
                  rfd);
    }
}

static int aeApiFireEvent(aeEventLoop *eventLoop, iocpSockState *sockstate,
                          int fd, int mask) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    if (fd < 0 || fd >= state->setsize ||
        sockstate->completion_port != state->iocp) return 0;
    WSIOCP_RetainSocketState(sockstate);
    eventLoop->fired[0].fd = fd;
    eventLoop->fired[0].mask = mask;
    eventLoop->fired[0].backend_data = sockstate;
    return 1;
}

/* AcceptEx completion is edge-triggered, while Redis listener callbacks rely
 * on level-triggered readiness.  A callback can intentionally leave a
 * completed accept queued (cluster listeners do this while loading), so
 * surface that readiness again at a bounded rate until it is consumed. */
static int aeApiFireQueuedAccept(aeEventLoop *eventLoop) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    int count = eventLoop->maxfd + 1;
    ULONGLONG now;

    if (count <= 0) return 0;
    now = GetTickCount64();
    if (state->next_accept_ready_ms != 0 &&
        now < state->next_accept_ready_ms) return 0;

    for (int offset = 0; offset < count; offset++) {
        int fd = (state->accept_ready_cursor + offset) % count;
        iocpSockState *sockstate = WSIOCP_GetExistingSocketState(fd);
        if (sockstate == NULL || sockstate->completion_port != state->iocp ||
            sockstate->reqs == NULL ||
            (sockstate->masks & (LISTEN_SOCK | AE_READABLE)) !=
                (LISTEN_SOCK | AE_READABLE) ||
            (sockstate->masks & CLOSE_PENDING) != 0)
            continue;

        state->accept_ready_cursor = (fd + 1) % count;
        state->next_accept_ready_ms = now + 100;
        return aeApiFireEvent(eventLoop, sockstate, fd, AE_READABLE);
    }

    state->next_accept_ready_ms = 0;
    return 0;
}

static int aeApiProcessCompletion(aeEventLoop *eventLoop,
                                  const OVERLAPPED_ENTRY *entry) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    int rfd = (int) entry->lpCompletionKey;
    iocpSockState *sockstate = WSIOCP_GetExistingSocketState(rfd);
    if (sockstate == NULL) {
        aeApiUnknownCompletion(NULL, rfd);
        return 0;
    }
    if (sockstate->completion_port != state->iocp) {
        /* A descriptor was recycled after a completion had already been
         * queued to its former owner's port.  Never apply that packet to the
         * new socket state. */
        return 0;
    }

    /* AcceptEx has a distinct OVERLAPPED owner.  Keep the exact owner in the
     * state so a late completion cannot be mistaken for a new listener's
     * accept after descriptor reuse. */
    if ((sockstate->masks & ACCEPT_PENDING) &&
        sockstate->accept_pending != NULL &&
        entry->lpOverlapped == &sockstate->accept_pending->ov) {
        aacceptreq *areq = sockstate->accept_pending;
        sockstate->accept_pending = NULL;
        sockstate->masks &= ~ACCEPT_PENDING;

        if (sockstate->masks & CLOSE_PENDING) {
            WSIOCP_DisposeAcceptRequest(areq);
            WSIOCP_TryFinalizeClosedState(sockstate);
            return 0;
        }

        areq->next = sockstate->reqs;
        sockstate->reqs = areq;
        if (sockstate->masks & AE_READABLE)
            return aeApiFireEvent(eventLoop, sockstate, rfd, AE_READABLE);
        return 0;
    }

    /* ConnectEx and read readiness share ov_read, but their state bits make
     * the completion type unambiguous. */
    if ((sockstate->masks & CONNECT_PENDING) &&
        entry->lpOverlapped == &sockstate->ov_read) {
        sockstate->masks &= ~CONNECT_PENDING;
        if (sockstate->masks & CLOSE_PENDING) {
            WSIOCP_TryFinalizeClosedState(sockstate);
            return 0;
        }

        DWORD connected_bytes = 0;
        DWORD connected_flags = 0;
        if (FDAPI_WSAGetOverlappedResult(rfd, &sockstate->ov_read,
                                         &connected_bytes, FALSE,
                                         &connected_flags) == FALSE) {
            int connect_error =
                win32_errno_from_system_error(FDAPI_WSAGetLastError());
            int visible_mask = sockstate->masks & (AE_READABLE | AE_WRITABLE);
            WSIOCP_SetDeferredError(rfd, connect_error);
            if (visible_mask == 0) visible_mask = AE_WRITABLE;
            return aeApiFireEvent(eventLoop, sockstate, rfd, visible_mask);
        }

        /* ConnectEx requires the socket context to be updated before the
         * connected socket is used for normal send/receive operations.  If
         * this fails, surface the error through the ordinary connection
         * callback so the owner closes and retries the link. */
        if (FDAPI_UpdateConnectContext(rfd) == SOCKET_ERROR) {
            int context_error =
                win32_errno_from_system_error(FDAPI_WSAGetLastError());
            int visible_mask = sockstate->masks & (AE_READABLE | AE_WRITABLE);
            WSIOCP_SetDeferredError(rfd, context_error);
            serverLog(LL_WARNING,
                      "IOCP ConnectEx context update failed for fd=%d: %s",
                      rfd, wsa_strerror(context_error));
            if (visible_mask == 0) visible_mask = AE_WRITABLE;
            return aeApiFireEvent(eventLoop, sockstate, rfd, visible_mask);
        }

        if (aeApiAddEvent(eventLoop, rfd, sockstate->masks) != 0) {
            int rearm_errno = errno;
            int visible_mask = sockstate->masks & (AE_READABLE | AE_WRITABLE);
            WSIOCP_SetDeferredError(rfd, rearm_errno);
            serverLog(LL_WARNING,
                      "IOCP connect completion could not rearm fd=%d: %s",
                      rfd, wsa_strerror(rearm_errno));
            /* The original connect event is writable, so preserve a visible
             * event even if the failed rearm was for the read side. The
             * connection handler consumes the deferred error and follows its
             * ordinary failed-connect/close path. */
            if (visible_mask == 0) visible_mask = AE_WRITABLE;
            return aeApiFireEvent(eventLoop, sockstate, rfd, visible_mask);
        }
        return 0;
    }

    if ((sockstate->masks & READ_QUEUED) &&
        entry->lpOverlapped == &sockstate->ov_read) {
        sockstate->masks &= ~READ_QUEUED;
        if (sockstate->masks & CLOSE_PENDING) {
            WSIOCP_TryFinalizeClosedState(sockstate);
            return 0;
        }
        if (sockstate->masks & AE_READABLE)
            return aeApiFireEvent(eventLoop, sockstate, rfd, AE_READABLE);
        return 0;
    }

    if (sockstate->wreqs > 0 && entry->lpOverlapped != NULL) {
        asendreq *areq = (asendreq *) entry->lpOverlapped;
        if (removeMatchFromList(&sockstate->wreqlist, areq)) {
            DWORD written = 0;
            DWORD flags = 0;
            int closing;

            if (!(sockstate->masks & CLOSE_PENDING) && areq->proc != NULL)
                FDAPI_WSAGetOverlappedResult(rfd, &areq->ov, &written, FALSE, &flags);

            /* Keep the state alive while a completion callback can close its
             * client and consequently request finalization. */
            if (areq->proc != NULL && !(sockstate->masks & CLOSE_PENDING)) {
                WSIOCP_RetainSocketState(sockstate);
                areq->proc(areq->eventLoop, rfd, &areq->req, (int) written);
                closing = (sockstate->masks & CLOSE_PENDING) != 0;
                sockstate->wreqs--;
                FreeMemoryNoCOW(areq);
                if (closing) {
                    /* Release may finalize and free the state.  Do not touch
                     * sockstate on this path afterward. */
                    WSIOCP_ReleaseSocketState(sockstate);
                    return 0;
                }
                /* Without CLOSE_PENDING, releasing the callback reference
                 * cannot finalize the still-open socket state. */
                WSIOCP_ReleaseSocketState(sockstate);
            } else {
                sockstate->wreqs--;
                FreeMemoryNoCOW(areq);
                if (sockstate->masks & CLOSE_PENDING) {
                    WSIOCP_TryFinalizeClosedState(sockstate);
                    return 0;
                }
            }

            if (sockstate->wreqs == 0 && (sockstate->masks & AE_WRITABLE))
                return aeApiFireEvent(eventLoop, sockstate, rfd, AE_WRITABLE);
            return 0;
        }
    }

    aeApiUnknownCompletion(sockstate, rfd);
    return 0;
}

static void aeApiRetryAccepts(aeEventLoop *eventLoop) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    if (!WSIOCP_AcceptRearmPending()) {
        state->next_accept_rearm_ms = 0;
        return;
    }

    ULONGLONG now = GetTickCount64();
    if (state->next_accept_rearm_ms != 0 &&
        now < state->next_accept_rearm_ms) return;
    state->next_accept_rearm_ms = now + 100;

    for (int fd = 0; fd <= eventLoop->maxfd; fd++) {
        iocpSockState *sockstate = WSIOCP_GetExistingSocketState(fd);
        if (sockstate == NULL || sockstate->completion_port != state->iocp ||
            (sockstate->masks & ACCEPT_REARM_NEEDED) == 0 ||
            (sockstate->masks & CLOSE_PENDING) != 0)
            continue;

        if (WSIOCP_QueueAccept(fd) == 0) {
            serverLog(LL_NOTICE,
                      "IOCP listener fd=%d resumed AcceptEx after a transient failure",
                      fd);
        } else if (!sockstate->accept_rearm_logged) {
            serverLog(LL_WARNING,
                      "IOCP listener fd=%d could not rearm AcceptEx: %s",
                      fd, wsa_strerror(errno));
            sockstate->accept_rearm_logged = 1;
        }
    }
}

/* A nonblocking send may consume all currently available buffer space after a
 * writable callback.  Retry the real Winsock readiness probe at a bounded
 * cadence; once ready, WSIOCP_QueueWriteReady posts the ordinary one-shot
 * completion consumed by this backend. */
static int aeApiRetryWrites(aeEventLoop *eventLoop) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    if (!WSIOCP_WriteRearmPending()) {
        state->next_write_rearm_ms = 0;
        return 0;
    }

    ULONGLONG now = GetTickCount64();
    if (state->next_write_rearm_ms != 0 &&
        now < state->next_write_rearm_ms) return 0;
    state->next_write_rearm_ms = now + 10;

    int fd_count = eventLoop->maxfd + 1;
    if (fd_count <= 0) return 0;

    struct pollfd candidates[WRITE_REARM_BATCH];
    iocpSockState *candidate_states[WRITE_REARM_BATCH];
    int candidate_count = 0;
    int last_fd = state->write_rearm_cursor % fd_count;

    /* Batch blocked writers into one WSAPoll call.  The cursor keeps a large
     * set of slow clients fair without scanning or probing every fd on each
     * 10 ms retry tick. */
    for (int offset = 0;
         offset < fd_count && candidate_count < WRITE_REARM_BATCH;
         offset++) {
        int fd = (state->write_rearm_cursor + offset) % fd_count;
        iocpSockState *sockstate = WSIOCP_GetExistingSocketState(fd);
        if (sockstate == NULL || sockstate->completion_port != state->iocp ||
            (sockstate->masks & WRITE_REARM_NEEDED) == 0 ||
            (sockstate->masks & CLOSE_PENDING) != 0)
            continue;

        candidates[candidate_count].fd = fd;
        candidates[candidate_count].events = POLLOUT;
        candidates[candidate_count].revents = 0;
        candidate_states[candidate_count] = sockstate;
        candidate_count++;
        last_fd = fd;
    }

    if (candidate_count == 0) return 0;
    state->write_rearm_cursor = (last_fd + 1) % fd_count;

    int poll_result = poll(candidates, candidate_count, 0);
    for (int index = 0; index < candidate_count; index++) {
        if (poll_result >= 0 && candidates[index].revents == 0) continue;

        int fd = candidates[index].fd;
        iocpSockState *sockstate = candidate_states[index];
        if (WSIOCP_QueueWriteReady(fd) != 0) {
            int rearm_error = errno;
            if (!sockstate->write_rearm_logged) {
                serverLog(LL_WARNING,
                          "IOCP fd=%d could not rearm write readiness: %s",
                          fd, wsa_strerror(rearm_error));
                sockstate->write_rearm_logged = 1;
            }
            if (sockstate->masks & AE_WRITABLE)
                return aeApiFireEvent(eventLoop, sockstate, fd, AE_WRITABLE);
        }
    }
    return 0;
}

/* Return at most one visible readiness event.  Internal completion packets
 * (connect completion, disabled reads, and close cleanup) are consumed here
 * with a zero timeout, bounded so a busy queue cannot monopolize the event
 * loop. */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp,
                     int *completion_budget) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    int first_timeout = (tvp == NULL) ? 100 :
                        (tvp->tv_sec * 1000) + (tvp->tv_usec / 1000);

    if (state->setsize <= 0 || *completion_budget <= 0) return 0;
    aeApiRetryAccepts(eventLoop);
    eventLoop->fired[0].backend_data = NULL;
    if (aeApiFireQueuedAccept(eventLoop)) return 1;
    if (aeApiRetryWrites(eventLoop)) return 1;
    if (WSIOCP_WriteRearmPending() && first_timeout > 10)
        first_timeout = 10;
    int attempt = 0;
    while (*completion_budget > 0) {
        OVERLAPPED_ENTRY entry;
        DWORD timeout = attempt == 0 ? (DWORD)first_timeout : 0;
        if (!aeApiGetCompletion(state, &entry, timeout)) return 0;
        (*completion_budget)--;
        attempt++;
        if (aeApiProcessCompletion(eventLoop, &entry)) return 1;
    }
    return 0;
}

static int aeApiFiredEventValid(aeEventLoop *eventLoop, aeFiredEvent *event) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    iocpSockState *expected = (iocpSockState *)event->backend_data;
    return expected != NULL &&
           WSIOCP_GetExistingSocketState(event->fd) == expected &&
           expected->completion_port == state->iocp &&
           (expected->masks & CLOSE_PENDING) == 0;
}

static void aeApiReleaseFiredEvent(aeFiredEvent *event) {
    iocpSockState *socketState = (iocpSockState *)event->backend_data;
    event->backend_data = NULL;
    if (socketState != NULL) WSIOCP_ReleaseSocketState(socketState);
}

/* Name of this event handler */
static char *aeApiName(void) {
    return "WinSock_IOCP";
}
