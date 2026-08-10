/* anet.c -- Basic TCP socket stuff made a bit less boring
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32_types.h"
#include "Win32_Interop/win32fixes.h"
#include "Win32_Interop/win32_wsiocp2.h"
#include "Win32_Interop/Win32_Error.h"
#define ANET_NOTUSED(V) ((void)(V))
#include <Mstcpip.h>
#endif

#include "fmacros.h"

#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#else
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <sys/stat.h>
#endif
#include <fcntl.h>
#include <string.h>
#ifndef _WIN32
#include <netdb.h>
#endif
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "anet.h"
#include "config.h"
#include "util.h"

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

static void anetSetError(char *err, const char *fmt, ...)
{
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

int anetGetError(int fd) {
    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    return sockerr;
}

int anetSetBlock(char *err, int fd, int non_block) {
    int flags;

    /* Set the socket blocking (if non_block is zero) or non-blocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
#ifdef _WIN32
    if ((flags = fcntl(fd, F_GETFL, 0)) == -1) { WIN_PORT_FIX
        anetSetError(err, "fcntl(F_GETFL): %s", wsa_strerror(errno));
#else
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
#endif
        return ANET_ERR;
    }

    /* Check if this flag has been set or unset, if so,
     * then there is no need to call fcntl to set/unset it again. */
    if (!!(flags & O_NONBLOCK) == !!non_block)
        return ANET_OK;

    if (non_block)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1) {
        anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s",
                     IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetNonBlock(char *err, int fd) {
    return anetSetBlock(err,fd,1);
}

int anetBlock(char *err, int fd) {
    return anetSetBlock(err,fd,0);
}

/* Enable the FD_CLOEXEC on the given fd to avoid fd leaks.
 * This function should be invoked for fd's on specific places
 * where fork + execve system calls are called. */
int anetCloexec(int fd) {
#ifdef _WIN32
    ANET_NOTUSED(fd);
    return ANET_OK;
#else
    int r;
    int flags;

    do {
        r = fcntl(fd, F_GETFD);
    } while (r == -1 && errno == EINTR);

    if (r == -1 || (r & FD_CLOEXEC))
        return r;

    flags = r | FD_CLOEXEC;

    do {
        r = fcntl(fd, F_SETFD, flags);
    } while (r == -1 && errno == EINTR);

    return r;
#endif
}

/* Enable TCP keep-alive mechanism to detect dead peers,
 * TCP_KEEPIDLE, TCP_KEEPINTVL and TCP_KEEPCNT will be set accordingly. */
int anetKeepAlive(char *err, int fd, int interval)
{
    int enabled = 1;
#ifdef _WIN32
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) == -1) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", wsa_strerror(errno));
        return ANET_ERR;
    }

    struct tcp_keepalive alive;
    DWORD bytes = 0;
    alive.onoff = TRUE;
    alive.keepalivetime = interval > (INT_MAX / 1000) ? UINT_MAX : (DWORD) interval * 1000;
    int keepalive_interval = interval / 10;
    if (keepalive_interval == 0) keepalive_interval = 1;
    alive.keepaliveinterval = (DWORD) keepalive_interval * 1000;
    if (FDAPI_WSAIoctl(fd, SIO_KEEPALIVE_VALS, &alive, sizeof(alive),
                       NULL, 0, &bytes, NULL, NULL) == SOCKET_ERROR) {
        anetSetError(err, "WSAIoctl(SIO_KEEPALIVE_VALS): %s", wsa_strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
#else
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)))
    {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }

    int idle;
    int intvl;
    int cnt;

    /* There are platforms that are expected to support the full mechanism of TCP keep-alive,
     * we want the compiler to emit warnings of unused variables if the preprocessor directives
     * somehow fail, and other than those platforms, just omit these warnings if they happen.
     */
#if !(defined(_AIX) || defined(__APPLE__) || defined(__DragonFly__) || \
    defined(__FreeBSD__) || defined(__illumos__) || defined(__linux__) || \
    defined(__NetBSD__) || defined(__sun))
    UNUSED(interval);
    UNUSED(idle);
    UNUSED(intvl);
    UNUSED(cnt);
