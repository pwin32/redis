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

#include "win32fixes.h"
#include "..\ae.h"
#include "..\adlist.h"
#include <mswsock.h>
#include "win32_wsiocp.h"
#include "Win32_FDAPI.h"
#include "Win32_Error.h"
#include "Win32_Assert.h"
#include <errno.h>

static HANDLE iocph;

#define SUCCEEDED_WITH_IOCP(result) \
    ((result) || (FDAPI_WSAGetLastError() == WSA_IO_PENDING))

/* For zero length reads use shared buf */
static char zreadchar[1];

static BOOL WSIOCP_RemoveWriteRequest(list *requestlist, void *value) {
    listNode *node;
    if (requestlist == NULL) return FALSE;
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

iocpSockState* WSIOCP_GetExistingSocketState(int fd) {
    void *state = NULL;
    if (!FDAPI_GetSocketState(fd, &state)) {
        return NULL;
    } else {
        return (iocpSockState *)state;
    }
}

/* Get the socket state. Create if not found. */
iocpSockState* WSIOCP_GetSocketState(int fd) {
    iocpSockState *existing = WSIOCP_GetExistingSocketState(fd);
    if (existing != NULL) return existing;

    iocpSockState *candidate =
        (iocpSockState *) CallocMemoryNoCOW(sizeof(iocpSockState));
    if (candidate == NULL) return NULL;
    candidate->fd = fd;

    void *actual = NULL;
    if (!FDAPI_InstallSocketState(fd, candidate, &actual)) {
        FreeMemoryNoCOW(candidate);
        return NULL;
    } else {
        if (actual != candidate) {
            FreeMemoryNoCOW(candidate);
        }
        return (iocpSockState *)actual;
    }
}

void WSIOCP_DisposeAcceptRequest(aacceptreq *request) {
    if (request == NULL) return;
    if ((int)request->accept != -1) close((int)request->accept);
    FreeMemoryNoCOW(request->buf);
    FreeMemoryNoCOW(request);
}

static void WSIOCP_DisposeCompletedAccepts(iocpSockState *socketState) {
    aacceptreq *request = socketState->reqs;
    socketState->reqs = NULL;
    while (request != NULL) {
        aacceptreq *next = request->next;
        WSIOCP_DisposeAcceptRequest(request);
        request = next;
    }
}

static BOOL WSIOCP_HasOutstandingState(const iocpSockState *socketState) {
    return socketState->wreqs != 0 || socketState->accept_pending != NULL ||
           (socketState->masks &
            (READ_QUEUED | CONNECT_PENDING | ACCEPT_PENDING)) != 0;
}

/* Finalize a descriptor whose underlying Winsock socket has already been
 * closed once its last overlapped completion has been consumed. */
BOOL WSIOCP_TryFinalizeClosedState(iocpSockState *socketState) {
    if (socketState == NULL ||
        (socketState->masks & CLOSE_PENDING) == 0 ||
        WSIOCP_HasOutstandingState(socketState)) {
        return FALSE;
    }

    int fd = socketState->fd;
    WSIOCP_DisposeCompletedAccepts(socketState);
    socketState->masks &= ~CLOSE_PENDING;
    FDAPI_ClearSocketState(fd, socketState);
    FDAPI_ClearSocketInfo(fd);
    FreeMemoryNoCOW(socketState);
    return TRUE;
}

/* Closes the socket state or sets the CLOSE_PENDING mask bit.
 * Returns TRUE if closed, FALSE if pending. */
BOOL WSIOCP_CloseSocketState(iocpSockState* socketState) {
    if (socketState == NULL) return TRUE;
    socketState->masks &= ~(SOCKET_ATTACHED | AE_WRITABLE | AE_READABLE);
    WSIOCP_DisposeCompletedAccepts(socketState);
    if (!WSIOCP_HasOutstandingState(socketState)) {
        FDAPI_ClearSocketState(socketState->fd, socketState);
        FreeMemoryNoCOW(socketState);
        return TRUE;
    } else {
        socketState->masks |= CLOSE_PENDING;
        return FALSE;
    }
}

BOOL WSIOCP_CloseSocketStateRFD(int rfd) {
    return WSIOCP_CloseSocketState(WSIOCP_GetExistingSocketState(rfd));
}

/* For each async socket, associate the owning event loop's completion port. */
int WSIOCP_SocketAttachToPort(int fd, iocpSockState *socketState,
                             HANDLE completionPort) {
    if (socketState == NULL) {
        socketState = WSIOCP_GetSocketState(fd);
    }

    if (completionPort != NULL && socketState != NULL) {
        if (FDAPI_SocketAttachIOCP(fd, completionPort)) {
            socketState->masks = SOCKET_ATTACHED;
            socketState->wreqs = 0;
            return 0;
        }
    } else {
        errno = EINVAL;
    }

    return -1;
}

int WSIOCP_SocketAttach(int fd, iocpSockState *socketState) {
    return WSIOCP_SocketAttachToPort(fd, socketState, iocph);
}

const int ACCEPTEX_ADDRESS_BUFFER_SIZE = sizeof(struct sockaddr_storage) + 32;

int WSIOCP_QueueAccept(int listenfd) {
    iocpSockState *sockstate;
    iocpSockState *accsockstate;
    SOCKADDR_STORAGE listenaddr;
    int listenaddrlen = sizeof(listenaddr);
    BOOL result;
    DWORD bytes;
    int acceptfd;
    aacceptreq * areq;

    if ((sockstate = WSIOCP_GetSocketState(listenfd)) == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (sockstate->accept_pending != NULL ||
        (sockstate->masks & ACCEPT_PENDING) != 0) {
        errno = EALREADY;
        return -1;
    }

    if (getsockname(listenfd, (struct sockaddr *) &listenaddr,
                    &listenaddrlen) == SOCKET_ERROR) {
        return -1;
    }
    if (listenaddr.ss_family != AF_INET &&
        listenaddr.ss_family != AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    /* Keep the AcceptEx target socket in the listener's address family. */
    acceptfd = socket(listenaddr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (acceptfd == -1) {
        return -1;
    }

    accsockstate = WSIOCP_GetSocketState(acceptfd);
    if (accsockstate == NULL) {
        errno = EINVAL;
        close(acceptfd);
        return -1;
    }

    accsockstate->masks = SOCKET_ATTACHED;
    // Keep accept socket in buf len until accepted
    areq = (aacceptreq *) CallocMemoryNoCOW(sizeof(aacceptreq));
    if (areq == NULL) {
        close(acceptfd);
        errno = ENOMEM;
        return -1;
    }
    areq->buf = CallocMemoryNoCOW(ACCEPTEX_ADDRESS_BUFFER_SIZE * 2);
    if (areq->buf == NULL) {
        close(acceptfd);
        FreeMemoryNoCOW(areq);
        errno = ENOMEM;
        return -1;
    }
    areq->accept = acceptfd;
    areq->next = NULL;
    sockstate->accept_pending = areq;

    result = FDAPI_AcceptEx(listenfd, acceptfd,
                            areq->buf, 0,
                            ACCEPTEX_ADDRESS_BUFFER_SIZE,
                            ACCEPTEX_ADDRESS_BUFFER_SIZE,
                            &bytes, &areq->ov);
    if (SUCCEEDED_WITH_IOCP(result)){
        sockstate->masks |= ACCEPT_PENDING;
    } else {
        int saved_errno = errno;
        sockstate->accept_pending = NULL;
        sockstate->masks &= ~ACCEPT_PENDING;
        accsockstate->masks = 0;
        close(acceptfd);
        FreeMemoryNoCOW(areq->buf);
        FreeMemoryNoCOW(areq);
        errno = saved_errno;
        return -1;
    }

    return 0;
}

/* Listen using extension function to get faster accepts */
int WSIOCP_Listen(int rfd, int backlog) {
    iocpSockState *sockstate = WSIOCP_GetSocketState(rfd);
    if (sockstate == NULL) {
        errno = EINVAL;
        return SOCKET_ERROR;
    }

    if (WSIOCP_SocketAttach(rfd, sockstate) != 0) {
        return SOCKET_ERROR;
    }

    sockstate->masks |= LISTEN_SOCK;

    if (listen(rfd, backlog) != 0) {
        return SOCKET_ERROR;
    }

    if (WSIOCP_QueueAccept(rfd) != 0) {
        return SOCKET_ERROR;
    }

    return 0;
}

/* Return the queued accept socket */
int WSIOCP_Accept(int fd, struct sockaddr *sa, socklen_t *len) {
    iocpSockState *sockstate;
    int acceptfd;
    int result;
    SOCKADDR *plocalsa = NULL;
    SOCKADDR *premotesa = NULL;
    int locallen = 0;
    int remotelen = 0;
    aacceptreq * areq;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = EINVAL;
        return SOCKET_ERROR;
    }

    areq = sockstate->reqs;
    if (areq == NULL) {
        errno = EWOULDBLOCK;
        return SOCKET_ERROR;
    }

    sockstate->reqs = areq->next;

    acceptfd = (int) areq->accept;

    result = FDAPI_UpdateAcceptContext(acceptfd, fd);
    if (result == SOCKET_ERROR) {
        int saved_errno = errno;
        close(acceptfd);
        FreeMemoryNoCOW(areq->buf);
        FreeMemoryNoCOW(areq);
        WSIOCP_QueueAccept(fd);
        errno = saved_errno;
        return SOCKET_ERROR;
    }

    if (!FDAPI_GetAcceptExSockaddrs(acceptfd,
                                    areq->buf,
                                    0,
                                    ACCEPTEX_ADDRESS_BUFFER_SIZE,
                                    ACCEPTEX_ADDRESS_BUFFER_SIZE,
                                    &plocalsa, &locallen,
                                    &premotesa, &remotelen)) {
        int saved_errno = errno;
        close(acceptfd);
        FreeMemoryNoCOW(areq->buf);
        FreeMemoryNoCOW(areq);
        WSIOCP_QueueAccept(fd);
        errno = saved_errno;
        return SOCKET_ERROR;
    }

    if (sa != NULL) {
        if (remotelen > 0) {
            if (remotelen < *len) {
                *len = remotelen;
            }
            memcpy(sa, premotesa, *len);
        } else {
            *len = 0;
        }
    }

    if (WSIOCP_SocketAttach(acceptfd, NULL) != 0) {
        int saved_errno = errno;
        close(acceptfd);
        FreeMemoryNoCOW(areq->buf);
        FreeMemoryNoCOW(areq);
        WSIOCP_QueueAccept(fd);
        errno = saved_errno;
        return SOCKET_ERROR;
    }

    FreeMemoryNoCOW(areq->buf);
    FreeMemoryNoCOW(areq);

    // Queue another accept
    if (WSIOCP_QueueAccept(fd) == -1) {
        return SOCKET_ERROR;
    }

    return acceptfd;
}

/* After doing a read, the caller needs to call this method in
 * order to continue to check for read events.
 * This is not necessary if the caller will delete read events */
int WSIOCP_QueueNextRead(int fd) {
    iocpSockState *sockstate;
    int result;
    WSABUF zreadbuf;
    DWORD bytesReceived = 0;
    DWORD recvFlags = 0;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* ConnectEx and read readiness share ov_read. Never reuse it while an
     * overlapped operation is still pending. */
    if ((sockstate->masks & SOCKET_ATTACHED) == 0 ||
        (sockstate->masks & (READ_QUEUED | CONNECT_PENDING)) != 0) {
        return 0;
    }

    // Use zero length read with overlapped to get notification
    // of when data is available
    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));

    zreadbuf.buf = zreadchar;
    zreadbuf.len = 0;
    result = FDAPI_WSARecv(fd,
                           &zreadbuf,
                           1,
                           &bytesReceived,
                           &recvFlags,
                           &sockstate->ov_read,
                           NULL);
    if (SUCCEEDED_WITH_IOCP(result == 0)){
        sockstate->masks |= READ_QUEUED;
    } else {
        errno = win32_errno_from_system_error(FDAPI_WSAGetLastError());
        sockstate->masks &= ~READ_QUEUED;
        return -1;
    }
    return 0;
}

/* Wrapper for send.
 * Enables use of WSA Send to get IOCP notification of completion.
 * Returns -1 with errno = EINPROGRESS if callback will be invoked later */
int WSIOCP_SocketSend(int fd, char *buf, int len, void *eventLoop,
                      void *client, void *data, void *proc) {
    iocpSockState *sockstate;
    int result;
    asendreq *areq;
    DWORD bytesSent = 0;

    if (len < 0 || (buf == NULL && len != 0)) {
        errno = EINVAL;
        return SOCKET_ERROR;
    }

    sockstate = WSIOCP_GetSocketState(fd);

    if (sockstate != NULL &&
        (sockstate->masks & CONNECT_PENDING)) {
        aeWait(fd, AE_WRITABLE, 50);
    }

    // If not an async socket, do normal send
    if (sockstate == NULL ||
        (sockstate->masks & SOCKET_ATTACHED) == 0 ||
        proc == NULL) {
        result = (int) write(fd, buf, len);
        return result;
    }

    // Use overlapped structure to send using IOCP
    areq = (asendreq *) CallocMemoryNoCOW(sizeof(asendreq));
    if (areq == NULL) {
        errno = ENOMEM;
        return SOCKET_ERROR;
    }
    areq->wbuf.len = len;
    areq->wbuf.buf = buf;
    areq->eventLoop = (aeEventLoop *) eventLoop;
    areq->req.client = client;
    areq->req.data = data;
    areq->req.len = len;
    areq->req.buf = buf;
    areq->proc = (aeFileProc *) proc;

    /* Publish ownership before submitting the overlapped operation. A very
     * fast completion must never race ahead of the request list, and list
     * allocation failure is still recoverable before WSASend starts. */
    if (listAddNodeTail(&sockstate->wreqlist, areq) == NULL) {
        FreeMemoryNoCOW(areq);
        errno = ENOMEM;
        return SOCKET_ERROR;
    }
    sockstate->wreqs++;

    result = FDAPI_WSASend(fd,
                           &areq->wbuf,
                           1,
                           &bytesSent,
                           0,
                           &areq->ov,
                           NULL);
	
    if (SUCCEEDED_WITH_IOCP(result == 0)) {
        errno = EINPROGRESS;
    } else {
        errno = win32_errno_from_system_error(FDAPI_WSAGetLastError());
        WSIOCP_RemoveWriteRequest(&sockstate->wreqlist, areq);
        sockstate->wreqs--;
        FreeMemoryNoCOW(areq);
    }
    return SOCKET_ERROR;
}

/* For non-blocking connect with IOCP */
int WSIOCP_SocketConnect(int fd, const SOCKADDR_STORAGE *socketAddrStorage) {
    int result;
    iocpSockState *sockstate;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = EINVAL;
        return SOCKET_ERROR;
    }

    if (WSIOCP_SocketAttach(fd, sockstate) != 0) {
        return SOCKET_ERROR;
    }

    /* ConnectEx posts an IOCP completion even when it succeeds immediately. */
    sockstate->masks |= CONNECT_PENDING;
    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));
    
    // Need to bind sock before connectex
    switch (socketAddrStorage->ss_family) {
        case AF_INET:
        {
            SOCKADDR_IN addr;
            memset(&addr, 0, sizeof(SOCKADDR_IN));
            addr.sin_family = socketAddrStorage->ss_family;
            addr.sin_addr.S_un.S_addr = INADDR_ANY;
            addr.sin_port = 0;
            result = bind(fd, (SOCKADDR*) &addr, sizeof(addr));
            if (result == SOCKET_ERROR) {
                sockstate->masks &= ~CONNECT_PENDING;
                return SOCKET_ERROR;
            }

            result = FDAPI_ConnectEx(fd,
                                     (SOCKADDR*) socketAddrStorage,
                                     sizeof(SOCKADDR_IN),
                                     NULL,
                                     0,
                                     NULL,
                                     &sockstate->ov_read);
            break;
        }
        case AF_INET6:
        {
            SOCKADDR_IN6 addr;
            memset(&addr, 0, sizeof(SOCKADDR_IN6));
            addr.sin6_family = socketAddrStorage->ss_family;
            memset(&(addr.sin6_addr.u.Byte), 0, 16);
            addr.sin6_port = 0;
            result = bind(fd, (SOCKADDR*) &addr, sizeof(addr));
            if (result == SOCKET_ERROR) {
                sockstate->masks &= ~CONNECT_PENDING;
                return SOCKET_ERROR;
            }

            result = FDAPI_ConnectEx(fd,
                                     (SOCKADDR*) socketAddrStorage,
                                     sizeof(SOCKADDR_IN6),
                                     NULL,
                                     0,
                                     NULL,
                                     &sockstate->ov_read);
            break;
        }
        default:
        {
            ASSERT(socketAddrStorage->ss_family == AF_INET || socketAddrStorage->ss_family == AF_INET6);
            sockstate->masks &= ~CONNECT_PENDING;
            errno = EINVAL;
            return SOCKET_ERROR;
        }
    }

    if (result != TRUE) {
        result = FDAPI_WSAGetLastError();
        if (result == ERROR_IO_PENDING) {
            errno = EINPROGRESS;
        } else {
            sockstate->masks &= ~CONNECT_PENDING;
            errno = win32_errno_from_system_error(result);
            return SOCKET_ERROR;
        }
    }
    return 0;
}

