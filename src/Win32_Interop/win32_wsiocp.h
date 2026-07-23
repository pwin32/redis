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

#ifndef WIN32_INTEROP_WSIOCP_H
#define WIN32_INTEROP_WSIOCP_H

#include "win32_wsiocp2.h"
#include "../ae.h"

 /* structs and functions for using IOCP with windows sockets */

 /* structure used for async write requests.
  * contains overlapped, WSABuf, and callback info
  * NOTE: OVERLAPPED must be first member */
typedef struct asendreq {
    OVERLAPPED ov;
    WSABUF wbuf;
    WSIOCP_Request req;
    aeFileProc *proc;
    aeEventLoop *eventLoop;
} asendreq;

/* structure used for async accept requests.
 * contains overlapped, accept socket, accept buffer
 * NOTE: OVERLAPPED must be first member */
typedef struct aacceptreq {
    OVERLAPPED ov;
    SOCKET accept;
    void *buf;
    struct aacceptreq *next;
} aacceptreq;

/* per socket information */
typedef struct iocpSockState {
    int masks;
    int fd;
    HANDLE completion_port;
    aacceptreq *reqs;
    aacceptreq *accept_pending;
    int wreqs;
    int event_refs;
    int accept_rearm_logged;
    int write_rearm_logged;
    int deferred_error;
    OVERLAPPED ov_read;
    list wreqlist;
    int unknownComplete;
} iocpSockState;

#define READ_QUEUED         0x000100
#define SOCKET_ATTACHED     0x000400
#define ACCEPT_PENDING      0x000800
#define LISTEN_SOCK         0x001000
#define CONNECT_PENDING     0x002000
#define CLOSE_PENDING       0x004000
#define ACCEPT_REARM_NEEDED 0x008000
#define WRITE_REARM_NEEDED  0x010000

void           WSIOCP_Init(HANDLE iocp);
void           WSIOCP_Cleanup(HANDLE iocp);
iocpSockState* WSIOCP_GetExistingSocketState(int fd);
iocpSockState* WSIOCP_GetSocketState(int fd);
int            WSIOCP_SocketAttach(int fd, iocpSockState *socketState);
int            WSIOCP_SocketAttachToPort(int fd, iocpSockState *socketState,
                                         HANDLE iocp);
BOOL           WSIOCP_CloseSocketState(iocpSockState* pSocketState);
BOOL           WSIOCP_CloseSocketStateRFD(int rfd);
void           WSIOCP_RetainSocketState(iocpSockState *socketState);
void           WSIOCP_ReleaseSocketState(iocpSockState *socketState);
BOOL           WSIOCP_TryFinalizeClosedState(iocpSockState *socketState);
void           WSIOCP_DisposeAcceptRequest(aacceptreq *request);
BOOL           WSIOCP_AcceptRearmPending(void);
BOOL           WSIOCP_WriteRearmPending(void);


void* CallocMemoryNoCOW(size_t size);
void  FreeMemoryNoCOW(void * ptr);

#endif
