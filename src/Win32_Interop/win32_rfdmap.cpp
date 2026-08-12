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

#include "win32_types.h"
#include "win32_rfdmap.h"
#include "Win32_Assert.h"
#include <new>

namespace {
class CriticalSectionGuard {
public:
    explicit CriticalSectionGuard(CRITICAL_SECTION *section)
        : section(section) {
        EnterCriticalSection(section);
    }
    ~CriticalSectionGuard() {
        LeaveCriticalSection(section);
    }
private:
    CRITICAL_SECTION *section;
};
}

RFDMap& RFDMap::getInstance() {
    static RFDMap instance; // Instantiated on first use. Guaranteed to be destroyed.
    return instance;
}

RFDMap::RFDMap() {
    InitializeCriticalSection(&mutex);
    for (int fd = FIRST_RESERVED_RFD_INDEX;
         fd <= LAST_RESERVED_RFD_INDEX; fd++) {
        CrtFDToRFDMap.emplace(fd, fd);
        RFDToCrtFDMap.emplace(fd, fd);
    }
}

RFDMap::~RFDMap() {
    DeleteCriticalSection(&mutex);
}

RFD RFDMap::getNextRFDAvailable() {
    RFD rfd = INVALID_FD;
    if (RFDRecyclePool.empty() == false) {
        rfd = RFDRecyclePool.front();
        RFDRecyclePool.pop();
    } else {
        if (next_available_rfd < INT_MAX) {
            rfd = RFDMap::next_available_rfd++;
        } else {
            rfd = INVALID_FD;
        }
    }
    return rfd;
}

void RFDMap::recycleRFD(RFD rfd) noexcept {
    if (rfd <= LAST_RESERVED_RFD_INDEX) return;
    try {
        RFDRecyclePool.push(rfd);
    } catch (...) {
        /* Recycling is only an optimization.  Do not turn close/rollback
         * into a failure if the free-descriptor queue cannot grow. */
    }
}

RFD RFDMap::addSocket(SOCKET s) {
    CriticalSectionGuard guard(&mutex);
    if (SocketToRFDMap.find(s) != SocketToRFDMap.end()) {
        errno = EEXIST;
        return INVALID_FD;
    }
    RFD rfd = getNextRFDAvailable();
    if (rfd == INVALID_FD) {
        errno = EMFILE;
        return INVALID_FD;
    }

    SocketInfo socket_info = {};
    socket_info.socket = s;
    try {
        if (!SocketToRFDMap.emplace(s, rfd).second ||
            !RFDToSocketInfoMap.emplace(rfd, socket_info).second) {
            SocketToRFDMap.erase(s);
            RFDToSocketInfoMap.erase(rfd);
            recycleRFD(rfd);
            errno = EIO;
            return INVALID_FD;
        }
    } catch (const std::bad_alloc&) {
        SocketToRFDMap.erase(s);
        RFDToSocketInfoMap.erase(rfd);
        recycleRFD(rfd);
        errno = ENOMEM;
        return INVALID_FD;
    } catch (...) {
        SocketToRFDMap.erase(s);
        RFDToSocketInfoMap.erase(rfd);
        recycleRFD(rfd);
        errno = EIO;
        return INVALID_FD;
    }
    return rfd;
}

void RFDMap::removeSocket(RFD rfd) {
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end()) return;
    if (info->second.socket != INVALID_SOCKET)
        SocketToRFDMap.erase(info->second.socket);
    RFDToSocketInfoMap.erase(info);
    recycleRFD(rfd);
}