int WSIOCP_SocketConnectBind(int fd, const SOCKADDR_STORAGE *socketAddrStorage, const char* source_addr) {
    int result;
    iocpSockState *sockstate;
    SOCKADDR_STORAGE sourceStorage;
    const struct sockaddr *sourceSockaddr = NULL;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = EINVAL;
        return SOCKET_ERROR;
    }

    if (WSIOCP_SocketAttach(fd, sockstate) != 0) {
        return SOCKET_ERROR;
    }

    /* ConnectEx posts an IOCP completion even when it succeeds immediately. */
    sockstate->masks |= CONNECT_PENDING;
    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));

    if (source_addr != NULL && source_addr[0] != '\0') {
        if (!ParseStorageAddress(source_addr, 0, &sourceStorage)) {
            sockstate->masks &= ~CONNECT_PENDING;
            errno = EINVAL;
            return SOCKET_ERROR;
        }
        sourceSockaddr = (const struct sockaddr *)&sourceStorage;
    }

    // Need to bind sock before ConnectEx.
    int storageSize = 0;
    switch (socketAddrStorage->ss_family) {
        case AF_INET:
        {
            storageSize = sizeof(SOCKADDR_IN);
            SOCKADDR_IN addr;
            memset(&addr, 0, storageSize);
            if (sourceSockaddr != NULL) {
                if (sourceStorage.ss_family != AF_INET) {
                    sockstate->masks &= ~CONNECT_PENDING;
                    errno = EAFNOSUPPORT;
                    return SOCKET_ERROR;
                }
                memcpy(&addr, sourceSockaddr, sizeof(addr));
            } else {
                addr.sin_family = socketAddrStorage->ss_family;
                addr.sin_addr.S_un.S_addr = INADDR_ANY;
                addr.sin_port = 0;
            }
            result = bind(fd, (SOCKADDR*) &addr, sizeof(addr));
            break;
        }
        case AF_INET6:
        {
            storageSize = sizeof(SOCKADDR_IN6);
            SOCKADDR_IN6 addr;
            memset(&addr, 0, storageSize);
            if (sourceSockaddr != NULL) {
                if (sourceStorage.ss_family != AF_INET6) {
                    sockstate->masks &= ~CONNECT_PENDING;
                    errno = EAFNOSUPPORT;
                    return SOCKET_ERROR;
                }
                memcpy(&addr, sourceSockaddr, sizeof(addr));
            } else {
                addr.sin6_family = socketAddrStorage->ss_family;
                memset(&(addr.sin6_addr.u.Byte), 0, 16);
                addr.sin6_port = 0;
            }
            result = bind(fd, (SOCKADDR*) &addr, sizeof(addr));
            break;
        }
        default:
        {
            ASSERT(socketAddrStorage->ss_family == AF_INET || socketAddrStorage->ss_family == AF_INET6);
            sockstate->masks &= ~CONNECT_PENDING;
            errno = EINVAL;
            return SOCKET_ERROR;
        }
    }

    if (result == SOCKET_ERROR) {
        sockstate->masks &= ~CONNECT_PENDING;
        return SOCKET_ERROR;
    }

    result = FDAPI_ConnectEx(fd, (const LPSOCKADDR) socketAddrStorage,
                             storageSize, NULL, 0, NULL, &sockstate->ov_read);
    if (result != TRUE) {
        result = FDAPI_WSAGetLastError();
        if (result == ERROR_IO_PENDING) {
            errno = EINPROGRESS;
        } else {
            sockstate->masks &= ~CONNECT_PENDING;
            errno = win32_errno_from_system_error(result);
            return SOCKET_ERROR;
        }
    }
    return 0;
}

void WSIOCP_Init(HANDLE iocp) {
    if (iocph == NULL) iocph = iocp;
    FDAPI_SetCloseSocketState(WSIOCP_CloseSocketStateRFD);
}

void WSIOCP_Cleanup(HANDLE iocp) {
    if (iocph == iocp) iocph = NULL;
}

static HANDLE privateheap;
static INIT_ONCE privateHeapInitOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitializePrivateHeapOnce(PINIT_ONCE once,
                                               PVOID parameter,
                                               PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;

    privateheap = HeapCreate(0, 0, 0);
    return privateheap != NULL;
}

void* CallocMemoryNoCOW(size_t size) {
    if (!InitOnceExecuteOnce(&privateHeapInitOnce,
                            InitializePrivateHeapOnce, NULL, NULL))
        return NULL;
    return HeapAlloc(privateheap, HEAP_ZERO_MEMORY, size);
}

void FreeMemoryNoCOW(void * ptr) {
    if (ptr != NULL) HeapFree(privateheap, 0, ptr);
}