#endif

#ifdef __sun
    /* The implementation of TCP keep-alive on Solaris/SmartOS is a bit unusual
     * compared to other Unix-like systems.
     * Thus, we need to specialize it on Solaris.
     *
     * There are two keep-alive mechanisms on Solaris:
     * - By default, the first keep-alive probe is sent out after a TCP connection is idle for two hours.
     * If the peer does not respond to the probe within eight minutes, the TCP connection is aborted.
     * You can alter the interval for sending out the first probe using the socket option TCP_KEEPALIVE_THRESHOLD
     * in milliseconds or TCP_KEEPIDLE in seconds.
     * The system default is controlled by the TCP ndd parameter tcp_keepalive_interval. The minimum value is ten seconds.
     * The maximum is ten days, while the default is two hours. If you receive no response to the probe,
     * you can use the TCP_KEEPALIVE_ABORT_THRESHOLD socket option to change the time threshold for aborting a TCP connection.
     * The option value is an unsigned integer in milliseconds. The value zero indicates that TCP should never time out and
     * abort the connection when probing. The system default is controlled by the TCP ndd parameter tcp_keepalive_abort_interval.
     * The default is eight minutes.
     *
     * - The second implementation is activated if socket option TCP_KEEPINTVL and/or TCP_KEEPCNT are set.
     * The time between each consequent probes is set by TCP_KEEPINTVL in seconds.
     * The minimum value is ten seconds. The maximum is ten days, while the default is two hours.
     * The TCP connection will be aborted after certain amount of probes, which is set by TCP_KEEPCNT, without receiving response.
     */

    idle = interval;
    if (idle < 10) idle = 10; // kernel expects at least 10 seconds
    if (idle > 10*24*60*60) idle = 10*24*60*60; // kernel expects at most 10 days

    /* `TCP_KEEPIDLE`, `TCP_KEEPINTVL`, and `TCP_KEEPCNT` were not available on Solaris
     * until version 11.4, but let's take a chance here. */
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle))) {
        anetSetError(err, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
        return ANET_ERR;
    }

    intvl = idle/3;
    if (intvl < 10) intvl = 10; /* kernel expects at least 10 seconds */
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl))) {
        anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }

    cnt = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt))) {
        anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }
#else
    /* Fall back to the first implementation of tcp-alive mechanism for older Solaris,
     * simulate the tcp-alive mechanism on other platforms via `TCP_KEEPALIVE_THRESHOLD` + `TCP_KEEPALIVE_ABORT_THRESHOLD`.
     */
    idle *= 1000; // kernel expects milliseconds
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE_THRESHOLD, &idle, sizeof(idle))) {
        anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Note that the consequent probes will not be sent at equal intervals on Solaris,
     * but will be sent using the exponential backoff algorithm. */
    int time_to_abort = idle;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE_ABORT_THRESHOLD, &time_to_abort, sizeof(time_to_abort))) {
        anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }
#endif

    return ANET_OK;

#endif

#ifdef TCP_KEEPIDLE
    /* Default settings are more or less garbage, with the keepalive time
     * set to 7200 by default on Linux and other Unix-like systems.
     * Modify settings to make the feature actually useful. */

    /* Send first probe after interval. */
    idle = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle))) {
        anetSetError(err, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
        return ANET_ERR;
    }
#elif defined(TCP_KEEPALIVE)
    /* Darwin/macOS uses TCP_KEEPALIVE in place of TCP_KEEPIDLE. */
    idle = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle))) {
        anetSetError(err, "setsockopt TCP_KEEPALIVE: %s\n", strerror(errno));
        return ANET_ERR;
    }
#endif

#ifdef TCP_KEEPINTVL
    /* Send next probes after the specified interval. Note that we set the
     * delay as interval / 3, as we send three probes before detecting
     * an error (see the next setsockopt call). */
    intvl = interval/3;
    if (intvl == 0) intvl = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl))) {
        anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }
#endif