RFD RFDMap::addCrtFD(int crt_fd) {
    CriticalSectionGuard guard(&mutex);
    auto existing = CrtFDToRFDMap.find(crt_fd);
    if (existing != CrtFDToRFDMap.end()) return existing->second;

    RFD rfd = getNextRFDAvailable();
    if (rfd == INVALID_FD) {
        errno = EMFILE;
        return INVALID_FD;
    }
    try {
        if (!CrtFDToRFDMap.emplace(crt_fd, rfd).second ||
            !RFDToCrtFDMap.emplace(rfd, crt_fd).second) {
            CrtFDToRFDMap.erase(crt_fd);
            RFDToCrtFDMap.erase(rfd);
            recycleRFD(rfd);
            errno = EIO;
            return INVALID_FD;
        }
    } catch (const std::bad_alloc&) {
        CrtFDToRFDMap.erase(crt_fd);
        RFDToCrtFDMap.erase(rfd);
        recycleRFD(rfd);
        errno = ENOMEM;
        return INVALID_FD;
    } catch (...) {
        CrtFDToRFDMap.erase(crt_fd);
        RFDToCrtFDMap.erase(rfd);
        recycleRFD(rfd);
        errno = EIO;
        return INVALID_FD;
    }
    return rfd;
}

void RFDMap::removeCrtFD(int crt_fd) {
    // crt_fd between FIRST_RESERVED_RFD_INDEX and LAST_RESERVED_RFD_INDEX
    // should never be removed.
    ASSERT(FIRST_RESERVED_RFD_INDEX == 0);
    if (crt_fd > RFDMap::LAST_RESERVED_RFD_INDEX) {
        CriticalSectionGuard guard(&mutex);
        map<int, RFD>::iterator mit = CrtFDToRFDMap.find(crt_fd);
        if (mit != CrtFDToRFDMap.end()) {
            RFD rfd = (*mit).second;
            recycleRFD(rfd);
            RFDToCrtFDMap.erase(rfd);
            CrtFDToRFDMap.erase(crt_fd);
        }
    }
}

SOCKET RFDMap::lookupSocket(RFD rfd) {
    SOCKET socket = INVALID_SOCKET;
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info != RFDToSocketInfoMap.end()) socket = info->second.socket;
    return socket;
}

bool RFDMap::getSocketInfo(RFD rfd, SocketInfo *socket_info) {
    if (socket_info == NULL) return false;
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end()) return false;
    *socket_info = info->second;
    return true;
}

bool RFDMap::saveSocketAddrStorage(
    RFD rfd, const SOCKADDR_STORAGE *socket_addr) {
    if (socket_addr == NULL) return false;
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end()) return false;
    info->second.socketAddrStorage = *socket_addr;
    return true;
}

bool RFDMap::setSocketFlags(RFD rfd, SOCKET socket, int flags) {
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end() || info->second.socket != socket)
        return false;
    info->second.flags = flags;
    return true;
}

bool RFDMap::markSocketClosed(RFD rfd, SOCKET *socket, void **state) {
    if (socket == NULL || state == NULL) return false;
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end() ||
        info->second.socket == INVALID_SOCKET)
        return false;
    *socket = info->second.socket;
    *state = info->second.state;
    SocketToRFDMap.erase(*socket);
    info->second.socket = INVALID_SOCKET;
    return true;
}

bool RFDMap::getSocketState(RFD rfd, void **state) {
    if (state == NULL) return false;
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end()) return false;
    *state = info->second.state;
    return true;
}

bool RFDMap::installSocketState(RFD rfd, void *state, void **actual_state) {
    if (state == NULL || actual_state == NULL) return false;
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end() ||
        info->second.socket == INVALID_SOCKET)
        return false;
    if (info->second.state == NULL) info->second.state = state;
    *actual_state = info->second.state;
    return true;
}

bool RFDMap::clearSocketState(RFD rfd, void *expected_state) {
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToSocketInfoMap.find(rfd);
    if (info == RFDToSocketInfoMap.end() ||
        info->second.state != expected_state)
        return false;
    info->second.state = NULL;
    return true;
}

int RFDMap::lookupCrtFD(RFD rfd) {
    int crt_fd = INVALID_FD;
    CriticalSectionGuard guard(&mutex);
    auto info = RFDToCrtFDMap.find(rfd);
    if (info != RFDToCrtFDMap.end()) crt_fd = info->second;
    return crt_fd;
}