#ifdef TCP_KEEPCNT
    /* Consider the socket in error state after three we send three ACK
     * probes without getting a reply. */
    cnt = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt))) {
        anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }
#endif

    return ANET_OK;
#endif
}

static int anetSetTcpNoDelay(char *err, int fd, int val)
{
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1)
    {
        anetSetError(err, "setsockopt TCP_NODELAY: %s",
                     IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetEnableTcpNoDelay(char *err, int fd)
{
    return anetSetTcpNoDelay(err, fd, 1);
}

int anetDisableTcpNoDelay(char *err, int fd)
{
    return anetSetTcpNoDelay(err, fd, 0);
}

/* Set the socket send timeout (SO_SNDTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
int anetSendTimeout(char *err, int fd, long long ms) {
#ifdef _WIN32
    DWORD timeout = ms <= 0 ? 0 : (ms > (long long) MAXDWORD ? MAXDWORD : (DWORD) ms);
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) == -1)
#else
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms%1000)*1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1)
#endif
    {
        anetSetError(err, "setsockopt SO_SNDTIMEO: %s",
                     IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* Set the socket receive timeout (SO_RCVTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
int anetRecvTimeout(char *err, int fd, long long ms) {
#ifdef _WIN32
    DWORD timeout = ms <= 0 ? 0 : (ms > (long long) MAXDWORD ? MAXDWORD : (DWORD) ms);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == -1)
#else
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms%1000)*1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1)
#endif
    {
        anetSetError(err, "setsockopt SO_RCVTIMEO: %s",
                     IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* Resolve the hostname "host" and set the string representation of the
 * IP address into the buffer pointed by "ipbuf".
 *
 * If flags is set to ANET_IP_ONLY the function only resolves hostnames
 * that are actually already IPv4 or IPv6 addresses. This turns the function
 * into a validating / normalizing function.
 *
 * If the flag ANET_PREFER_IPV4 is set, IPv4 is preferred over IPv6.
 * If the flag ANET_PREFER_IPV6 is set, IPv6 is preferred over IPv4.
 * */
int anetResolve(char *err, char *host, char *ipbuf, size_t ipbuf_len,
                       int flags)
{
    struct addrinfo hints, *info;
    int rv;

    memset(&hints,0,sizeof(hints));
    if (flags & ANET_IP_ONLY) hints.ai_flags = AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;
    if (flags & ANET_PREFER_IPV4 && !(flags & ANET_PREFER_IPV6)) {
        hints.ai_family = AF_INET;
    } else if (flags & ANET_PREFER_IPV6 && !(flags & ANET_PREFER_IPV4)) {
        hints.ai_family = AF_INET6;
    }
    hints.ai_socktype = SOCK_STREAM;  /* specify socktype to avoid dups */

    rv = getaddrinfo(host, NULL, &hints, &info);
    if (rv != 0 && hints.ai_family != AF_UNSPEC) {
        /* Try the other IP version. */
        hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;
        rv = getaddrinfo(host, NULL, &hints, &info);
    }
    if (rv != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    if (info->ai_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)info->ai_addr;
        inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbuf_len);
    } else {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)info->ai_addr;
        inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbuf_len);
    }

    freeaddrinfo(info);
    return ANET_OK;
}

static int anetSetReuseAddr(char *err, int fd) {
    int yes = 1;
    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_REUSEADDR: %s",
                     IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetCreateSocket(char *err, int domain) {
    int s;
    if ((s = socket(domain, SOCK_STREAM, IF_WIN32(IPPROTO_TCP, 0))) == -1) {
        anetSetError(err, "creating socket: %s",
                     IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }

    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
    if (anetSetReuseAddr(err,s) == ANET_ERR) {
        close(s);
        return ANET_ERR;
    }
    return s;
}

#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
#define ANET_CONNECT_BE_BINDING 2 /* Best effort binding. */
#ifdef _WIN32
static int anetTcpGenericConnect(char *err, const char *addr, int port,
                                 const char *source_addr, int flags) {
    int fd;
    SOCKADDR_STORAGE storage;
    if (!ParseStorageAddress(addr, port, &storage)) {
        anetSetError(err, "invalid address: %s", addr);
        errno = EINVAL;
        return ANET_ERR;
    }

    if ((fd = anetCreateSocket(err, storage.ss_family)) == ANET_ERR)
        return ANET_ERR;

    /* A pending ConnectEx cannot be queried with getpeername(). Save the
     * requested endpoint in the FDAPI map for address reporting. */
    FDAPI_SaveSocketAddrStorage(fd, &storage);
    int connect_result = source_addr ?
        WSIOCP_SocketConnectBind(fd, &storage, source_addr) :
        WSIOCP_SocketConnect(fd, &storage);
    if (connect_result == SOCKET_ERROR) {
        if ((errno == EINPROGRESS) && (flags & ANET_CONNECT_NONBLOCK))
            return fd;

        if (source_addr && (flags & ANET_CONNECT_BE_BINDING)) {
            close(fd);
            return anetTcpGenericConnect(err, addr, port, NULL, flags);
        }

        anetSetError(err, "connect: %s", wsa_strerror(errno));
        close(fd);
        return ANET_ERR;
    }

    return fd;
}
#else
static int anetTcpGenericConnect(char *err, const char *addr, int port,
                                 const char *source_addr, int flags)
{
    int s = ANET_ERR, rv;
    char portstr[6];  /* strlen("65535") + 1; */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;

    snprintf(portstr,sizeof(portstr),"%d",port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(addr,portstr,&hints,&servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        /* Try to create the socket and to connect it.
         * If we fail in the socket() call, or on connect(), we retry with
         * the next entry in servinfo. */
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;
        if (anetSetReuseAddr(err,s) == ANET_ERR) goto error;
        if (flags & ANET_CONNECT_NONBLOCK && anetNonBlock(err,s) != ANET_OK)
            goto error;
        if (source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(source_addr, NULL, &hints, &bservinfo)) != 0)
            {
                anetSetError(err, "%s", gai_strerror(rv));
                goto error;
            }
            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                anetSetError(err, "bind: %s", strerror(errno));
                goto error;
            }
        }
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            /* If the socket is non-blocking, it is ok for connect() to
             * return an EINPROGRESS error here. */
            if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
                goto end;
            close(s);
            s = ANET_ERR;
            continue;
        }

        /* If we ended an iteration of the for loop without errors, we
         * have a connected socket. Let's return to the caller. */
        goto end;
    }
    if (p == NULL)
        anetSetError(err, "creating socket: %s", strerror(errno));

error:
    if (s != ANET_ERR) {
        close(s);
        s = ANET_ERR;
    }

end:
    freeaddrinfo(servinfo);

    /* Handle best effort binding: if a binding address was used, but it is
     * not possible to create a socket, try again without a binding address. */
    if (s == ANET_ERR && source_addr && (flags & ANET_CONNECT_BE_BINDING)) {
        return anetTcpGenericConnect(err,addr,port,NULL,flags);
    } else {
        return s;
    }
}
#endif

int anetTcpNonBlockConnect(char *err, const char *addr, int port)
{
    return anetTcpGenericConnect(err,addr,port,NULL,ANET_CONNECT_NONBLOCK);
}

int anetTcpNonBlockBestEffortBindConnect(char *err, const char *addr, int port,
                                         const char *source_addr)
{
    return anetTcpGenericConnect(err,addr,port,source_addr,
            ANET_CONNECT_NONBLOCK|ANET_CONNECT_BE_BINDING);
}

int anetUnixGenericConnect(char *err, const char *path, int flags)
{
#ifdef _WIN32
    ANET_NOTUSED(err);
    ANET_NOTUSED(path);
    ANET_NOTUSED(flags);
    errno = EAFNOSUPPORT;
    return ANET_ERR;
#else
    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    sa.sun_family = AF_LOCAL;
    redis_strlcpy(sa.sun_path,path,sizeof(sa.sun_path));
    if (flags & ANET_CONNECT_NONBLOCK) {
        if (anetNonBlock(err,s) != ANET_OK) {
            close(s);
            return ANET_ERR;
        }
    }
    if (connect(s,(struct sockaddr*)&sa,sizeof(sa)) == -1) {
        if (errno == EINPROGRESS &&
            flags & ANET_CONNECT_NONBLOCK)
            return s;

        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
#endif
}

static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len, int backlog, mode_t perm) {
    if (bind(s,sa,len) == -1) {
        anetSetError(err, "bind: %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
        close(s);
        return ANET_ERR;
    }

#ifndef _WIN32
    if (sa->sa_family == AF_LOCAL && perm)
        redis_chmod(((struct sockaddr_un *) sa)->sun_path, perm);
#else
    ANET_NOTUSED(perm);
#endif

#ifdef _WIN32
    if (WSIOCP_Listen(s, backlog) == SOCKET_ERROR) {
#else
    if (listen(s, backlog) == -1) {
#endif
        anetSetError(err, "listen: %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetV6Only(char *err, int s) {
    int yes = 1;
    if (setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&yes,sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt: %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }
    return ANET_OK;
}

#ifdef _WIN32
static int anetSetExclusiveAddr(char *err, int fd) {
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_EXCLUSIVEADDRUSE: %s", wsa_strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}
#endif

static int _anetTcpServer(char *err, int port, char *bindaddr, int af, int backlog)
{
    int s = -1, rv;
    char _port[6];  /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;

    snprintf(_port,6,"%d",port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* No effect if bindaddr != NULL */
    if (bindaddr && !strcmp("*", bindaddr))
        bindaddr = NULL;
    if (af == AF_INET6 && bindaddr && !strcmp("::*", bindaddr))
        bindaddr = NULL;

    if ((rv = getaddrinfo(bindaddr,_port,&hints,&servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        if (af == AF_INET6 && anetV6Only(err,s) == ANET_ERR) goto error;
#ifdef _WIN32
        if (anetSetExclusiveAddr(err,s) == ANET_ERR) goto error;
#else
        if (anetSetReuseAddr(err,s) == ANET_ERR) goto error;
#endif
        if (anetListen(err,s,p->ai_addr,p->ai_addrlen,backlog,0) == ANET_ERR) s = ANET_ERR;
        goto end;
    }
    if (p == NULL) {
        anetSetError(err, "unable to bind socket, errno: %d", errno);
        goto error;
    }

error:
    if (s != -1) close(s);
    s = ANET_ERR;
end:
    freeaddrinfo(servinfo);
    return s;
}

int anetTcpServer(char *err, int port, char *bindaddr, int backlog)
{
    return _anetTcpServer(err, port, bindaddr, AF_INET, backlog);
}

int anetTcp6Server(char *err, int port, char *bindaddr, int backlog)
{
    return _anetTcpServer(err, port, bindaddr, AF_INET6, backlog);
}

int anetUnixServer(char *err, char *path, mode_t perm, int backlog)
{
#ifdef _WIN32
    ANET_NOTUSED(err);
    ANET_NOTUSED(path);
    ANET_NOTUSED(perm);
    ANET_NOTUSED(backlog);
    errno = EAFNOSUPPORT;
    return ANET_ERR;
#else
    int s;
    struct sockaddr_un sa;

    if (strlen(path) > sizeof(sa.sun_path)-1) {
        anetSetError(err,"unix socket path too long (%zu), must be under %zu", strlen(path), sizeof(sa.sun_path));
        return ANET_ERR;
    }
    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    memset(&sa,0,sizeof(sa));
    sa.sun_family = AF_LOCAL;
    redis_strlcpy(sa.sun_path,path,sizeof(sa.sun_path));
    if (anetListen(err,s,(struct sockaddr*)&sa,sizeof(sa),backlog,perm) == ANET_ERR)
        return ANET_ERR;
    return s;
#endif
}

/* Accept a connection and also make sure the socket is non-blocking, and CLOEXEC.
 * returns the new socket FD, or -1 on error. */
static int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len) {
    int fd;
    do {
        /* Use the accept4() call on linux to simultaneously accept and
         * set a socket as non-blocking. */
#if defined(HAVE_ACCEPT4) && !defined(_WIN32)
        fd = accept4(s, sa, len,  SOCK_NONBLOCK | SOCK_CLOEXEC);
#elif defined(_WIN32)
        fd = WSIOCP_Accept(s, sa, len);
#else
        fd = accept(s,sa,len);
#endif
    } while(fd == -1 && errno == EINTR);
    if (fd == -1) {
        anetSetError(err, "accept: %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return ANET_ERR;
    }
#if !defined(HAVE_ACCEPT4) || defined(_WIN32)
#ifndef _WIN32
    if (anetCloexec(fd) == -1) {
        anetSetError(err, "anetCloexec: %s", strerror(errno));
        close(fd);
        return ANET_ERR;
    }
    if (anetNonBlock(err, fd) != ANET_OK) {
        close(fd);
        return ANET_ERR;
    }
#endif
#endif
    return fd;
}

/* Accept a connection and also make sure the socket is non-blocking, and CLOEXEC.
 * returns the new socket FD, or -1 on error. */
int anetTcpAccept(char *err, int serversock, char *ip, size_t ip_len, int *port) {
    int fd;
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,serversock,(struct sockaddr*)&sa,&salen)) == ANET_ERR)
        return ANET_ERR;

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return fd;
}

/* Accept a connection and also make sure the socket is non-blocking, and CLOEXEC.
 * returns the new socket FD, or -1 on error. */
int anetUnixAccept(char *err, int s) {
#ifdef _WIN32
    ANET_NOTUSED(err);
    ANET_NOTUSED(s);
    errno = EAFNOSUPPORT;
    return ANET_ERR;
#else
    int fd;
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,s,(struct sockaddr*)&sa,&salen)) == ANET_ERR)
        return ANET_ERR;

    return fd;
#endif
}

int anetFdToString(int fd, char *ip, size_t ip_len, int *port, int remote) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (remote) {
        if (getpeername(fd, (struct sockaddr *)&sa, &salen) == -1) goto error;
    } else {
        if (getsockname(fd, (struct sockaddr *)&sa, &salen) == -1) goto error;
    }

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) {
            if (inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len) == NULL)
                goto error;
        }
        if (port) *port = ntohs(s->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) {
            if (inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len) == NULL)
                goto error;
        }
        if (port) *port = ntohs(s->sin6_port);
    }
#ifndef _WIN32
    else if (sa.ss_family == AF_UNIX) {
        if (ip) {
            int res = snprintf(ip, ip_len, "/unixsocket");
            if (res < 0 || (unsigned int) res >= ip_len) goto error;
        }
        if (port) *port = 0;
    }
#endif
    else {
        goto error;
    }
    return 0;

error:
    if (ip) {
        if (ip_len >= 2) {
            ip[0] = '?';
            ip[1] = '\0';
        } else if (ip_len == 1) {
            ip[0] = '\0';
        }
    }
    if (port) *port = 0;
    return -1;
}

/* Create a pipe buffer with given flags for read end and write end.
 * Note that it supports the file flags defined by pipe2() and fcntl(F_SETFL),
 * and one of the use cases is O_CLOEXEC|O_NONBLOCK. */
int anetPipe(int fds[2], int read_flags, int write_flags) {
#ifdef _WIN32
    /* Anonymous CRT pipes cannot participate in the IOCP event loop.  Use a
     * loopback socket pair instead; FDAPI exposes it as two Redis file
     * descriptors and keeps both handles non-inheritable. */
    if (FDAPI_pipe_for_eventloop(fds) != 0)
        return -1;

    if (read_flags & O_NONBLOCK) {
        if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) goto win_error;
    }
    if (write_flags & O_NONBLOCK) {
        if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) goto win_error;
    }
    /* O_CLOEXEC is implicit for the socket-pair handles. */
    return 0;

win_error:
    close(fds[0]);
    close(fds[1]);
    fds[0] = fds[1] = -1;
    return -1;
#else
    int pipe_flags = 0;
#ifdef HAVE_PIPE2
    /* When possible, try to leverage pipe2() to apply flags that are common to both ends.
     * There is no harm to set O_CLOEXEC to prevent fd leaks. */
    pipe_flags = O_CLOEXEC | (read_flags & write_flags);
    if (pipe2(fds, pipe_flags)) {
        /* Fail on real failures, and fallback to simple pipe if pipe2 is unsupported. */
        if (errno != ENOSYS && errno != EINVAL)
            return -1;
        pipe_flags = 0;
    } else {
        /* If the flags on both ends are identical, no need to do anything else. */
        if ((O_CLOEXEC | read_flags) == (O_CLOEXEC | write_flags))
            return 0;
        /* Clear the flags which have already been set using pipe2. */
        read_flags &= ~pipe_flags;
        write_flags &= ~pipe_flags;
    }
#endif

    /* When we reach here with pipe_flags of 0, it means pipe2 failed (or was not attempted),
     * so we try to use pipe. Otherwise, we skip and proceed to set specific flags below. */
    if (pipe_flags == 0 && pipe(fds))
        return -1;

    /* File descriptor flags.
     * Currently, only one such flag is defined: FD_CLOEXEC, the close-on-exec flag. */
    if (read_flags & O_CLOEXEC)
        if (fcntl(fds[0], F_SETFD, FD_CLOEXEC))
            goto error;
    if (write_flags & O_CLOEXEC)
        if (fcntl(fds[1], F_SETFD, FD_CLOEXEC))
            goto error;

    /* File status flags after clearing the file descriptor flag O_CLOEXEC. */
    read_flags &= ~O_CLOEXEC;
    if (read_flags)
        if (fcntl(fds[0], F_SETFL, read_flags))
            goto error;
    write_flags &= ~O_CLOEXEC;
    if (write_flags)
        if (fcntl(fds[1], F_SETFL, write_flags))
            goto error;

    return 0;

error:
    close(fds[0]);
    close(fds[1]);
    return -1;
#endif
}

int anetSetSockMarkId(char *err, int fd, uint32_t id) {
#ifdef HAVE_SOCKOPTMARKID
    if (setsockopt(fd, SOL_SOCKET, SOCKOPTMARKID, (void *)&id, sizeof(id)) == -1) {
        anetSetError(err, "setsockopt: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
#else
    UNUSED(fd);
    UNUSED(id);
    anetSetError(err,"anetSetSockMarkid unsupported on this platform");
    return ANET_OK;
#endif
}

int anetIsFifo(char *filepath) {
    struct redis_stat_type sb;
    if (redis_stat(filepath, &sb) == -1) return 0;
    return S_ISFIFO(sb.st_mode);
}

/* This function must be called after accept4() fails. It returns 1 if 'err'
 * indicates accepted connection faced an error, and it's okay to continue
 * accepting next connection by calling accept4() again. Other errors either
 * indicate programming errors, e.g. calling accept() on a closed fd or indicate
 * a resource limit has been reached, e.g. -EMFILE, open fd limit has been
 * reached. In the latter case, caller might wait until resources are available.
 * See accept4() documentation for details. */
int anetAcceptFailureNeedsRetry(int err) {
#ifdef _WIN32
    return err == WSAECONNABORTED || err == WSAECONNRESET ||
           err == WSAENETDOWN || err == WSAENETUNREACH ||
           err == WSAEHOSTUNREACH || err == WSAEPROTONOSUPPORT;
#else
    if (err == ECONNABORTED)
        return 1;

#if defined(__linux__)
    /* For details, see 'Error Handling' section on
     * https://man7.org/linux/man-pages/man2/accept.2.html */
    if (err == ENETDOWN || err == EPROTO || err == ENOPROTOOPT ||
        err == EHOSTDOWN || err == ENONET || err == EHOSTUNREACH ||
        err == EOPNOTSUPP || err == ENETUNREACH)
    {
        return 1;
    }
#endif
    return 0;
#endif
}
