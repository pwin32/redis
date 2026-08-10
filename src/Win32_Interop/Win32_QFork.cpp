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

 /*
 Redis is an in memory DB. We need to share the redis database with a quasi-forked process so that we can do the RDB and AOF saves
 without halting the main redis process, or crashing due to code that was never designed to be thread safe. Essentially we need to
 replicate the COW behavior of fork() on Windows, but we don't actually need a complete fork() implementation. A complete fork()
 implementation would require subsystem level support to make happen. The following is required to make this quasi-fork scheme work:

 DLMallocMemoryMap:
 - An uncomitted memory map whose size is the total physical memory on the system less some memory for the rest of the system so that
 we avoid excessive swapping.
 - This is reserved high in VM space so that it can be mapped at a specific address in the child qforked process (ASLR must be
 disabled for these processes)
 - This must be mapped in exactly the same virtual memory space in both forker and forkee.

 QForkControlMemoryMap:
 - contains a map of the allocated segments in the DLMallocMemoryMap
 - contains handles for inter-process synchronization
 - contains pointers to some of the global data in the parent process if mapped into DLMallocMemoryMap, and a copy of any other
 required global data

 QFork process:
 - a copy of the parent process with a command line specifying QFork behavior
 - when a COW operation is requested via an event signal
 - opens the DLMAllocMemoryMap with PAGE_WRITECOPY
 - reserve space for DLMAllocMemoryMap at the memory location specified in ControlMemoryMap
 - locks the DLMalloc segments as specified in QForkControlMemoryMap
 - maps global data from the QForkControlMEmoryMap into this process
 - executes the requested operation
 - unmaps all the mm views (discarding any writes)
 - signals the parent when the operation is complete

 How the parent invokes the QFork process:
 - protects mapped memory segments with VirtualProtect using PAGE_WRITECOPY (both the allocated portions of DLMAllocMemoryMap and
 the QForkControlMemoryMap)
 - QForked process is signaled to process command
 - Parent waits (asynchronously) until QForked process signals that operation is complete, then as an atomic operation:
 - signals and waits for the forked process to terminate
 - resotres protection status on mapped blocks
 - determines which pages have been modified and copies these to a buffer
 - unmaps the view of the heap (discarding COW changes form the view)
 - remaps the view
 - copies the changes back into the view
 */

 /*
 Not specifying the maxmemory flag will result in the default behavior of: new key generation not bounded by heap usage,
 and the heap size equal to the size of physical memory.

 Redis will respect the maxmemory flag by preventing new key creation when the number of bytes allocated in the heap
 exceeds the level specified by the maxmemory flag. This does not account for heap fragmentation or memory usage by
 the heap allocator. To allow for this extra space we allow the heap to allocate 10 times the physical memory.

 Since the heap is entirely contained in the system paging file, the size of the system paging file needs to be large accordingly.

 During forking the system paging file is used for managing virtual memory sharing and the copy on write pages for both
 forker and forkee. There must be sufficient system paging space availability for this. By default Windows will dynamically
 allocate a system paging file that will expand up to about (3.5 * physical).
 */

#include "win32_types.h"
#include "Win32_FDAPI.h"
#include "Win32_Common.h"
#include "Win32_Assert.h"
#include "Win32_Error.h"

#include <Windows.h>
#include <WinNT.h>
#ifdef __MINGW32__
typedef unsigned char byte;
#endif
#include <errno.h>
#include <Psapi.h>
#include <iostream>

#define QFORK_MAIN_IMPL
#include "Win32_QFork.h"
#include "Win32_QFork_impl.h"
#include "Win32_SmartHandle.h"
#include "Win32_Service.h"
#include "Win32_CommandLine.h"
#include "Win32_RedisLog.h"
#include "Win32_StackTrace.h"
#include "Win32_ThreadControl.h"
#include "Win32_EventLog.h"

#include <jemalloc/jemalloc.h>
#include <jemalloc/internal/jemalloc_internal_defs.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace std;

extern "C" {
BOOL g_IsForkedProcess = FALSE;
int moduleForEachForkModule(
    int (*callback)(const char *, void *, const wchar_t *, uint64_t, void *),
    void *privdata);
size_t moduleCount(void);
void moduleSetQForkChildReady(int ready);
size_t RedisSharedForkDataSize(void);
BOOL RedisCopySharedForkData(void *data, size_t size);
void RedisGetCoreForkData(RedisCoreForkData *data);
}

//#define DEBUG_WITH_PROCMON
#ifdef DEBUG_WITH_PROCMON
#define FILE_DEVICE_PROCMON_LOG 0x00009535
#define IOCTL_EXTERNAL_LOG_DEBUGOUT (ULONG) CTL_CODE( FILE_DEVICE_PROCMON_LOG, 0x81, METHOD_BUFFERED, FILE_WRITE_ACCESS )

HANDLE hProcMonDevice = INVALID_HANDLE_VALUE;
BOOL WriteToProcmon(wstring message)
{
    if (hProcMonDevice != INVALID_HANDLE_VALUE) {
        DWORD nb = 0;
        return DeviceIoControl(
            hProcMonDevice,
            IOCTL_EXTERNAL_LOG_DEBUGOUT,
            (LPVOID) (message.c_str()),
            (DWORD) (message.length() * sizeof(wchar_t)),
            NULL,
            0,
            &nb,
            NULL);
    }
    else {
        return FALSE;
    }
}
#endif

#ifndef PAGE_REVERT_TO_FILE_MAP
#define PAGE_REVERT_TO_FILE_MAP 0x80000000  // From Win8.1 SDK
#endif

#ifndef IMAGE_DLLCHARACTERISTICS_GUARD_CF
#define IMAGE_DLLCHARACTERISTICS_GUARD_CF 0x4000
#endif

#define IFFAILTHROW(a,m) if(!(a)) { throw system_error(GetLastError(), system_category(), m); }

static wstring QForkUtf8ToWide(const char *value) {
    wchar_t *wide = win32_utf8_to_wide(value);
    if (wide == NULL) {
        throw system_error(errno, generic_category(),
                           "QFork UTF-8 to UTF-16 conversion failed");
    }
    wstring result(wide);
    win32_free(wide);
    return result;
}

static void QForkAppendQuotedArgument(wstring& commandLine,
                                      const wstring& argument) {
    size_t backslashes = 0;

    commandLine.push_back(L'"');
    for (wchar_t c : argument) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        if (c == L'"') {
            commandLine.append(backslashes * 2 + 1, L'\\');
            commandLine.push_back(L'"');
            backslashes = 0;
            continue;
        }
        commandLine.append(backslashes, L'\\');
        backslashes = 0;
        commandLine.push_back(c);
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine.push_back(L'"');
}

static wstring QForkBuildCommandLine(const vector<wstring>& arguments) {
    wstring commandLine;
    for (size_t index = 0; index < arguments.size(); index++) {
        if (index != 0) commandLine.push_back(L' ');
        QForkAppendQuotedArgument(commandLine, arguments[index]);
    }
    return commandLine;
}

/* redisServer grew substantially between 6.2 and 7.2.  Keep the copied
 * process-static server image bounded, but leave enough headroom for future
 * 7.2 maintenance fields; CopyForkOperationData still fails closed if the
 * live structure ever exceeds this contract. */
#define MAX_REDIS_DATA_SIZE (64 * 1024)
#define MAX_RDB_SAVE_INFO_SIZE 128
/* A Windows path may contain up to 32,767 UTF-16 code units. Redis stores the
 * logical path as UTF-8, whose worst-case representation uses four bytes per
 * code point, so the shared QFork control block must not retain MAX_PATH. */
#define MAX_QFORK_FILENAME_SIZE (32767 * 4 + 1)
struct QForkInfo {
    BYTE redisData[MAX_REDIS_DATA_SIZE];
    size_t redisDataSize;
    uint8_t dictHashSeed[16];
    RedisACLForkData acl;
    RedisCoreForkData core;
    size_t sharedDataSize;
    BYTE sharedData[REDIS_QFORK_MAX_SHARED_DATA_SIZE];
    RedisModuleForkData modules;
    HANDLE moduleSnapshotMap;
    uint64_t moduleSnapshotSize;
    uint32_t moduleSnapshotCount;
    uint32_t moduleSnapshotReserved;
    size_t usedMemory;
    char filename[MAX_QFORK_FILENAME_SIZE];
    int rdb_req;
    int rdb_flags;
    size_t rdb_save_info_size;
    BYTE rdb_save_info[MAX_RDB_SAVE_INFO_SIZE];
    int rdb_pipe_write_fd;
    WSAPROTOCOL_INFOW rdb_pipe_write_protocol_info;
    int rdb_child_exit_pipe_read_fd;
    WSAPROTOCOL_INFOW rdb_child_exit_pipe_read_protocol_info;
};

extern "C"
{
    int checkForSentinelMode(int argc, char **argv, char *exec_name);
    void InitTimeFunctions();
    PORT_LONGLONG memtoll(const char *p, int *err);     // Forward def from util.h
    size_t zmalloc_used_memory(void);
}

const size_t cAllocationGranularity = 1 << LG_PAGE;    // 4MB per heap block (matches the default allocation threshold of jemalloc)
#ifdef _WIN64
const int  cMaxBlocks = 1 << (40 - LG_PAGE);                // 4MB * 256K heap blocks = 1TB
#else
const int  cMaxBlocks = 1 << 8;                 // 4MB * 256 heap blocks = 1GB
#endif

const int cDeadForkWait = 30000;

enum class BlockState : uint8_t { bsINVALID = 0, bsUNMAPPED = 1, bsMAPPED_IN_USE = 2, bsMAPPED_FREE = 3 };

struct heapBlockInfo {
    HANDLE heapMap;
    BlockState state;
};

struct QForkControl {
    LPVOID heapStart;
    LPVOID heapEnd;
    int maxAvailableBlocks;
    int numMappedBlocks;
    int blockSearchStart;
    heapBlockInfo heapBlockList[cMaxBlocks];

    OperationType typeOfOperation;
    HANDLE operationComplete;
    HANDLE operationFailed;

    // Global data pointers to be passed to the forked process
    QForkInfo globalData;
};

QForkControl* g_pQForkControl;
HANDLE g_hQForkControlFileMap;
HANDLE g_hQForkModuleSnapshotMap;
HANDLE g_hForkedProcess = 0;
vector<HMODULE> g_QForkPinnedModules;
vector<HANDLE> g_QForkPinnedModuleFiles;
int g_ChildExitCode = 0; // For child process
BOOL g_SentinelMode;
BOOL g_PersistenceDisabled;
/* If g_IsForkedProcess || g_PersistenceDisabled || g_SentinelMode is true
 * memory is not allocated from the memory map heap, instead the system heap
 * is used */
BOOL g_BypassMemoryMapOnAlloc;
/* g_HasMemoryMappedHeap is true if g_PersistenceDisabled and g_SentinelMode
 * are both false, so it is true for the parent process and the child process
 * when persistence is available */
BOOL g_HasMemoryMappedHeap;
BOOL g_QForkHeapReady;
//[tporadowski/#2]
BOOL g_StartedAsCheckAofOrRdbTool;

/* Jemalloc initializes before the QFork heap exists. The main thread is moved
 * to a fresh manual arena once QFork is ready, but every newly created thread
 * starts with independent jemalloc TSD and may otherwise auto-select an arena
 * that still owns pre-QFork extents. Keep the selected arena process-local and
 * bind each Redis allocator thread once per arena generation. */
static unsigned g_QForkJemallocArena;
static volatile LONG g_QForkJemallocArenaGeneration;
#ifdef __MINGW32__
static __thread LONG g_QForkThreadArenaGeneration;
#else
static __declspec(thread) LONG g_QForkThreadArenaGeneration;
#endif

/* Jemalloc can call the process-wide heap hooks from different arenas and
 * worker threads. Keep this lock process-local: QForkControl is copied into
 * the child, while an SRW lock must never be shared that way. */
static SRWLOCK g_QForkHeapLock = SRWLOCK_INIT;

class QForkHeapLockGuard {
public:
    QForkHeapLockGuard() {
        AcquireSRWLockExclusive(&g_QForkHeapLock);
    }

    ~QForkHeapLockGuard() {
        ReleaseSRWLockExclusive(&g_QForkHeapLock);
    }

private:
    QForkHeapLockGuard(const QForkHeapLockGuard&);
    QForkHeapLockGuard& operator=(const QForkHeapLockGuard&);
};

static void FlushJemallocThreadCache(const char* context) {
    int err = je_mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);
    if (err != 0 && err != EFAULT) {
        throw system_error(err, generic_category(), context);
    }
}

int QForkEnsureCurrentThreadJemallocArena() {
    LONG generation = g_QForkJemallocArenaGeneration;

    if (generation == 0 ||
        g_QForkThreadArenaGeneration == generation ||
        g_IsForkedProcess ||
        g_BypassMemoryMapOnAlloc ||
        !g_QForkHeapReady ||
        g_pQForkControl == NULL) {
        return 0;
    }

    int err = je_mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);
    if (err != 0 && err != EFAULT) return err;

    unsigned arena = g_QForkJemallocArena;
    err = je_mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena));
    if (err != 0) return err;

    err = je_mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);
    if (err != 0 && err != EFAULT) return err;

    g_QForkThreadArenaGeneration = generation;
    return 0;
}

BOOL QForkIsInheritedHeapAddress(const void *ptr) {
    if (ptr == NULL || !g_IsForkedProcess || !g_QForkHeapReady ||
        g_pQForkControl == NULL)
        return FALSE;

    uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t heapStart = reinterpret_cast<uintptr_t>(
        g_pQForkControl->heapStart);
    uintptr_t heapEnd = reinterpret_cast<uintptr_t>(
        g_pQForkControl->heapEnd);
    if (address < heapStart || address >= heapEnd)
        return FALSE;

    /* Child-private VirtualAlloc extents can occupy an unused address inside
     * the reserved range.  Only the inherited MapViewOfFile view is owned by
     * the parent snapshot and must be left untouched. */
    MEMORY_BASIC_INFORMATION memInfo;
    if (!VirtualQuery(ptr, &memInfo, sizeof(memInfo)))
        return FALSE;
    return memInfo.Type == MEM_MAPPED;
}

static void SwitchToQForkJemallocArena() {
    unsigned arena = 0;
    size_t arenaSize = sizeof(arena);

    FlushJemallocThreadCache("QForkMasterInit: could not flush startup jemalloc thread cache");

    int err = je_mallctl("arenas.create", &arena, &arenaSize, NULL, 0);
    if (err != 0) {
        throw system_error(err, generic_category(), "QForkMasterInit: could not create QFork jemalloc arena");
    }

    g_QForkJemallocArena = arena;
    _InterlockedIncrement(&g_QForkJemallocArenaGeneration);

    err = QForkEnsureCurrentThreadJemallocArena();
    if (err != 0) {
        throw system_error(err, generic_category(), "QForkMasterInit: could not set QFork jemalloc arena");
    }
}

bool ReportSpecialSystemErrors(int error) {
    switch (error)
    {
    case ERROR_NO_SYSTEM_RESOURCES:
    case ERROR_COMMITMENT_LIMIT:
    {
        serverLog(
            LL_WARNING,
            "\n"
            "The Windows version of Redis reserves heap memory from the system paging file\n"
            "for sharing with the forked process used for persistence operations."
            "At this time there is insufficient contiguous free space available in the\n"
            "system paging file. You may increase the size of the system paging file.\n"
            "Sometimes a reboot will defragment the system paging file sufficiently for\n"
            "this operation to complete successfully.\n"
            "\n"
            "Redis can not continue. Exiting."
        );
        RedisEventLog().LogError("Failed to reserves heap memory from the system paging file.");
        return true;
    }
    default:
        return false;
    }
}

struct QForkModuleDescriptor {
    string name;
    wstring path;
    HMODULE expectedBase;
    uint64_t sequence;
};

struct QForkModuleFileIdentity {
    DWORD volumeSerialNumber;
    DWORD fileIndexHigh;
    DWORD fileIndexLow;
    uint64_t fileSize;
    uint64_t lastWriteTime;
};

struct QForkModuleProtectedRange {
    DWORD rva;
    size_t size;
};

struct QForkModuleImageRange {
    byte *address;
    DWORD rva;
    DWORD size;
    DWORD characteristics;
    WORD sectionIndex;
    char name[IMAGE_SIZEOF_SHORT_NAME];
    uint64_t snapshotOffset;
};

struct QForkModuleImageInfo {
    DWORD imageSize;
    DWORD sizeOfHeaders;
    DWORD timeDateStamp;
    DWORD checkSum;
    WORD machine;
    WORD optionalMagic;
    WORD numberOfSections;
    QForkModuleFileIdentity fileIdentity;
    vector<QForkModuleProtectedRange> protectedRanges;
    vector<QForkModuleImageRange> writableRanges;
};

struct QForkCapturedModule {
    QForkModuleDescriptor descriptor;
    QForkModuleImageInfo image;
    uint64_t nameOffset;
    uint64_t pathOffset;
    uint32_t firstRange;
};

static const uint32_t cQForkModuleSnapshotMagic = 0x534d4651; /* QFMS */
static const uint16_t cQForkModuleSnapshotVersion = 1;
static const uint64_t cQForkModuleTlsTemplateSize = sizeof(uintptr_t);
static const size_t cQForkModuleMaxTlsCallbacks = 32;
/* The PE32+ load-config fields from GuardCFCheckFunctionPointer onward are
 * loader/security metadata. Reject any nonzero extension rather than copying
 * parent-process pointer state over the child loader's values. */
static const uint64_t cQForkModuleLoadConfigGuardOffset = 112;

struct QForkModuleSnapshotHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t headerSize;
    uint32_t moduleCount;
    uint32_t rangeCount;
    uint64_t totalSize;
    uint64_t modulesOffset;
    uint64_t rangesOffset;
    uint64_t payloadOffset;
};

struct QForkModuleSnapshotRecord {
    uint64_t sequence;
    uint64_t expectedBase;
    uint64_t nameOffset;
    uint64_t pathOffset;
    uint32_t nameBytes;
    uint32_t pathChars;
    uint32_t firstRange;
    uint32_t rangeCount;
    uint32_t imageSize;
    uint32_t sizeOfHeaders;
    uint32_t timeDateStamp;
    uint32_t checkSum;
    uint16_t machine;
    uint16_t optionalMagic;
    uint16_t numberOfSections;
    uint16_t reserved;
    uint32_t volumeSerialNumber;
    uint32_t fileIndexHigh;
    uint32_t fileIndexLow;
    uint32_t fileIdentityReserved;
    uint64_t fileSize;
    uint64_t lastWriteTime;
};

struct QForkModuleSnapshotRange {
    uint64_t bytesOffset;
    uint32_t rva;
    uint32_t size;
    uint32_t characteristics;
    uint16_t sectionIndex;
    uint16_t reserved;
    char name[IMAGE_SIZEOF_SHORT_NAME];
};

static int CollectQForkModule(const char *name, void *handle,
    const wchar_t *path, uint64_t sequence, void *privdata)
{
    try {
        vector<QForkModuleDescriptor> *modules =
            reinterpret_cast<vector<QForkModuleDescriptor> *>(privdata);
        QForkModuleDescriptor descriptor;
        descriptor.name = name;
        descriptor.path = path;
        descriptor.expectedBase = reinterpret_cast<HMODULE>(handle);
        descriptor.sequence = sequence;
        modules->push_back(descriptor);
        return 0;
    }
    catch (...) {
        return -1;
    }
}

static bool QForkModuleRangeInsideImage(uint64_t rva, uint64_t size,
    uint64_t imageSize)
{
    return rva <= imageSize && size <= imageSize - rva;
}

static bool QForkModuleRangeInsideRange(uint64_t rva, uint64_t size,
    uint64_t outerRva, uint64_t outerSize)
{
    return rva >= outerRva && rva - outerRva <= outerSize &&
           size <= outerSize - (rva - outerRva);
}

static bool QForkCheckedAdd(uint64_t left, uint64_t right, uint64_t *result)
{
    if (right > numeric_limits<uint64_t>::max() - left) return false;
    *result = left + right;
    return true;
}

static bool QForkCheckedMultiply(uint64_t left, uint64_t right,
    uint64_t *result)
{
    if (left != 0 && right > numeric_limits<uint64_t>::max() / left)
        return false;
    *result = left * right;
    return true;
}

static bool QForkCheckedAlign(uint64_t value, uint64_t alignment,
    uint64_t *result)
{
    if (alignment == 0) return false;
    uint64_t remainder = value % alignment;
    if (remainder == 0) {
        *result = value;
        return true;
    }
    return QForkCheckedAdd(value, alignment - remainder, result);
}

static bool AddQForkModuleProtectedRange(
    vector<QForkModuleProtectedRange>& ranges,
    uint64_t rva, uint64_t size, uint64_t imageSize)
{
    if (size == 0 || !QForkModuleRangeInsideImage(rva, size, imageSize) ||
        rva > MAXDWORD || size > MAXDWORD)
        return false;

    QForkModuleProtectedRange range;
    range.rva = static_cast<DWORD>(rva);
    range.size = static_cast<size_t>(size);
    ranges.push_back(range);
    return true;
}

static void RestoreQForkModuleProtectedRanges(byte *imageBase,
    DWORD sectionRva, vector<byte>& childBytes,
    const vector<QForkModuleProtectedRange>& protectedRanges)
{
    uint64_t sectionStart = sectionRva;
    uint64_t sectionEnd = sectionStart + childBytes.size();

    for (const QForkModuleProtectedRange& range : protectedRanges) {
        uint64_t protectedStart = range.rva;
        uint64_t protectedEnd = protectedStart + range.size;
        uint64_t copyStart = max(sectionStart, protectedStart);
        uint64_t copyEnd = min(sectionEnd, protectedEnd);
        if (copyStart >= copyEnd) continue;

        memcpy(imageBase + copyStart,
               childBytes.data() + (copyStart - sectionStart),
               static_cast<size_t>(copyEnd - copyStart));
    }
}

static bool GetQForkModuleFileIdentity(HANDLE file,
    QForkModuleFileIdentity& identity, const char *name)
{
    BY_HANDLE_FILE_INFORMATION info;
    BOOL ok = GetFileInformationByHandle(file, &info);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok) {
        serverLog(LL_WARNING,
            "QFork module validation: could not identify %s image (0x%08x)",
            name, error);
        return false;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        serverLog(LL_WARNING,
            "QFork module validation: %s image path is a directory", name);
        return false;
    }

    ULARGE_INTEGER size;
    size.LowPart = info.nFileSizeLow;
    size.HighPart = info.nFileSizeHigh;
    ULARGE_INTEGER writeTime;
    writeTime.LowPart = info.ftLastWriteTime.dwLowDateTime;
    writeTime.HighPart = info.ftLastWriteTime.dwHighDateTime;
    identity.volumeSerialNumber = info.dwVolumeSerialNumber;
    identity.fileIndexHigh = info.nFileIndexHigh;
    identity.fileIndexLow = info.nFileIndexLow;
    identity.fileSize = size.QuadPart;
    identity.lastWriteTime = writeTime.QuadPart;
    return true;
}

static HANDLE OpenQForkModuleFile(const wchar_t *path,
    wstring& canonicalPath, const char *name)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        serverLog(LL_WARNING,
            "QFork module validation: could not lock %s image (0x%08x)",
            name, GetLastError());
        return NULL;
    }

    if (GetFileType(file) != FILE_TYPE_DISK) {
        serverLog(LL_WARNING,
            "QFork module validation: %s image is not a disk file", name);
        CloseHandle(file);
        return NULL;
    }

    try {
        DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
        DWORD required = GetFinalPathNameByHandleW(file, NULL, 0, flags);
        if (required == 0 || required == MAXDWORD) {
            DWORD error = required == 0 ? GetLastError() :
                ERROR_INSUFFICIENT_BUFFER;
            serverLog(LL_WARNING,
                "QFork module validation: could not resolve %s image path (0x%08x)",
                name, error);
            CloseHandle(file);
            return NULL;
        }

        vector<wchar_t> pathBuffer(static_cast<size_t>(required) + 1);
        DWORD copied = GetFinalPathNameByHandleW(file, pathBuffer.data(),
            static_cast<DWORD>(pathBuffer.size()), flags);
        if (copied == 0 || copied >= pathBuffer.size()) {
            DWORD error = copied == 0 ? GetLastError() :
                ERROR_INSUFFICIENT_BUFFER;
            serverLog(LL_WARNING,
                "QFork module validation: could not canonicalize %s image path (0x%08x)",
                name, error);
            CloseHandle(file);
            return NULL;
        }

        canonicalPath.assign(pathBuffer.data(), copied);
        return file;
    }
    catch (...) {
        CloseHandle(file);
        throw;
    }
}

static bool QForkModuleFileIdentityEqual(
    const QForkModuleFileIdentity& left,
    const QForkModuleFileIdentity& right)
{
    return left.volumeSerialNumber == right.volumeSerialNumber &&
           left.fileIndexHigh == right.fileIndexHigh &&
           left.fileIndexLow == right.fileIndexLow &&
           left.fileSize == right.fileSize &&
           left.lastWriteTime == right.lastWriteTime;
}

static bool GetQForkModuleHeaders(HMODULE module, const char *name,
    PIMAGE_NT_HEADERS64 *ntOut, DWORD *imageSizeOut)
{
    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), module, &moduleInfo,
                              sizeof(moduleInfo)))
    {
        serverLog(LL_WARNING,
            "QFork module validation: GetModuleInformation failed for %s (0x%08x)",
            name, GetLastError());
        return false;
    }

    byte *imageBase = reinterpret_cast<byte *>(moduleInfo.lpBaseOfDll);
    PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(imageBase);
    if (moduleInfo.lpBaseOfDll != module ||
        moduleInfo.SizeOfImage < sizeof(*dos) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        moduleInfo.SizeOfImage < sizeof(IMAGE_NT_HEADERS64) ||
        static_cast<uint64_t>(dos->e_lfanew) >
            moduleInfo.SizeOfImage - sizeof(IMAGE_NT_HEADERS64))
    {
        serverLog(LL_WARNING,
            "QFork module validation: invalid DOS header for %s", name);
        return false;
    }

    PIMAGE_NT_HEADERS64 nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
        imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage != moduleInfo.SizeOfImage ||
        nt->OptionalHeader.NumberOfRvaAndSizes >
            IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        nt->OptionalHeader.SizeOfHeaders > moduleInfo.SizeOfImage ||
        nt->FileHeader.NumberOfSections == 0)
    {
        serverLog(LL_WARNING,
            "QFork module validation: unsupported PE image for %s", name);
        return false;
    }

    uint64_t sectionTableOffset = static_cast<uint64_t>(dos->e_lfanew) +
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt->FileHeader.SizeOfOptionalHeader;
    uint64_t sectionTableSize;
    uint64_t sectionTableEnd;
    if (!QForkCheckedMultiply(nt->FileHeader.NumberOfSections,
                             sizeof(IMAGE_SECTION_HEADER),
                             &sectionTableSize) ||
        !QForkCheckedAdd(sectionTableOffset, sectionTableSize,
                         &sectionTableEnd) ||
        sectionTableEnd > nt->OptionalHeader.SizeOfHeaders ||
        sectionTableEnd > moduleInfo.SizeOfImage)
    {
        serverLog(LL_WARNING,
            "QFork module validation: invalid section table for %s", name);
        return false;
    }

    *ntOut = nt;
    *imageSizeOut = moduleInfo.SizeOfImage;
    return true;
}

static IMAGE_DATA_DIRECTORY GetQForkModuleDataDirectory(
    PIMAGE_NT_HEADERS64 nt, DWORD index)
{
    IMAGE_DATA_DIRECTORY empty = { 0, 0 };
    if (index >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        index >= nt->OptionalHeader.NumberOfRvaAndSizes)
        return empty;
    return nt->OptionalHeader.DataDirectory[index];
}

static bool ValidateQForkModuleImportThunks(HMODULE module, DWORD imageSize,
    const IMAGE_DATA_DIRECTORY& imports, const IMAGE_DATA_DIRECTORY& iat,
    const char *name)
{
    bool importsPresent = imports.VirtualAddress != 0 || imports.Size != 0;
    bool iatPresent = iat.VirtualAddress != 0 || iat.Size != 0;
    if (!importsPresent) return true;

    if (imports.VirtualAddress == 0 || imports.Size == 0 ||
        iat.VirtualAddress == 0 || iat.Size == 0 ||
        imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !QForkModuleRangeInsideImage(imports.VirtualAddress, imports.Size,
                                     imageSize) ||
        !QForkModuleRangeInsideImage(iat.VirtualAddress, iat.Size, imageSize))
    {
        serverLog(LL_WARNING,
            "QFork module validation: invalid import/IAT directory in %s",
            name);
        return false;
    }

    byte *imageBase = reinterpret_cast<byte *>(module);
    size_t descriptorCount = imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    bool descriptorsTerminated = false;
    for (size_t i = 0; i < descriptorCount; i++) {
        IMAGE_IMPORT_DESCRIPTOR descriptor;
        memcpy(&descriptor,
            imageBase + imports.VirtualAddress +
                i * sizeof(IMAGE_IMPORT_DESCRIPTOR),
            sizeof(descriptor));

        if (descriptor.OriginalFirstThunk == 0 &&
            descriptor.TimeDateStamp == 0 &&
            descriptor.ForwarderChain == 0 && descriptor.Name == 0 &&
            descriptor.FirstThunk == 0)
        {
            descriptorsTerminated = true;
            break;
        }

        if (descriptor.FirstThunk == 0 ||
            descriptor.FirstThunk % sizeof(IMAGE_THUNK_DATA64) != 0 ||
            !QForkModuleRangeInsideRange(descriptor.FirstThunk,
                sizeof(IMAGE_THUNK_DATA64), iat.VirtualAddress, iat.Size))
        {
            serverLog(LL_WARNING,
                "QFork module validation: import thunk outside IAT in %s",
                name);
            return false;
        }

        uint64_t available = static_cast<uint64_t>(iat.VirtualAddress) +
            iat.Size - descriptor.FirstThunk;
        size_t thunkCount = static_cast<size_t>(
            available / sizeof(IMAGE_THUNK_DATA64));
        bool thunksTerminated = false;
        for (size_t j = 0; j < thunkCount; j++) {
            IMAGE_THUNK_DATA64 thunk;
            memcpy(&thunk,
                imageBase + descriptor.FirstThunk +
                    j * sizeof(IMAGE_THUNK_DATA64),
                sizeof(thunk));
            if (thunk.u1.Function == 0) {
                thunksTerminated = true;
                break;
            }
        }
        if (!thunksTerminated) {
            serverLog(LL_WARNING,
                "QFork module validation: unterminated import thunk array in %s",
                name);
            return false;
        }
    }

    if (!descriptorsTerminated) {
        serverLog(LL_WARNING,
            "QFork module validation: unterminated import directory in %s",
            name);
        return false;
    }
    return iatPresent;
}

static bool InspectQForkModuleImage(HMODULE module, HANDLE moduleFile,
    const char *name, QForkModuleImageInfo& image)
{
    const char *safeName = name != NULL ? name : "unknown";
    if (module == NULL || moduleFile == NULL ||
        moduleFile == INVALID_HANDLE_VALUE)
    {
        serverLog(LL_WARNING,
            "QFork module validation: missing image handle for %s", safeName);
        return false;
    }

    PIMAGE_NT_HEADERS64 nt;
    DWORD imageSize;
    if (!GetQForkModuleHeaders(module, safeName, &nt, &imageSize) ||
        !GetQForkModuleFileIdentity(moduleFile, image.fileIdentity, safeName))
        return false;

    image.imageSize = imageSize;
    image.sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    image.timeDateStamp = nt->FileHeader.TimeDateStamp;
    image.checkSum = nt->OptionalHeader.CheckSum;
    image.machine = nt->FileHeader.Machine;
    image.optionalMagic = nt->OptionalHeader.Magic;
    image.numberOfSections = nt->FileHeader.NumberOfSections;

    IMAGE_DATA_DIRECTORY delayImports = GetQForkModuleDataDirectory(nt,
        IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (delayImports.VirtualAddress != 0 || delayImports.Size != 0) {
        serverLog(LL_WARNING,
            "QFork module validation: delay imports are unsupported in %s",
            safeName);
        return false;
    }

    uint64_t base = reinterpret_cast<uint64_t>(module);

    IMAGE_DATA_DIRECTORY imports = GetQForkModuleDataDirectory(nt,
        IMAGE_DIRECTORY_ENTRY_IMPORT);
    IMAGE_DATA_DIRECTORY iat = GetQForkModuleDataDirectory(nt,
        IMAGE_DIRECTORY_ENTRY_IAT);
    if (iat.VirtualAddress != 0 || iat.Size != 0) {
        if (iat.VirtualAddress == 0 || iat.Size == 0 ||
            !AddQForkModuleProtectedRange(image.protectedRanges,
                iat.VirtualAddress, iat.Size, imageSize))
        {
            serverLog(LL_WARNING,
                "QFork module validation: invalid IAT range in %s", safeName);
            return false;
        }
    }
    if (!ValidateQForkModuleImportThunks(module, imageSize, imports, iat,
                                          safeName))
        return false;

    if (nt->OptionalHeader.DllCharacteristics &
        IMAGE_DLLCHARACTERISTICS_GUARD_CF)
    {
        serverLog(LL_WARNING,
            "QFork module validation: CFG/XFG is unsupported in %s",
            safeName);
        return false;
    }

    IMAGE_DATA_DIRECTORY tlsDirectory = GetQForkModuleDataDirectory(nt,
        IMAGE_DIRECTORY_ENTRY_TLS);
    if (tlsDirectory.VirtualAddress != 0 || tlsDirectory.Size != 0) {
        if (tlsDirectory.VirtualAddress == 0 ||
            tlsDirectory.Size < sizeof(IMAGE_TLS_DIRECTORY64) ||
            !QForkModuleRangeInsideImage(tlsDirectory.VirtualAddress,
                sizeof(IMAGE_TLS_DIRECTORY64), imageSize))
        {
            serverLog(LL_WARNING,
                "QFork module validation: invalid TLS directory in %s",
                safeName);
            return false;
        }
        PIMAGE_TLS_DIRECTORY64 tls =
            reinterpret_cast<PIMAGE_TLS_DIRECTORY64>(
                reinterpret_cast<byte *>(module) + tlsDirectory.VirtualAddress);
        uint64_t tlsTemplateRva = tls->StartAddressOfRawData - base;
        if (tls->StartAddressOfRawData < base ||
            tls->EndAddressOfRawData < tls->StartAddressOfRawData ||
            tls->EndAddressOfRawData - tls->StartAddressOfRawData !=
                cQForkModuleTlsTemplateSize ||
            tls->SizeOfZeroFill != 0 ||
            !AddQForkModuleProtectedRange(image.protectedRanges,
                tlsTemplateRva,
                tls->EndAddressOfRawData - tls->StartAddressOfRawData,
                imageSize))
        {
            serverLog(LL_WARNING,
                "QFork module validation: non-boilerplate TLS data in %s",
                safeName);
            return false;
        }
        const byte *tlsTemplate = reinterpret_cast<const byte *>(module) +
            tlsTemplateRva;
        for (size_t i = 0; i < cQForkModuleTlsTemplateSize; i++) {
            if (tlsTemplate[i] != 0) {
                serverLog(LL_WARNING,
                    "QFork module validation: initialized TLS data in %s",
                    safeName);
                return false;
            }
        }
        if (tls->AddressOfIndex < base ||
            !AddQForkModuleProtectedRange(image.protectedRanges,
                tls->AddressOfIndex - base, sizeof(DWORD), imageSize))
        {
            serverLog(LL_WARNING,
                "QFork module validation: invalid TLS index in %s", safeName);
            return false;
        }

        if (tls->AddressOfCallBacks != 0) {
            if (tls->AddressOfCallBacks < base ||
                !QForkModuleRangeInsideImage(tls->AddressOfCallBacks - base,
                    sizeof(uint64_t), imageSize))
            {
                serverLog(LL_WARNING,
                    "QFork module validation: invalid TLS callback table in %s",
                    safeName);
                return false;
            }
            bool terminated = false;
            uint64_t callbacksRva = tls->AddressOfCallBacks - base;
            for (size_t i = 0; i < cQForkModuleMaxTlsCallbacks; i++) {
                uint64_t entryRva = callbacksRva + i * sizeof(uint64_t);
                if (!QForkModuleRangeInsideImage(entryRva,
                                                  sizeof(uint64_t), imageSize))
                    break;
                uint64_t callback = *reinterpret_cast<uint64_t *>(
                    reinterpret_cast<byte *>(module) + entryRva);
                if (callback == 0) {
                    terminated = true;
                    break;
                }
                if (callback < base || callback - base >= imageSize) break;
            }
            if (!terminated) {
                serverLog(LL_WARNING,
                    "QFork module validation: unsupported TLS callbacks in %s",
                    safeName);
                return false;
            }
        }
    }

    IMAGE_DATA_DIRECTORY loadConfigDirectory = GetQForkModuleDataDirectory(nt,
        IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
    if (loadConfigDirectory.VirtualAddress != 0 ||
        loadConfigDirectory.Size != 0)
    {
        const uint64_t securityCookieEnd =
            FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie) +
                sizeof(uint64_t);
        if (loadConfigDirectory.VirtualAddress == 0 ||
            loadConfigDirectory.Size < securityCookieEnd ||
            !QForkModuleRangeInsideImage(loadConfigDirectory.VirtualAddress,
                securityCookieEnd, imageSize))
        {
            serverLog(LL_WARNING,
                "QFork module validation: invalid load config in %s",
                safeName);
            return false;
        }
        const byte *loadConfig = reinterpret_cast<const byte *>(module) +
            loadConfigDirectory.VirtualAddress;
        DWORD declaredLoadConfigSize;
        memcpy(&declaredLoadConfigSize,
            loadConfig + FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64, Size),
            sizeof(declaredLoadConfigSize));
        uint64_t loadConfigSize = declaredLoadConfigSize;
        if (loadConfigSize < securityCookieEnd ||
            loadConfigSize > loadConfigDirectory.Size ||
            loadConfigSize > sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64) ||
            !QForkModuleRangeInsideImage(loadConfigDirectory.VirtualAddress,
                loadConfigSize, imageSize))
        {
            serverLog(LL_WARNING,
                "QFork module validation: inconsistent load config in %s",
                safeName);
            return false;
        }
        uint64_t securityCookie;
        memcpy(&securityCookie,
            loadConfig + FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY64,
                                      SecurityCookie),
            sizeof(securityCookie));
        if (securityCookie != 0 &&
            (securityCookie < base ||
             !AddQForkModuleProtectedRange(image.protectedRanges,
                securityCookie - base, sizeof(uint64_t), imageSize)))
        {
            serverLog(LL_WARNING,
                "QFork module validation: invalid security cookie in %s",
                safeName);
            return false;
        }
        if (loadConfigSize > cQForkModuleLoadConfigGuardOffset) {
            const byte *guardState = loadConfig +
                cQForkModuleLoadConfigGuardOffset;
            size_t guardStateSize = static_cast<size_t>(
                loadConfigSize - cQForkModuleLoadConfigGuardOffset);
            for (size_t i = 0; i < guardStateSize; i++) {
                if (guardState[i] != 0) {
                    serverLog(LL_WARNING,
                        "QFork module validation: extended load-config state is unsupported in %s",
                        safeName);
                    return false;
                }
            }
        }
    }

    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(
        reinterpret_cast<PIMAGE_NT_HEADERS>(nt));
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (section->Characteristics &
            (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_SHARED))
        {
            serverLog(LL_WARNING,
                "QFork module validation: unsafe writable section in %s",
                safeName);
            return false;
        }

        DWORD sectionSize = max(section->Misc.VirtualSize,
                                section->SizeOfRawData);
        if (sectionSize == 0) continue;
        if (!QForkModuleRangeInsideImage(section->VirtualAddress,
                                          sectionSize, imageSize))
        {
            serverLog(LL_WARNING,
                "QFork module validation: invalid section bounds in %s",
                safeName);
            return false;
        }

        QForkModuleImageRange writable;
        writable.address = reinterpret_cast<byte *>(module) +
            section->VirtualAddress;
        writable.rva = section->VirtualAddress;
        writable.size = sectionSize;
        writable.characteristics = section->Characteristics;
        writable.sectionIndex = i;
        memcpy(writable.name, section->Name, sizeof(writable.name));
        writable.snapshotOffset = 0;
        image.writableRanges.push_back(writable);
    }

    return true;
}

extern "C" BOOL QForkValidateModuleImage(void *handle, const wchar_t *path,
    const char *name)
{
    try {
        const char *safeName = name != NULL ? name : "unknown";
        if (path == NULL || path[0] == L'\0') return FALSE;
        wstring canonicalPath;
        HANDLE moduleFile = OpenQForkModuleFile(path, canonicalPath, safeName);
        if (moduleFile == NULL) return FALSE;
        SmartHandle pinnedFile(moduleFile);
        QForkModuleImageInfo image;
        return InspectQForkModuleImage(reinterpret_cast<HMODULE>(handle),
            moduleFile, name, image) ? TRUE : FALSE;
    }
    catch (const exception& ex) {
        serverLog(LL_WARNING,
            "QFork module validation: exception for %s: %s",
            name != NULL ? name : "unknown", ex.what());
        return FALSE;
    }
    catch (...) {
        serverLog(LL_WARNING,
            "QFork module validation: unknown exception for %s",
            name != NULL ? name : "unknown");
        return FALSE;
    }
}

static void ReleasePinnedQForkModules(vector<HMODULE>& modules)
{
    for (vector<HMODULE>::reverse_iterator module = modules.rbegin();
         module != modules.rend(); ++module)
    {
        if (*module != NULL) FreeLibrary(*module);
    }
    modules.clear();
}

static bool ReleasePinnedQForkModuleFiles(vector<HANDLE>& files)
{
    bool success = true;
    for (vector<HANDLE>::reverse_iterator file = files.rbegin();
         file != files.rend(); ++file)
    {
        if (*file != NULL && *file != INVALID_HANDLE_VALUE &&
            !CloseHandle(*file))
            success = false;
    }
    files.clear();
    return success;
}

static bool ReleaseQForkModuleSnapshot()
{
    if (g_IsForkedProcess) return true;

    bool success = true;
    if (g_hQForkModuleSnapshotMap != NULL) {
        if (!CloseHandle(g_hQForkModuleSnapshotMap)) success = false;
        g_hQForkModuleSnapshotMap = NULL;
    }
    for (vector<HMODULE>::reverse_iterator module =
             g_QForkPinnedModules.rbegin();
         module != g_QForkPinnedModules.rend(); ++module)
    {
        if (*module != NULL && !FreeLibrary(*module)) success = false;
    }
    g_QForkPinnedModules.clear();
    if (!ReleasePinnedQForkModuleFiles(g_QForkPinnedModuleFiles))
        success = false;

    if (g_pQForkControl != NULL) {
        g_pQForkControl->globalData.moduleSnapshotMap = NULL;
        g_pQForkControl->globalData.moduleSnapshotSize = 0;
        g_pQForkControl->globalData.moduleSnapshotCount = 0;
        g_pQForkControl->globalData.moduleSnapshotReserved = 0;
    }
    return success;
}

static void PrepareQForkModuleSnapshot()
{
    if (g_hQForkModuleSnapshotMap != NULL ||
        !g_QForkPinnedModules.empty() ||
        !g_QForkPinnedModuleFiles.empty())
        throw runtime_error("A QFork module snapshot is already active");

    g_pQForkControl->globalData.moduleSnapshotMap = NULL;
    g_pQForkControl->globalData.moduleSnapshotSize = 0;
    g_pQForkControl->globalData.moduleSnapshotCount = 0;
    g_pQForkControl->globalData.moduleSnapshotReserved = 0;

    vector<QForkModuleDescriptor> descriptors;
    if (moduleForEachForkModule(CollectQForkModule, &descriptors) != 0)
        throw runtime_error("Could not enumerate modules for QFork snapshot");
    if (descriptors.empty()) return;

    sort(descriptors.begin(), descriptors.end(),
        [](const QForkModuleDescriptor& left,
           const QForkModuleDescriptor& right) {
            return left.sequence < right.sequence;
        });

    if (descriptors.size() > numeric_limits<uint32_t>::max())
        throw runtime_error("Too many modules for QFork snapshot");

    vector<QForkCapturedModule> modules;
    vector<HMODULE> pinnedModules;
    vector<HANDLE> pinnedModuleFiles;
    HANDLE writableMap = NULL;
    HANDLE readOnlyMap = NULL;
    byte *view = NULL;

    try {
        modules.reserve(descriptors.size());
        pinnedModules.reserve(descriptors.size());
        pinnedModuleFiles.reserve(descriptors.size());
        uint64_t totalRanges = 0;
        for (size_t i = 0; i < descriptors.size(); i++) {
            if (i != 0 &&
                descriptors[i - 1].sequence >= descriptors[i].sequence)
                throw runtime_error("Duplicate QFork module load sequence");
            for (size_t j = 0; j < i; j++) {
                if (descriptors[j].expectedBase == descriptors[i].expectedBase)
                    throw runtime_error("Duplicate QFork module image base");
            }

            HMODULE pinned = NULL;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(descriptors[i].expectedBase),
                    &pinned)) {
                throw system_error(GetLastError(), system_category(),
                    "Could not pin QFork module image");
            }
            if (pinned != descriptors[i].expectedBase) {
                FreeLibrary(pinned);
                throw runtime_error("Pinned QFork module image base changed");
            }
            pinnedModules.push_back(pinned);

            wstring canonicalPath;
            HANDLE pinnedFile = OpenQForkModuleFile(
                descriptors[i].path.c_str(), canonicalPath,
                descriptors[i].name.c_str());
            if (pinnedFile == NULL)
                throw runtime_error("Could not lock QFork module image file");
            pinnedModuleFiles.push_back(pinnedFile);

            QForkCapturedModule captured;
            captured.descriptor = descriptors[i];
            captured.descriptor.path = canonicalPath;
            captured.nameOffset = 0;
            captured.pathOffset = 0;
            captured.firstRange = static_cast<uint32_t>(totalRanges);
            if (!InspectQForkModuleImage(pinned, pinnedFile,
                    captured.descriptor.name.c_str(), captured.image))
                throw runtime_error("Could not validate QFork module image");
            if (!QForkCheckedAdd(totalRanges,
                    captured.image.writableRanges.size(), &totalRanges) ||
                totalRanges > numeric_limits<uint32_t>::max())
                throw runtime_error("Too many writable ranges in QFork snapshot");
            modules.push_back(captured);
        }

        uint64_t cursor;
        uint64_t bytes;
        if (!QForkCheckedAlign(sizeof(QForkModuleSnapshotHeader), 8, &cursor))
            throw runtime_error("QFork snapshot size overflow");
        uint64_t modulesOffset = cursor;
        if (!QForkCheckedMultiply(modules.size(),
                sizeof(QForkModuleSnapshotRecord), &bytes) ||
            !QForkCheckedAdd(cursor, bytes, &cursor) ||
            !QForkCheckedAlign(cursor, 8, &cursor))
            throw runtime_error("QFork snapshot module table overflow");
        uint64_t rangesOffset = cursor;
        if (!QForkCheckedMultiply(totalRanges,
                sizeof(QForkModuleSnapshotRange), &bytes) ||
            !QForkCheckedAdd(cursor, bytes, &cursor) ||
            !QForkCheckedAlign(cursor, 8, &cursor))
            throw runtime_error("QFork snapshot range table overflow");
        uint64_t payloadOffset = cursor;

        for (size_t i = 0; i < modules.size(); i++) {
            if (modules[i].descriptor.name.size() + 1 >
                    numeric_limits<uint32_t>::max() ||
                modules[i].descriptor.path.size() + 1 >
                    numeric_limits<uint32_t>::max())
                throw runtime_error("QFork module name or path is too long");

            modules[i].nameOffset = cursor;
            if (!QForkCheckedAdd(cursor,
                    modules[i].descriptor.name.size() + 1, &cursor) ||
                !QForkCheckedAlign(cursor, sizeof(wchar_t), &cursor))
                throw runtime_error("QFork snapshot name overflow");
            modules[i].pathOffset = cursor;
            if (!QForkCheckedMultiply(modules[i].descriptor.path.size() + 1,
                    sizeof(wchar_t), &bytes) ||
                !QForkCheckedAdd(cursor, bytes, &cursor) ||
                !QForkCheckedAlign(cursor, 8, &cursor))
                throw runtime_error("QFork snapshot path overflow");

            for (size_t j = 0; j < modules[i].image.writableRanges.size(); j++) {
                modules[i].image.writableRanges[j].snapshotOffset = cursor;
                if (!QForkCheckedAdd(cursor,
                        modules[i].image.writableRanges[j].size, &cursor) ||
                    !QForkCheckedAlign(cursor, 8, &cursor))
                    throw runtime_error("QFork snapshot data overflow");
            }
        }

        if (cursor > numeric_limits<SIZE_T>::max())
            throw runtime_error("QFork module snapshot is too large");

        DWORD sizeHigh = static_cast<DWORD>(cursor >> 32);
        DWORD sizeLow = static_cast<DWORD>(cursor & MAXDWORD);
        writableMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
            PAGE_READWRITE, sizeHigh, sizeLow, NULL);
        if (writableMap == NULL)
            throw system_error(GetLastError(), system_category(),
                "Could not create QFork module snapshot mapping");
        view = reinterpret_cast<byte *>(MapViewOfFile(writableMap,
            FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(cursor)));
        if (view == NULL)
            throw system_error(GetLastError(), system_category(),
                "Could not map writable QFork module snapshot");
        memset(view, 0, static_cast<SIZE_T>(cursor));

        QForkModuleSnapshotHeader *header =
            reinterpret_cast<QForkModuleSnapshotHeader *>(view);
        header->magic = cQForkModuleSnapshotMagic;
        header->version = cQForkModuleSnapshotVersion;
        header->headerSize = sizeof(*header);
        header->moduleCount = static_cast<uint32_t>(modules.size());
        header->rangeCount = static_cast<uint32_t>(totalRanges);
        header->totalSize = cursor;
        header->modulesOffset = modulesOffset;
        header->rangesOffset = rangesOffset;
        header->payloadOffset = payloadOffset;

        QForkModuleSnapshotRecord *records =
            reinterpret_cast<QForkModuleSnapshotRecord *>(view + modulesOffset);
        QForkModuleSnapshotRange *ranges =
            reinterpret_cast<QForkModuleSnapshotRange *>(view + rangesOffset);
        uint32_t rangeIndex = 0;
        for (size_t i = 0; i < modules.size(); i++) {
            QForkModuleSnapshotRecord& record = records[i];
            record.sequence = modules[i].descriptor.sequence;
            record.expectedBase = reinterpret_cast<uint64_t>(
                modules[i].descriptor.expectedBase);
            record.nameOffset = modules[i].nameOffset;
            record.pathOffset = modules[i].pathOffset;
            record.nameBytes = static_cast<uint32_t>(
                modules[i].descriptor.name.size() + 1);
            record.pathChars = static_cast<uint32_t>(
                modules[i].descriptor.path.size() + 1);
            record.firstRange = modules[i].firstRange;
            record.rangeCount = static_cast<uint32_t>(
                modules[i].image.writableRanges.size());
            record.imageSize = modules[i].image.imageSize;
            record.sizeOfHeaders = modules[i].image.sizeOfHeaders;
            record.timeDateStamp = modules[i].image.timeDateStamp;
            record.checkSum = modules[i].image.checkSum;
            record.machine = modules[i].image.machine;
            record.optionalMagic = modules[i].image.optionalMagic;
            record.numberOfSections = modules[i].image.numberOfSections;
            record.volumeSerialNumber =
                modules[i].image.fileIdentity.volumeSerialNumber;
            record.fileIndexHigh = modules[i].image.fileIdentity.fileIndexHigh;
            record.fileIndexLow = modules[i].image.fileIdentity.fileIndexLow;
            record.fileSize = modules[i].image.fileIdentity.fileSize;
            record.lastWriteTime = modules[i].image.fileIdentity.lastWriteTime;

            memcpy(view + record.nameOffset,
                   modules[i].descriptor.name.c_str(), record.nameBytes);
            memcpy(view + record.pathOffset,
                   modules[i].descriptor.path.c_str(),
                   static_cast<size_t>(record.pathChars) * sizeof(wchar_t));

            for (size_t j = 0; j < modules[i].image.writableRanges.size(); j++) {
                QForkModuleImageRange& source =
                    modules[i].image.writableRanges[j];
                QForkModuleSnapshotRange& range = ranges[rangeIndex++];
                range.bytesOffset = source.snapshotOffset;
                range.rva = source.rva;
                range.size = source.size;
                range.characteristics = source.characteristics;
                range.sectionIndex = source.sectionIndex;
                memcpy(range.name, source.name, sizeof(range.name));
                memcpy(view + range.bytesOffset, source.address, range.size);
            }
        }

        if (!UnmapViewOfFile(view))
            throw system_error(GetLastError(), system_category(),
                "Could not unmap writable QFork module snapshot");
        view = NULL;

        if (!DuplicateHandle(GetCurrentProcess(), writableMap,
                GetCurrentProcess(), &readOnlyMap,
                SECTION_MAP_READ | SECTION_QUERY, FALSE, 0))
            throw system_error(GetLastError(), system_category(),
                "Could not create read-only QFork module snapshot handle");
        CloseHandle(writableMap);
        writableMap = NULL;

        g_hQForkModuleSnapshotMap = readOnlyMap;
        readOnlyMap = NULL;
        g_QForkPinnedModules.swap(pinnedModules);
        g_QForkPinnedModuleFiles.swap(pinnedModuleFiles);
        g_pQForkControl->globalData.moduleSnapshotMap =
            g_hQForkModuleSnapshotMap;
        g_pQForkControl->globalData.moduleSnapshotSize = cursor;
        g_pQForkControl->globalData.moduleSnapshotCount =
            static_cast<uint32_t>(modules.size());
        serverLog(LL_DEBUG,
            "QFork captured %u module images in %llu bytes",
            static_cast<unsigned>(modules.size()),
            static_cast<unsigned long long>(cursor));
    }
    catch (...) {
        if (view != NULL) UnmapViewOfFile(view);
        if (readOnlyMap != NULL) CloseHandle(readOnlyMap);
        if (writableMap != NULL) CloseHandle(writableMap);
        ReleasePinnedQForkModuleFiles(pinnedModuleFiles);
        ReleasePinnedQForkModules(pinnedModules);
        throw;
    }
}

static const byte *QForkSnapshotSpan(const byte *snapshot, size_t snapshotSize,
    uint64_t offset, uint64_t size, size_t alignment)
{
    if (alignment != 0 && offset % alignment != 0) return NULL;
    if (offset > snapshotSize || size > snapshotSize - offset) return NULL;
    return snapshot + offset;
}

static bool ValidateQForkModuleSnapshot(const byte *snapshot,
    size_t snapshotSize, uint32_t expectedCount,
    const QForkModuleSnapshotHeader **headerOut,
    const QForkModuleSnapshotRecord **recordsOut,
    const QForkModuleSnapshotRange **rangesOut)
{
    if (snapshot == NULL ||
        snapshotSize < sizeof(QForkModuleSnapshotHeader))
        return false;

    const QForkModuleSnapshotHeader *header =
        reinterpret_cast<const QForkModuleSnapshotHeader *>(snapshot);
    if (header->magic != cQForkModuleSnapshotMagic ||
        header->version != cQForkModuleSnapshotVersion ||
        header->headerSize != sizeof(*header) ||
        header->moduleCount != expectedCount ||
        header->totalSize != snapshotSize)
        return false;

    uint64_t moduleBytes;
    uint64_t rangeBytes;
    uint64_t modulesOffset;
    uint64_t modulesEnd;
    uint64_t rangesOffset;
    uint64_t rangesEnd;
    uint64_t payloadOffset;
    if (!QForkCheckedMultiply(header->moduleCount,
            sizeof(QForkModuleSnapshotRecord), &moduleBytes) ||
        !QForkCheckedMultiply(header->rangeCount,
            sizeof(QForkModuleSnapshotRange), &rangeBytes) ||
        !QForkCheckedAlign(sizeof(QForkModuleSnapshotHeader), 8,
            &modulesOffset) ||
        !QForkCheckedAdd(modulesOffset, moduleBytes, &modulesEnd) ||
        !QForkCheckedAlign(modulesEnd, 8, &rangesOffset) ||
        !QForkCheckedAdd(rangesOffset, rangeBytes, &rangesEnd) ||
        !QForkCheckedAlign(rangesEnd, 8, &payloadOffset) ||
        header->modulesOffset != modulesOffset ||
        header->rangesOffset != rangesOffset ||
        header->payloadOffset != payloadOffset)
        return false;

    const byte *moduleTable = QForkSnapshotSpan(snapshot, snapshotSize,
        header->modulesOffset, moduleBytes, 8);
    const byte *rangeTable = QForkSnapshotSpan(snapshot, snapshotSize,
        header->rangesOffset, rangeBytes, 8);
    if (moduleTable == NULL || rangeTable == NULL ||
        header->payloadOffset > snapshotSize)
        return false;

    const QForkModuleSnapshotRecord *records =
        reinterpret_cast<const QForkModuleSnapshotRecord *>(moduleTable);
    const QForkModuleSnapshotRange *ranges =
        reinterpret_cast<const QForkModuleSnapshotRange *>(rangeTable);
    uint64_t previousSequence = 0;
    uint64_t payloadCursor = header->payloadOffset;
    uint32_t nextRange = 0;
    for (uint32_t i = 0; i < header->moduleCount; i++) {
        const QForkModuleSnapshotRecord& record = records[i];
        if (record.nameBytes == 0 || record.pathChars == 0 ||
            record.expectedBase == 0 ||
            (i != 0 && record.sequence <= previousSequence) ||
            record.firstRange != nextRange ||
            record.rangeCount > header->rangeCount - nextRange ||
            record.imageSize == 0 || record.sizeOfHeaders == 0 ||
            record.sizeOfHeaders > record.imageSize ||
            record.machine != IMAGE_FILE_MACHINE_AMD64 ||
            record.optionalMagic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            record.numberOfSections == 0 || record.reserved != 0 ||
            record.fileIdentityReserved != 0 ||
            record.expectedBase >
                numeric_limits<uint64_t>::max() - record.imageSize)
            return false;
        previousSequence = record.sequence;

        if (record.nameOffset != payloadCursor) return false;
        const char *name = reinterpret_cast<const char *>(
            QForkSnapshotSpan(snapshot, snapshotSize, record.nameOffset,
                record.nameBytes, 1));
        uint64_t nameEnd;
        uint64_t pathBytes;
        if (!QForkCheckedAdd(record.nameOffset, record.nameBytes, &nameEnd) ||
            !QForkCheckedAlign(nameEnd, sizeof(wchar_t), &payloadCursor) ||
            record.pathOffset != payloadCursor ||
            !QForkCheckedMultiply(record.pathChars, sizeof(wchar_t),
                                  &pathBytes))
            return false;
        const wchar_t *path = reinterpret_cast<const wchar_t *>(
            QForkSnapshotSpan(snapshot, snapshotSize, record.pathOffset,
                pathBytes, sizeof(wchar_t)));
        uint64_t pathEnd;
        if (name == NULL || path == NULL ||
            name[record.nameBytes - 1] != '\0' ||
            path[record.pathChars - 1] != L'\0' ||
            memchr(name, '\0', record.nameBytes - 1) != NULL ||
            wmemchr(path, L'\0', record.pathChars - 1) != NULL ||
            !QForkCheckedAdd(record.pathOffset, pathBytes, &pathEnd) ||
            !QForkCheckedAlign(pathEnd, 8, &payloadCursor))
            return false;

        uint64_t previousEnd = 0;
        for (uint32_t j = 0; j < record.rangeCount; j++) {
            const QForkModuleSnapshotRange& range =
                ranges[record.firstRange + j];
            if (range.size == 0 ||
                range.bytesOffset != payloadCursor ||
                range.reserved != 0 ||
                range.sectionIndex >= record.numberOfSections ||
                !(range.characteristics & IMAGE_SCN_MEM_WRITE) ||
                (range.characteristics & (IMAGE_SCN_MEM_EXECUTE |
                    IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_SHARED)) ||
                QForkSnapshotSpan(snapshot, snapshotSize, range.bytesOffset,
                    range.size, 1) == NULL ||
                !QForkModuleRangeInsideImage(range.rva, range.size,
                                              record.imageSize) ||
                (j != 0 && range.rva < previousEnd))
                return false;
            previousEnd = static_cast<uint64_t>(range.rva) + range.size;
            uint64_t rangeEnd;
            if (!QForkCheckedAdd(range.bytesOffset, range.size, &rangeEnd) ||
                !QForkCheckedAlign(rangeEnd, 8, &payloadCursor))
                return false;
        }
        nextRange += record.rangeCount;
    }

    if (nextRange != header->rangeCount || payloadCursor != header->totalSize)
        return false;

    *headerOut = header;
    *recordsOut = records;
    *rangesOut = ranges;
    return true;
}

static bool RestoreQForkModuleImage(const byte *snapshot, size_t snapshotSize,
    const QForkModuleSnapshotRecord& record,
    const QForkModuleSnapshotRange *ranges)
{
    const char *name = reinterpret_cast<const char *>(snapshot +
                                                       record.nameOffset);
    const wchar_t *path = reinterpret_cast<const wchar_t *>(snapshot +
                                                             record.pathOffset);

    QForkModuleFileIdentity recordedIdentity;
    recordedIdentity.volumeSerialNumber = record.volumeSerialNumber;
    recordedIdentity.fileIndexHigh = record.fileIndexHigh;
    recordedIdentity.fileIndexLow = record.fileIndexLow;
    recordedIdentity.fileSize = record.fileSize;
    recordedIdentity.lastWriteTime = record.lastWriteTime;

    wstring canonicalPath;
    HANDLE childModuleFile = OpenQForkModuleFile(path, canonicalPath, name);
    if (childModuleFile == NULL) return false;
    SmartHandle pinnedFile(childModuleFile);
    QForkModuleFileIdentity childFileIdentity;
    if (canonicalPath != path ||
        !GetQForkModuleFileIdentity(childModuleFile, childFileIdentity, name) ||
        !QForkModuleFileIdentityEqual(childFileIdentity, recordedIdentity))
    {
        serverLog(LL_WARNING,
            "QFork module restore: locked image changed for %s", name);
        return false;
    }

    HMODULE childBase = LoadLibraryExW(path, NULL,
                                      LOAD_WITH_ALTERED_SEARCH_PATH);
    if (childBase == NULL) {
        serverLog(LL_WARNING,
            "QFork module restore: LoadLibrary failed for %s (0x%08x)",
            name, GetLastError());
        return false;
    }
    if (reinterpret_cast<uint64_t>(childBase) != record.expectedBase) {
        serverLog(LL_WARNING,
            "QFork module restore: %s loaded at %p, expected %p",
            name, childBase, reinterpret_cast<void *>(record.expectedBase));
        return false;
    }

    QForkModuleImageInfo image;
    if (!InspectQForkModuleImage(childBase, childModuleFile, name, image))
        return false;
    if (image.imageSize != record.imageSize ||
        image.sizeOfHeaders != record.sizeOfHeaders ||
        image.timeDateStamp != record.timeDateStamp ||
        image.checkSum != record.checkSum ||
        image.machine != record.machine ||
        image.optionalMagic != record.optionalMagic ||
        image.numberOfSections != record.numberOfSections ||
        !QForkModuleFileIdentityEqual(image.fileIdentity, recordedIdentity) ||
        image.writableRanges.size() != record.rangeCount)
    {
        serverLog(LL_WARNING,
            "QFork module restore: image metadata changed for %s", name);
        return false;
    }

    byte *imageBase = reinterpret_cast<byte *>(childBase);
    for (uint32_t i = 0; i < record.rangeCount; i++) {
        const QForkModuleSnapshotRange& source = ranges[record.firstRange + i];
        const QForkModuleImageRange& target = image.writableRanges[i];
        if (target.rva != source.rva || target.size != source.size ||
            target.characteristics != source.characteristics ||
            target.sectionIndex != source.sectionIndex ||
            memcmp(target.name, source.name, sizeof(source.name)) != 0)
        {
            serverLog(LL_WARNING,
                "QFork module restore: writable section layout changed for %s",
                name);
            return false;
        }

        const byte *parentBytes = QForkSnapshotSpan(snapshot, snapshotSize,
            source.bytesOffset, source.size, 1);
        if (parentBytes == NULL) return false;
        vector<byte> childBytes(source.size);
        memcpy(childBytes.data(), target.address, source.size);

        DWORD oldProtect;
        if (!VirtualProtect(target.address, source.size, PAGE_READWRITE,
                            &oldProtect))
        {
            serverLog(LL_WARNING,
                "QFork module restore: could not unprotect %s state (0x%08x)",
                name, GetLastError());
            return false;
        }

        memcpy(target.address, parentBytes, source.size);
        RestoreQForkModuleProtectedRanges(imageBase, target.rva, childBytes,
                                          image.protectedRanges);

        DWORD ignored;
        if (!VirtualProtect(target.address, source.size, oldProtect, &ignored)) {
            serverLog(LL_WARNING,
                "QFork module restore: could not reprotect %s state (0x%08x)",
                name, GetLastError());
            return false;
        }
    }

    serverLog(LL_DEBUG, "QFork restored module %s at %p",
              name, childBase);
    return true;
}

static bool RestoreQForkModules(const byte *snapshot, size_t snapshotSize,
    uint32_t expectedCount)
{
    if (expectedCount == 0)
        return snapshot == NULL && snapshotSize == 0;

    const QForkModuleSnapshotHeader *header;
    const QForkModuleSnapshotRecord *records;
    const QForkModuleSnapshotRange *ranges;
    if (!ValidateQForkModuleSnapshot(snapshot, snapshotSize, expectedCount,
                                     &header, &records, &ranges))
        return false;

    for (uint32_t i = 0; i < header->moduleCount; i++) {
        if (!RestoreQForkModuleImage(snapshot, snapshotSize, records[i], ranges))
            return false;
    }
    return true;
}

BOOL QForkChildInit(HANDLE QForkControlMemoryMapHandle, DWORD ParentProcessID) {
    SmartHandle shParent;
    SmartHandle shQForkControlHeapMap;
    SmartFileView<QForkControl> sfvParentQForkControl;
    SmartHandle dupOperationComplete;
    SmartHandle dupOperationFailed;
    SmartHandle dupModuleSnapshot;
    SmartFileView<byte> sfvModuleSnapshot;
    const byte *moduleSnapshot = NULL;
    size_t moduleSnapshotSize = 0;

    try {
        shParent.Assign(
            OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE,
                        TRUE, ParentProcessID),
            string("Could not open parent process"));

        shQForkControlHeapMap.Assign(shParent, QForkControlMemoryMapHandle);
        sfvParentQForkControl.Assign(
            shQForkControlHeapMap,
            FILE_MAP_COPY,
            string("Could not map view of QForkControl in child. Is system swap file large enough?"));
        g_pQForkControl = sfvParentQForkControl;

        // Duplicate handles and stuff into control structure (parent protected by PAGE_WRITECOPY)

        dupOperationComplete.Assign(shParent, sfvParentQForkControl->operationComplete);
        g_pQForkControl->operationComplete = dupOperationComplete;

        dupOperationFailed.Assign(shParent, sfvParentQForkControl->operationFailed);
        g_pQForkControl->operationFailed = dupOperationFailed;

        const QForkInfo& parentData = sfvParentQForkControl->globalData;
        if (parentData.moduleSnapshotReserved != 0) {
            throw runtime_error("Invalid QFork module snapshot metadata");
        }
        if (parentData.moduleSnapshotCount == 0) {
            if (parentData.moduleSnapshotMap != NULL ||
                parentData.moduleSnapshotSize != 0)
                throw runtime_error("Inconsistent empty QFork module snapshot");
        }
        else {
            if (parentData.moduleSnapshotMap == NULL ||
                parentData.moduleSnapshotSize <
                    sizeof(QForkModuleSnapshotHeader) ||
                parentData.moduleSnapshotSize >
                    numeric_limits<SIZE_T>::max())
                throw runtime_error("Invalid QFork module snapshot size");

            dupModuleSnapshot.Assign(shParent,
                                     parentData.moduleSnapshotMap);
            moduleSnapshotSize = static_cast<size_t>(
                parentData.moduleSnapshotSize);
            moduleSnapshot = sfvModuleSnapshot.Assign(
                dupModuleSnapshot, FILE_MAP_READ, 0, 0,
                static_cast<SIZE_T>(moduleSnapshotSize),
                string("Could not map QFork module snapshot in child"));
        }

        vector<SmartHandle> dupHeapHandle(g_pQForkControl->numMappedBlocks);
        vector<SmartFileView<byte>> sfvHeap(g_pQForkControl->numMappedBlocks);
        for (int i = 0; i < g_pQForkControl->numMappedBlocks; i++) {
            if (sfvParentQForkControl->heapBlockList[i].state == BlockState::bsMAPPED_IN_USE) {
                dupHeapHandle[i].Assign(shParent, sfvParentQForkControl->heapBlockList[i].heapMap);
                g_pQForkControl->heapBlockList[i].heapMap = dupHeapHandle[i];

                sfvHeap[i].Assign(g_pQForkControl->heapBlockList[i].heapMap,
                    FILE_MAP_COPY,
                    0,
                    0,
                    cAllocationGranularity,
                    (byte*) g_pQForkControl->heapStart + i * cAllocationGranularity,
                    string("QForkChildInit: could not map heap in forked process"));
            }
            else {
                g_pQForkControl->heapBlockList[i].heapMap = NULL;
                g_pQForkControl->heapBlockList[i].state = BlockState::bsINVALID;
            }
        }
        g_QForkHeapReady = TRUE;

        // Copy redis globals into fork process
        if (!SetupRedisGlobals(g_pQForkControl->globalData.redisData,
                g_pQForkControl->globalData.redisDataSize,
                g_pQForkControl->globalData.dictHashSeed,
                &g_pQForkControl->globalData.acl,
                &g_pQForkControl->globalData.core,
                g_pQForkControl->globalData.sharedData,
                g_pQForkControl->globalData.sharedDataSize,
                &g_pQForkControl->globalData.modules,
                g_pQForkControl->globalData.usedMemory)) {
            throw runtime_error("Could not restore Redis globals in QFork child");
        }

        if (moduleCount() !=
            g_pQForkControl->globalData.moduleSnapshotCount)
            throw runtime_error("QFork module registry count does not match snapshot");

        if (!RestoreQForkModules(moduleSnapshot, moduleSnapshotSize,
                g_pQForkControl->globalData.moduleSnapshotCount)) {
            throw runtime_error("Could not restore loaded modules in QFork child");
        }
        moduleSetQForkChildReady(1);

        // Execute requested operation
        if (g_pQForkControl->typeOfOperation == OperationType::otRDB) {
            g_ChildExitCode = do_rdbSave(
                g_pQForkControl->globalData.rdb_req,
                g_pQForkControl->globalData.filename,
                g_pQForkControl->globalData.rdb_save_info,
                g_pQForkControl->globalData.rdb_save_info_size,
                g_pQForkControl->globalData.rdb_flags);
        }
        else if (g_pQForkControl->typeOfOperation == OperationType::otAOF) {
            g_ChildExitCode = do_aofSave(g_pQForkControl->globalData.filename);
        }
        else if (g_pQForkControl->typeOfOperation == OperationType::otSocket) {
            int rdb_pipe_write_fd = FDAPI_WSASocket(
                FROM_PROTOCOL_INFO,
                FROM_PROTOCOL_INFO,
                FROM_PROTOCOL_INFO,
                &g_pQForkControl->globalData.rdb_pipe_write_protocol_info,
                0,
                WSA_FLAG_OVERLAPPED);
            if (rdb_pipe_write_fd == -1) {
                throw system_error(FDAPI_WSAGetLastError(), system_category(),
                    "Could not recreate diskless RDB pipe writer in QFork child");
            }

            int safe_to_exit_pipe_fd = FDAPI_WSASocket(
                FROM_PROTOCOL_INFO,
                FROM_PROTOCOL_INFO,
                FROM_PROTOCOL_INFO,
                &g_pQForkControl->globalData.rdb_child_exit_pipe_read_protocol_info,
                0,
                WSA_FLAG_OVERLAPPED);
            if (safe_to_exit_pipe_fd == -1) {
                FDAPI_CloseDuplicatedSocket(rdb_pipe_write_fd);
                throw system_error(FDAPI_WSAGetLastError(), system_category(),
                    "Could not recreate diskless RDB exit pipe reader in QFork child");
            }

            g_ChildExitCode = do_socketSave(
                g_pQForkControl->globalData.rdb_req,
                g_pQForkControl->globalData.rdb_save_info,
                g_pQForkControl->globalData.rdb_save_info_size,
                rdb_pipe_write_fd,
                safe_to_exit_pipe_fd);
        }
        else {
            throw runtime_error("unexpected operation type");
        }

        // Let parent know we are done
        IFFAILTHROW(SetEvent(g_pQForkControl->operationComplete),
            "Could not signal QFork operation completion");

        IFFAILTHROW(TerminateProcess(GetCurrentProcess(),
            static_cast<UINT>(g_ChildExitCode)),
            "Could not terminate completed QFork child");
    }
    catch (const system_error& syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            RedisEventLog().LogError("QForkChildInit: system error. " + string(syserr.what()));
            serverLog(LL_WARNING, "QForkChildInit: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
        }
    }
    catch (const exception& ex) {
        RedisEventLog().LogError("QForkChildInit: exception. " + string(ex.what()));
        serverLog(LL_WARNING,
            "QForkChildInit: exception caught. message=%s\n", ex.what());
    }
    catch (...) {
        RedisEventLog().LogError("QForkChildInit: unknown exception.");
        serverLog(LL_WARNING, "QForkChildInit: unknown exception caught.\n");
    }

    if (g_pQForkControl != NULL) {
        if (g_pQForkControl->operationFailed != NULL) {
            SetEvent(g_pQForkControl->operationFailed);
        }
    }
    TerminateProcess(GetCurrentProcess(), 1);
    return FALSE;
}

BOOL QForkParentInit() {
    try {
        // Allocate file map for qfork control so it can be passed to the forked process
        g_hQForkControlFileMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0, sizeof(QForkControl),
            NULL);
        IFFAILTHROW(g_hQForkControlFileMap, "QForkMasterInit: CreateFileMapping failed");

        g_pQForkControl = (QForkControl*) MapViewOfFile(
            g_hQForkControlFileMap,
            FILE_MAP_ALL_ACCESS,
            0, 0,
            0);
        IFFAILTHROW(g_pQForkControl, "QForkMasterInit: MapViewOfFile failed");

        MEMORYSTATUSEX memstatus;
        memstatus.dwLength = sizeof(MEMORYSTATUSEX);
        IFFAILTHROW(GlobalMemoryStatusEx(&memstatus), "QForkMasterInit: cannot get global memory status");

#ifdef _WIN64
        size_t max_heap_allocation = memstatus.ullTotalPhys * 10;
        if (max_heap_allocation > cAllocationGranularity * cMaxBlocks) {
            max_heap_allocation = cAllocationGranularity * cMaxBlocks;
        }
#else
        // On x86 the limit is always cAllocationGranularity * cMaxBlocks
        size_t max_heap_allocation = cAllocationGranularity * cMaxBlocks;
#endif

        // maxAvailableBlocks is guaranteed to be <= cMaxBlocks
        // On x86 maxAvailableBlocks = cMaxBlocks
        g_pQForkControl->maxAvailableBlocks = (int) (max_heap_allocation / cAllocationGranularity);
        g_pQForkControl->blockSearchStart = 0;
        g_pQForkControl->numMappedBlocks = 0;

        // Find a place in the virtual memory space where we can reserve space for
        // our allocations that is likely to be available in the forked process.
        LPVOID pHigh = VirtualAllocEx(
            GetCurrentProcess(),
            NULL,
            // the +1 is needed since we will align the heap start address
            (g_pQForkControl->maxAvailableBlocks + 1) * cAllocationGranularity,
#ifdef _WIN64
            MEM_RESERVE | MEM_TOP_DOWN,
#else
            MEM_RESERVE,
#endif
            PAGE_READWRITE);
        IFFAILTHROW(pHigh, "QForkMasterInit: VirtualAllocEx failed.");

        IFFAILTHROW(VirtualFree(pHigh, 0, MEM_RELEASE), "QForkMasterInit: VirtualFree failed.");

        // Need to adjust the heap start address to align on allocation granularity offset
        g_pQForkControl->heapStart = (LPVOID) (((uintptr_t) pHigh + cAllocationGranularity) - ((uintptr_t) pHigh % cAllocationGranularity));
        g_pQForkControl->heapEnd = (LPVOID) ((uintptr_t) g_pQForkControl->heapStart +
            g_pQForkControl->maxAvailableBlocks * cAllocationGranularity);

        // Reserve the heap memory that will be mapped on demand in AllocHeapBlock()
        for (int i = 0; i < g_pQForkControl->maxAvailableBlocks; i++) {
            LPVOID addr = (byte*) g_pQForkControl->heapStart + i * cAllocationGranularity;
            IFFAILTHROW(VirtualAlloc(addr, cAllocationGranularity, MEM_RESERVE, PAGE_READWRITE),
                "QForkMasterInit: VirtualAlloc of reserve segment failed");
        }

        for (int i = 0; i < g_pQForkControl->maxAvailableBlocks; i++) {
            g_pQForkControl->heapBlockList[i].state = BlockState::bsUNMAPPED;
            g_pQForkControl->heapBlockList[i].heapMap = NULL;
        }
        for (int i = g_pQForkControl->maxAvailableBlocks; i < cMaxBlocks; i++) {
            g_pQForkControl->heapBlockList[i].state = BlockState::bsINVALID;
        }
        g_QForkHeapReady = TRUE;
        // Startup allocations may have initialized jemalloc before QFork is ready.
        // Move Redis data allocations to a fresh arena backed by the QFork heap.
        SwitchToQForkJemallocArena();

        g_pQForkControl->typeOfOperation = OperationType::otINVALID;
        g_pQForkControl->operationComplete = CreateEvent(NULL, TRUE, FALSE, NULL);
        IFFAILTHROW(g_pQForkControl->operationComplete, "QForkMasterInit: CreateEvent failed.");

        g_pQForkControl->operationFailed = CreateEvent(NULL, TRUE, FALSE, NULL);
        IFFAILTHROW(g_pQForkControl->operationFailed, "QForkMasterInit: CreateEvent failed.");

        return TRUE;
    }
    catch (const system_error& syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            RedisEventLog().LogError("QForkParentInit: system error. " + string(syserr.what()));
            serverLog(LL_WARNING, "QForkParentInit: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
        }
    }
    catch (const runtime_error& runerr) {
        RedisEventLog().LogError("QForkParentInit: runtime error. " + string(runerr.what()));
        serverLog(LL_WARNING, "QForkParentInit: runtime error caught. message=%s\n", runerr.what());
    }
    catch (const exception& ex) {
        RedisEventLog().LogError("QForkParentInit: an exception occurred. " + string(ex.what()));
        serverLog(LL_WARNING, "QForkParentInit: other exception caught.\n");
    }
    return FALSE;
}

StartupStatus QForkStartup() {
    PERFORMANCE_INFORMATION perfinfo;
    perfinfo.cb = sizeof(PERFORMANCE_INFORMATION);
    if (FALSE == GetPerformanceInfo(&perfinfo, sizeof(PERFORMANCE_INFORMATION))) {
        serverLog(LL_WARNING, "GetPerformanceInfo failed.\n");
        serverLog(LL_WARNING, "Failing startup.\n");
        return StartupStatus::ssFAILED;
    }
    Globals::pageSize = perfinfo.PageSize;

    if (g_IsForkedProcess) {
        // Child command line looks like: --QFork [QForkControlMemoryMap handle] [parent pid]
        HANDLE QForkControlMemoryMapHandle = (HANDLE) strtoull(g_argMap[cQFork].at(0).at(0).c_str(), NULL, 10);
        DWORD PPID = strtoul(g_argMap[cQFork].at(0).at(1).c_str(), NULL, 10);
        return QForkChildInit(QForkControlMemoryMapHandle, PPID) ? StartupStatus::ssCHILD_EXIT : StartupStatus::ssFAILED;
    }
    else {
        return QForkParentInit() ? StartupStatus::ssCONTINUE_AS_PARENT : StartupStatus::ssFAILED;
    }
}

void CloseEventHandle(HANDLE * phandle) {
    if (*phandle != NULL) {
        CloseHandle(*phandle);
        *phandle = NULL;
    }
}

BOOL QForkShutdown() {
    BOOL success = TRUE;
    if (g_hForkedProcess != NULL) {
        DWORD waitResult = WaitForSingleObject(g_hForkedProcess, 0);
        if (waitResult == WAIT_TIMEOUT) {
            if (!TerminateProcess(g_hForkedProcess, -1) &&
                WaitForSingleObject(g_hForkedProcess, 0) != WAIT_OBJECT_0)
            {
                serverLog(LL_WARNING,
                    "QForkShutdown: could not terminate forked process\n");
                return FALSE;
            }
            waitResult = WaitForSingleObject(g_hForkedProcess,
                                             cDeadForkWait);
        }
        if (waitResult != WAIT_OBJECT_0) {
            serverLog(LL_WARNING,
                "QForkShutdown: forked process did not terminate\n");
            return FALSE;
        }
        CloseHandle(g_hForkedProcess);
        g_hForkedProcess = NULL;
    }

    if (!ReleaseQForkModuleSnapshot()) success = FALSE;

    {
        QForkHeapLockGuard lock;

        if (g_pQForkControl != NULL)
        {
            g_QForkHeapReady = FALSE;
            CloseEventHandle(&g_pQForkControl->operationComplete);
            CloseEventHandle(&g_pQForkControl->operationFailed);

            for (int i = 0; i < g_pQForkControl->numMappedBlocks; i++) {
                if (g_pQForkControl->heapBlockList[i].heapMap != NULL) {
                    CloseEventHandle(
                        &g_pQForkControl->heapBlockList[i].heapMap);
                }
            }

            if (g_pQForkControl->heapStart != NULL) {
                UnmapViewOfFile(g_pQForkControl->heapStart);
                g_pQForkControl->heapStart = NULL;
            }

            UnmapViewOfFile(g_pQForkControl);
            g_pQForkControl = NULL;

            CloseEventHandle(&g_hQForkControlFileMap);
        }
    }

    return success;
}

void CopyForkOperationData(OperationType type, LPVOID redisData, int redisDataSize, uint8_t *dictHashSeed, LPVOID modules) {
    if (redisDataSize > MAX_REDIS_DATA_SIZE) {
        throw runtime_error("Global redis data too large.");
    }

    size_t usedMemory = zmalloc_used_memory();
    DWORD protectError = ERROR_SUCCESS;
    const char *protectOperation = NULL;

    {
        QForkHeapLockGuard lock;

        // Copy operation data while allocator workers cannot change the heap map.
        g_pQForkControl->typeOfOperation = type;
        memcpy(&(g_pQForkControl->globalData.redisData), redisData, redisDataSize);
        g_pQForkControl->globalData.redisDataSize = redisDataSize;
        ACLGetForkData(&g_pQForkControl->globalData.acl);
        RedisGetCoreForkData(&g_pQForkControl->globalData.core);
        size_t sharedDataSize = RedisSharedForkDataSize();
        if (sharedDataSize > sizeof(g_pQForkControl->globalData.sharedData))
            throw runtime_error("Global Redis shared data too large.");
        if (!RedisCopySharedForkData(
                g_pQForkControl->globalData.sharedData, sharedDataSize))
            throw runtime_error("Could not copy global Redis shared data.");
        g_pQForkControl->globalData.sharedDataSize = sharedDataSize;
        memcpy(&g_pQForkControl->globalData.modules, modules,
               sizeof(g_pQForkControl->globalData.modules));
        g_pQForkControl->globalData.usedMemory = usedMemory;
        memcpy(&(g_pQForkControl->globalData.dictHashSeed), dictHashSeed,
               sizeof(g_pQForkControl->globalData.dictHashSeed));

        // Protect the qfork control map from propagating local changes.
        DWORD oldProtect = 0;
        if (!VirtualProtect(g_pQForkControl, sizeof(QForkControl),
                            PAGE_WRITECOPY, &oldProtect)) {
            protectError = GetLastError();
            protectOperation =
                "CopyForkOperationData: VirtualProtect failed for QForkControl";
        }

        // Protect the heap map from propagating local changes.
        for (int i = 0;
             protectError == ERROR_SUCCESS &&
                 i < g_pQForkControl->numMappedBlocks;
             i++) {
            if (g_pQForkControl->heapBlockList[i].state ==
                    BlockState::bsMAPPED_IN_USE) {
                oldProtect = 0;
                if (!VirtualProtect(
                        (byte*) g_pQForkControl->heapStart +
                            i * cAllocationGranularity,
                        cAllocationGranularity,
                        PAGE_WRITECOPY,
                        &oldProtect)) {
                    protectError = GetLastError();
                    protectOperation =
                        "CopyForkOperationData: VirtualProtect failed for heap block";
                }
            }
        }
    }

    if (protectError != ERROR_SUCCESS) {
        throw system_error(protectError, system_category(), protectOperation);
    }
}

void CreateChildProcess(PROCESS_INFORMATION *pi, DWORD dwCreationFlags = 0) {
    // Ensure events are in the correst state
    IFFAILTHROW(ResetEvent(g_pQForkControl->operationComplete),
        "CreateChildProcess: ResetEvent() failed.");
    IFFAILTHROW(ResetEvent(g_pQForkControl->operationFailed),
        "CreateChildProcess: ResetEvent() failed.");

    // Launch the "forked" process through the UTF-16 process boundary.
    wchar_t *modulePath = win32_get_module_filename_wide();
    if (modulePath == NULL) {
        throw system_error(errno, generic_category(),
                           "Failed to get module name");
    }
    wstring fileName(modulePath);
    win32_free(modulePath);

    vector<wstring> childArguments;
    childArguments.push_back(fileName);
    childArguments.push_back(L"--" + QForkUtf8ToWide(cQFork.c_str()));
    childArguments.push_back(to_wstring((uint64_t)g_hQForkControlFileMap));
    childArguments.push_back(to_wstring((unsigned long)GetCurrentProcessId()));
    childArguments.push_back(L"--" + QForkUtf8ToWide(cLogfile.c_str()));
    childArguments.push_back(QForkUtf8ToWide(getLogFilename()));
    wstring commandLine = QForkBuildCommandLine(childArguments);
    vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    IFFAILTHROW(CreateProcessW(fileName.c_str(), mutableCommandLine.data(),
        NULL, NULL, TRUE, dwCreationFlags, NULL, NULL, &si, pi),
        "Problem creating slave process");
    g_hForkedProcess = pi->hProcess;
}

typedef void(*CHILD_PID_HOOK)(DWORD pid);

pid_t BeginForkOperation(OperationType type,
    LPVOID redisData,
    int redisDataSize,
    uint8_t *dictHashSeed,
    LPVOID modules)
{
    PROCESS_INFORMATION pi;
    pi.hProcess = INVALID_HANDLE_VALUE;
    pi.hThread = INVALID_HANDLE_VALUE;
    pi.dwProcessId = -1;
    bool mapsMayBeProtected = false;
    try {
        PrepareQForkModuleSnapshot();
        CreateChildProcess(&pi, CREATE_SUSPENDED);
        if (type == OperationType::otSocket) {
            BeginForkOperation_Socket_Duplicate(pi.dwProcessId);
        }
        /* CopyForkOperationData can fail after protecting only part of the
         * QFork maps, so every exception from this point needs full rejoin. */
        mapsMayBeProtected = true;
        CopyForkOperationData(type, redisData, redisDataSize, dictHashSeed, modules);
        IFFAILTHROW(ResumeThread(pi.hThread) != (DWORD)-1,
            "Problem resuming QFork child process");

        CloseHandle(pi.hThread);
        pi.hThread = INVALID_HANDLE_VALUE;

        return pi.dwProcessId;
    }
    catch (const system_error& syserr) {
        serverLog(LL_WARNING, "BeginForkOperation: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
    }
    catch (const runtime_error& runerr) {
        serverLog(LL_WARNING, "BeginForkOperation: runtime error caught. message=%s\n", runerr.what());
    }
    catch (...) {
        serverLog(LL_WARNING, "BeginForkOperation: other exception caught.\n");
    }
    if (pi.hThread != INVALID_HANDLE_VALUE) {
        CloseHandle(pi.hThread);
        pi.hThread = INVALID_HANDLE_VALUE;
    }
    if (pi.hProcess != INVALID_HANDLE_VALUE) {
        if (mapsMayBeProtected) {
            AbortForkOperation();
        }
        else {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 0);
            if (waitResult == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                waitResult = WaitForSingleObject(pi.hProcess,
                                                 cDeadForkWait);
            }
            if (waitResult != WAIT_OBJECT_0) {
                serverLog(LL_WARNING,
                    "BeginForkOperation: child cleanup did not complete\n");
                exit(1);
            }
            CloseHandle(pi.hProcess);
            if (g_hForkedProcess == pi.hProcess) g_hForkedProcess = 0;
            if (!ReleaseQForkModuleSnapshot()) {
                serverLog(LL_WARNING,
                    "BeginForkOperation: could not release module snapshot\n");
            }
        }
    }
    else {
        if (!ReleaseQForkModuleSnapshot()) {
            serverLog(LL_WARNING,
                "BeginForkOperation: could not release module snapshot\n");
        }
    }
    return -1;
}

static void SetRdbOperationData(int req, const void *rdbSaveInfo,
    size_t rdbSaveInfoSize, int rdbFlags)
{
    if (rdbSaveInfoSize > MAX_RDB_SAVE_INFO_SIZE ||
        (rdbSaveInfoSize != 0 && rdbSaveInfo == NULL))
        throw runtime_error("RDB save metadata is too large or missing");

    g_pQForkControl->globalData.rdb_req = req;
    g_pQForkControl->globalData.rdb_flags = rdbFlags;
    g_pQForkControl->globalData.rdb_save_info_size = rdbSaveInfoSize;
    memset(g_pQForkControl->globalData.rdb_save_info, 0,
           sizeof(g_pQForkControl->globalData.rdb_save_info));
    if (rdbSaveInfoSize != 0)
        memcpy(g_pQForkControl->globalData.rdb_save_info,
               rdbSaveInfo, rdbSaveInfoSize);
}

pid_t BeginForkOperation_Rdb(int req, char *filename,
    const void *rdbSaveInfo, size_t rdbSaveInfoSize, int rdbFlags,
    LPVOID redisData,
    int redisDataSize,
    uint8_t *dictHashSeed,
    LPVOID modules)
{
    SetRdbOperationData(req, rdbSaveInfo, rdbSaveInfoSize, rdbFlags);
    strcpy_s(g_pQForkControl->globalData.filename, filename);
    return BeginForkOperation(otRDB, redisData, redisDataSize, dictHashSeed, modules);
}

pid_t BeginForkOperation_Aof(char *filename,
    LPVOID redisData,
    int redisDataSize,
    uint8_t *dictHashSeed,
    LPVOID modules)
{
    strcpy_s(g_pQForkControl->globalData.filename, filename);
    return BeginForkOperation(otAOF, redisData, redisDataSize, dictHashSeed, modules);
}

void BeginForkOperation_Socket_Duplicate(DWORD dwProcessId) {
    if (FDAPI_WSADuplicateSocket(
            g_pQForkControl->globalData.rdb_pipe_write_fd,
            dwProcessId,
            &g_pQForkControl->globalData.rdb_pipe_write_protocol_info) == SOCKET_ERROR)
        throw system_error(FDAPI_WSAGetLastError(), system_category(),
            "Could not duplicate diskless RDB pipe writer into QFork child");

    if (FDAPI_WSADuplicateSocket(
            g_pQForkControl->globalData.rdb_child_exit_pipe_read_fd,
            dwProcessId,
            &g_pQForkControl->globalData.rdb_child_exit_pipe_read_protocol_info) == SOCKET_ERROR)
        throw system_error(FDAPI_WSAGetLastError(), system_category(),
            "Could not duplicate diskless RDB exit pipe reader into QFork child");
}

pid_t BeginForkOperation_Socket(int req, const void *rdbSaveInfo,
    size_t rdbSaveInfoSize, int rdb_pipe_write_fd,
    int safe_to_exit_pipe_fd,
    LPVOID redisData,
    int redisDataSize,
    uint8_t *dictHashSeed,
    LPVOID modules)
{
    SetRdbOperationData(req, rdbSaveInfo, rdbSaveInfoSize, 0);
    g_pQForkControl->globalData.rdb_pipe_write_fd = rdb_pipe_write_fd;
    g_pQForkControl->globalData.rdb_child_exit_pipe_read_fd =
        safe_to_exit_pipe_fd;

    return BeginForkOperation(otSocket, redisData, redisDataSize, dictHashSeed, modules);
}

OperationStatus GetForkOperationStatus() {
    if (WaitForSingleObject(g_pQForkControl->operationComplete, 0) == WAIT_OBJECT_0) {
        return OperationStatus::osCOMPLETE;
    }

    if (WaitForSingleObject(g_pQForkControl->operationFailed, 0) == WAIT_OBJECT_0) {
        return OperationStatus::osFAILED;
    }

    if (g_hForkedProcess) {
        // Verify if the child process is still running
        if (WaitForSingleObject(g_hForkedProcess, 0) == WAIT_OBJECT_0) {
            // The child process is not running, close the handle and report the status
            // setting the operationFailed event
            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
            if (g_pQForkControl->operationFailed != NULL) {
                SetEvent(g_pQForkControl->operationFailed);
            }
            return OperationStatus::osFAILED;
        }
        else {
            return OperationStatus::osINPROGRESS;
        }
    }

    return OperationStatus::osUNSTARTED;
}

BOOL AbortForkOperation() {
    /* Main-thread-only: worker suspension and resume are coordinated by the
     * Redis event-loop thread. */
    try {
        if (g_hForkedProcess != 0)
        {
            DWORD wait_result = WaitForSingleObject(g_hForkedProcess, 0);
            if (wait_result == WAIT_TIMEOUT &&
                !TerminateProcess(g_hForkedProcess, 1) &&
                WaitForSingleObject(g_hForkedProcess, 0) != WAIT_OBJECT_0)
            {
                throw system_error(GetLastError(), system_category(),
                    "AbortForkOperation: killing forked process failed");
            }
            if (wait_result == WAIT_FAILED) {
                throw system_error(GetLastError(), system_category(),
                    "AbortForkOperation: checking forked process failed");
            }
            if (wait_result == WAIT_TIMEOUT) {
                DWORD exit_wait = WaitForSingleObject(g_hForkedProcess,
                                                       cDeadForkWait);
                if (exit_wait != WAIT_OBJECT_0) {
                    DWORD error = exit_wait == WAIT_FAILED ?
                        GetLastError() : ERROR_TIMEOUT;
                    throw system_error(error, system_category(),
                        "AbortForkOperation: waiting for forked process failed");
                }
            }
            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
        }

        /* EndForkOperation unmaps and remaps mapped heap views. The normal
         * completion path suspends all worker threads before doing that; an
         * explicit abort must provide the same safety guarantee. */
        RequestSuspension();
        ULONGLONG suspensionDeadline = GetTickCount64() + cDeadForkWait;
        while (!SuspensionCompleted()) {
            if (GetTickCount64() >= suspensionDeadline) {
                throw system_error(ERROR_TIMEOUT, system_category(),
                    "AbortForkOperation: worker suspension timed out");
            }
            Sleep(1);
        }
        BOOL result = EndForkOperation(NULL);
        ResumeFromSuspension();
        return result;
    }
    catch (const system_error& syserr) {
        serverLog(LL_WARNING, "AbortForkOperation: 0x%08x - %s\n", syserr.code().value(), syserr.what());
        // If we can not properly restore fork state, then another fork operation is not possible.
        exit(1);
    }
    catch (const exception& ex) {
        serverLog(LL_WARNING, "AbortForkOperation: %s\n", ex.what());
        exit(1);
    }
    return FALSE;
}

BOOL RejoinCOWPages(HANDLE mmHandle, byte* mmStart, size_t mmSize,
                    DWORD *error, const char **operation) {
    *error = ERROR_SUCCESS;
    *operation = NULL;

    byte *copyView = (byte*) MapViewOfFile(
        mmHandle, FILE_MAP_WRITE, 0, 0, mmSize);
    if (copyView == NULL) {
        *error = GetLastError();
        *operation = "RejoinCOWPages: Could not map COW back-copy view";
        return FALSE;
    }

    for (byte* mmAddress = mmStart; mmAddress < mmStart + mmSize; ) {
        MEMORY_BASIC_INFORMATION memInfo;

        if (!VirtualQuery(mmAddress, &memInfo, sizeof(memInfo))) {
            *error = GetLastError();
            *operation = "RejoinCOWPages: VirtualQuery failure";
            break;
        }

        byte* regionEnd = (byte*) memInfo.BaseAddress + memInfo.RegionSize;

        if (memInfo.Protect != PAGE_WRITECOPY) {
            // Copy back only the pages that have been copied on write
            byte* srcEnd = min(regionEnd, mmStart + mmSize);
            memcpy(copyView + (mmAddress - mmStart), mmAddress, srcEnd - mmAddress);
        }
        mmAddress = regionEnd;
    }

    // If the COWs are not discarded, then there is no way of propagating
    // changes into subsequent fork operations.
#if FALSE
    // This works when using a memory mapped file but it fails when using
    // the system paging file.
    if (WindowsVersion::getInstance().IsAtLeast_6_2()) {
        // Restores all page protections on the view and culls the COW pages.
        DWORD oldProtect;
        IFFAILTHROW(VirtualProtect(mmStart, mmSize, PAGE_READWRITE | PAGE_REVERT_TO_FILE_MAP, &oldProtect),
            "RejoinCOWPages: COW cull failed");
    }
    else
#endif
    {
        // Prior to Win8 unmapping the view was the only way to discard the
        // COW pages from the view. Unfortunately this forces the view to be
        // completely flushed to disk, which is a bit inefficient.
        if (*error == ERROR_SUCCESS && !UnmapViewOfFile(mmStart)) {
            *error = GetLastError();
            *operation = "RejoinCOWPages: UnmapViewOfFile failed";
        }

        // There is a possible race condition here. Something could map into
        // the virtual address space used by the heap at the moment we are
        // discarding local changes. There is nothing to do but report the
        // problem and exit. This problem does not exist with the code above
        // in Win8+ as the view is never unmapped.
        if (*error == ERROR_SUCCESS) {
            byte *remappedView = (byte*) MapViewOfFileEx(
                mmHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0, mmStart);
            if (remappedView != mmStart) {
                *error = remappedView == NULL ?
                    GetLastError() : ERROR_INVALID_ADDRESS;
                *operation = "RejoinCOWPages: MapViewOfFileEx failed";
                if (remappedView != NULL) {
                    UnmapViewOfFile(remappedView);
                }
            }
        }
    }

    if (!UnmapViewOfFile(copyView) && *error == ERROR_SUCCESS) {
        *error = GetLastError();
        *operation = "RejoinCOWPages: could not unmap COW back-copy view";
    }

    return *error == ERROR_SUCCESS;
}

BOOL EndForkOperation(int * pExitCode) {
    try {
        if (g_hForkedProcess != 0) {
            DWORD waitResult = WaitForSingleObject(g_hForkedProcess,
                                                   cDeadForkWait);
            if (waitResult == WAIT_TIMEOUT) {
                IFFAILTHROW(TerminateProcess(g_hForkedProcess, 1),
                    "EndForkOperation: Killing forked process failed.");
                waitResult = WaitForSingleObject(g_hForkedProcess,
                                                 cDeadForkWait);
            }
            if (waitResult != WAIT_OBJECT_0) {
                DWORD error = waitResult == WAIT_FAILED ?
                    GetLastError() : ERROR_TIMEOUT;
                throw system_error(error, system_category(),
                    "EndForkOperation: waiting for forked process failed");
            }

            if (pExitCode != NULL) {
                IFFAILTHROW(GetExitCodeProcess(g_hForkedProcess,
                    (DWORD*) pExitCode),
                    "EndForkOperation: could not get child exit code");
            }

            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
        }

        IFFAILTHROW(ResetEvent(g_pQForkControl->operationComplete),
            "EndForkOperation: ResetEvent() failed.");
        IFFAILTHROW(ResetEvent(g_pQForkControl->operationFailed),
            "EndForkOperation: ResetEvent() failed.");

        DWORD rejoinError = ERROR_SUCCESS;
        const char *rejoinOperation = NULL;
        {
            QForkHeapLockGuard lock;

            // Move the heap local changes back into memory mapped views for
            // the next fork operation.
            for (int i = 0; i < g_pQForkControl->numMappedBlocks; i++) {
                /* A block protected while in use can be freed before the
                 * child exits. It still has private COW pages that must be
                 * merged before the block is reused or snapshotted again. */
                if (g_pQForkControl->heapBlockList[i].heapMap != NULL) {
                    if (!RejoinCOWPages(
                        g_pQForkControl->heapBlockList[i].heapMap,
                        (byte*) g_pQForkControl->heapStart +
                            cAllocationGranularity * i,
                        cAllocationGranularity,
                        &rejoinError,
                        &rejoinOperation)) {
                        break;
                    }
                }
            }

            if (rejoinError == ERROR_SUCCESS) {
                RejoinCOWPages(g_hQForkControlFileMap,
                               (byte*) g_pQForkControl,
                               sizeof(QForkControl),
                               &rejoinError,
                               &rejoinOperation);
            }
        }

        if (rejoinError != ERROR_SUCCESS) {
            throw system_error(rejoinError, system_category(),
                               rejoinOperation);
        }

        if (!ReleaseQForkModuleSnapshot()) {
            throw runtime_error(
                "EndForkOperation: could not release module snapshot");
        }

        return TRUE;
    }
    catch (const system_error& syserr) {
        serverLog(LL_WARNING, "EndForkOperation: 0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state, then another fork operation is not possible.
        exit(1);
    }
    catch (const exception& ex) {
        serverLog(LL_WARNING, "EndForkOperation: %s\n", ex.what());
        exit(1);
    }
    return FALSE;
}

struct BlockMapFailure {
    DWORD error;
    const char *operation;
    BOOL recoveryFailed;
};

HANDLE CreateBlockMap(int blockIndex, BlockMapFailure *failure) {
    HANDLE map = NULL;
    LPVOID realAddr = NULL;
    LPVOID addr = (byte*) g_pQForkControl->heapStart +
        blockIndex * cAllocationGranularity;
    BOOL reservationReleased = FALSE;

    failure->error = ERROR_SUCCESS;
    failure->operation = NULL;
    failure->recoveryFailed = FALSE;

    // cAllocationGranularity is guaranteed to be < 2^31.
    ASSERT(cAllocationGranularity < (1 << 31));
    map = CreateFileMappingW(INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        cAllocationGranularity,
        NULL);
    if (map == NULL) {
        failure->error = GetLastError();
        failure->operation = "PhysicalMapMemory: CreateFileMapping failed";
        return NULL;
    }

    // Free the memory that was reserved in QForkParentInit() before mapping it.
    if (!VirtualFree(addr, 0, MEM_RELEASE)) {
        failure->error = GetLastError();
        failure->operation = "PhysicalMapMemory: VirtualFree failed";
        CloseHandle(map);
        return NULL;
    }
    reservationReleased = TRUE;

    realAddr = MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, 0, addr);
    if (realAddr == NULL) {
        failure->error = GetLastError();
        failure->operation = "PhysicalMapMemory: MapViewOfFileEx failed";
    }
    else if (realAddr != addr) {
        failure->error = ERROR_INVALID_ADDRESS;
        failure->operation =
            "PhysicalMapMemory: MapViewOfFileEx returned the wrong address";
    }
    else {
        DWORD old;
        if (!VirtualProtect(realAddr, cAllocationGranularity,
                            PAGE_READWRITE, &old)) {
            failure->error = GetLastError();
            failure->operation = "PhysicalMapMemory: VirtualProtect failed";
        }
        else {
            return map;
        }
    }

    if (realAddr != NULL && !UnmapViewOfFile(realAddr)) {
        failure->recoveryFailed = TRUE;
        if (failure->error == ERROR_SUCCESS) {
            failure->error = GetLastError();
            failure->operation =
                "PhysicalMapMemory: cleanup UnmapViewOfFile failed";
        }
    }
    if (map != NULL) {
        CloseHandle(map);
    }
    if (reservationReleased) {
        LPVOID reserved = VirtualAlloc(addr, cAllocationGranularity,
            MEM_RESERVE, PAGE_READWRITE);
        if (reserved != addr) {
            if (reserved != NULL) {
                VirtualFree(reserved, 0, MEM_RELEASE);
            }
            g_pQForkControl->heapBlockList[blockIndex].state =
                BlockState::bsINVALID;
            failure->recoveryFailed = TRUE;
        }
    }

    return NULL;
}

BOOL RollBackNewBlockMap(int blockIndex) {
    LPVOID addr = (byte*) g_pQForkControl->heapStart +
        blockIndex * cAllocationGranularity;
    HANDLE map = g_pQForkControl->heapBlockList[blockIndex].heapMap;
    BOOL success = TRUE;

    if (map == NULL) return TRUE;

    if (!UnmapViewOfFile(addr)) {
        success = FALSE;
    }
    CloseHandle(map);
    g_pQForkControl->heapBlockList[blockIndex].heapMap = NULL;

    LPVOID reserved = VirtualAlloc(addr, cAllocationGranularity,
        MEM_RESERVE, PAGE_READWRITE);
    if (reserved != addr) {
        if (reserved != NULL) {
            VirtualFree(reserved, 0, MEM_RELEASE);
        }
        g_pQForkControl->heapBlockList[blockIndex].state =
            BlockState::bsINVALID;
        success = FALSE;
    }

    return success;
}

LPVOID AllocHeapBlock(LPVOID addr, size_t size, BOOL zero) {
    if (g_BypassMemoryMapOnAlloc) {
        return VirtualAlloc(addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    size_t requestedBlocks = 0;
    LPVOID retPtr = NULL;
    BlockMapFailure mapFailure = {};
    int failedBlockIndex = -1;
    BOOL rollbackFailed = FALSE;

    {
        QForkHeapLockGuard lock;

        if (g_pQForkControl == NULL || !g_QForkHeapReady) {
            return VirtualAlloc(addr, size, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE);
        }

        if (size % cAllocationGranularity != 0) {
            errno = EINVAL;
            return NULL;
        }

        // Reject impossible sizes before narrowing the block count to int. A
        // request larger than the reserved heap can never be satisfied, and
        // the cast below would otherwise wrap. Returning NULL lets jemalloc
        // propagate ENOMEM instead of returning an unbacked extent.
        requestedBlocks = size / cAllocationGranularity;
        if (requestedBlocks == 0 ||
            requestedBlocks >
                (size_t) g_pQForkControl->maxAvailableBlocks) {
            errno = ENOMEM;
            return NULL;
        }

        int contiguousBlocksToAllocate = (int) requestedBlocks;

        int startSearch;
        int endSearch;

        if (addr != NULL) {
            uintptr_t heapStart = reinterpret_cast<uintptr_t>(
                g_pQForkControl->heapStart);
            uintptr_t requestedAddress = reinterpret_cast<uintptr_t>(addr);
            if (requestedAddress < heapStart) {
                errno = ENOMEM;
                return NULL;
            }

            size_t offset = (size_t) (requestedAddress - heapStart);
            if ((offset % cAllocationGranularity) != 0) {
                errno = EINVAL;
                return NULL;
            }

            size_t requestedStart = offset / cAllocationGranularity;
            if (requestedStart >
                    (size_t) g_pQForkControl->maxAvailableBlocks -
                        requestedBlocks) {
                errno = ENOMEM;
                return NULL;
            }
            startSearch = (int) requestedStart;
            endSearch = startSearch;
        }
        else {
            startSearch = g_pQForkControl->blockSearchStart;
            endSearch = g_pQForkControl->maxAvailableBlocks -
                contiguousBlocksToAllocate;
        }

        int contiguousBlocksFound = 0;
        int allocationStartIndex = 0;

        for (int startIdx = startSearch; startIdx <= endSearch; startIdx++) {
            contiguousBlocksFound = 0;
            for (int i = 0; i < contiguousBlocksToAllocate; i++) {
                BlockState state =
                    g_pQForkControl->heapBlockList[startIdx + i].state;
                if (state == BlockState::bsUNMAPPED ||
                    state == BlockState::bsMAPPED_FREE) {
                    contiguousBlocksFound++;
                }
                else {
                    if (addr == NULL) startIdx += i;
                    break;
                }
            }
            if (contiguousBlocksFound == contiguousBlocksToAllocate) {
                allocationStartIndex = startIdx;
                break;
            }
        }

        if (contiguousBlocksFound != contiguousBlocksToAllocate) {
            errno = ENOMEM;
            return NULL;
        }

        ASSERT(allocationStartIndex + contiguousBlocksToAllocate <=
               g_pQForkControl->maxAvailableBlocks);

        /* Map every previously unused block before changing allocator state.
         * If any map fails, undo maps created for this request while the same
         * heap lock is held. */
        for (int i = 0; i < contiguousBlocksToAllocate; i++) {
            int index = allocationStartIndex + i;
            if (g_pQForkControl->heapBlockList[index].state ==
                    BlockState::bsUNMAPPED) {
                HANDLE map = CreateBlockMap(index, &mapFailure);
                if (map == NULL) {
                    failedBlockIndex = index;
                    for (int j = 0; j < i; j++) {
                        int rollbackIndex = allocationStartIndex + j;
                        if (g_pQForkControl->heapBlockList[rollbackIndex].state ==
                                BlockState::bsUNMAPPED &&
                            !RollBackNewBlockMap(rollbackIndex)) {
                            rollbackFailed = TRUE;
                        }
                    }
                    break;
                }
                g_pQForkControl->heapBlockList[index].heapMap = map;
            }
        }

        if (failedBlockIndex == -1) {
            for (int i = 0; i < contiguousBlocksToAllocate; i++) {
                int index = allocationStartIndex + i;
                if (g_pQForkControl->heapBlockList[index].state ==
                        BlockState::bsUNMAPPED) {
                    g_pQForkControl->numMappedBlocks = max(
                        g_pQForkControl->numMappedBlocks, index + 1);
                }
                else if (zero) {
                    // Reused mapped blocks need explicit zeroing.
                    LPVOID ptr = reinterpret_cast<byte*>(
                        g_pQForkControl->heapStart) +
                        cAllocationGranularity * index;
                    ZeroMemory(ptr, cAllocationGranularity);
                }
                g_pQForkControl->heapBlockList[index].state =
                    BlockState::bsMAPPED_IN_USE;
            }

            retPtr = reinterpret_cast<byte*>(g_pQForkControl->heapStart) +
                cAllocationGranularity * allocationStartIndex;
            if (addr == NULL &&
                allocationStartIndex ==
                    g_pQForkControl->blockSearchStart) {
                g_pQForkControl->blockSearchStart =
                    allocationStartIndex + contiguousBlocksToAllocate;
            }
        }
    }

    if (failedBlockIndex != -1) {
        if (mapFailure.operation != NULL) {
            serverLog(LL_WARNING,
                "%s for heap block %d: system error 0x%08x",
                mapFailure.operation, failedBlockIndex, mapFailure.error);
        }
        if (mapFailure.recoveryFailed || rollbackFailed) {
            serverLog(LL_WARNING,
                "PhysicalMapMemory: QFork heap reservation recovery failed; exiting");
            exit(1);
        }
        errno = ENOMEM;
        return NULL;
    }

    return retPtr;
}

BOOL FreeHeapBlock(LPVOID addr, size_t size) {
    if (size == 0) {
        return FALSE;
    }

    QForkHeapLockGuard lock;

    if (g_pQForkControl == NULL || !g_QForkHeapReady ||
        !g_HasMemoryMappedHeap) {
        return VirtualFree(addr, 0, MEM_RELEASE);
    }

    // Check if the address belongs to the memory map heap or to the system heap.
    uintptr_t address = reinterpret_cast<uintptr_t>(addr);
    uintptr_t heapStart = reinterpret_cast<uintptr_t>(
        g_pQForkControl->heapStart);
    uintptr_t heapEnd = reinterpret_cast<uintptr_t>(
        g_pQForkControl->heapEnd);
    BOOL addressInRedisHeap = address >= heapStart && address < heapEnd;
    if (!addressInRedisHeap) {
        return VirtualFree(addr, 0, MEM_RELEASE);
    }

    /* The QFork child bypasses the mapped heap for new allocations, and the
     * system allocator may place one of those allocations in an unused hole
     * inside the parent's heap address range. Distinguish private VirtualAlloc
     * extents from inherited MapViewOfFile extents before applying QFork block
     * validation. */
    if (g_BypassMemoryMapOnAlloc) {
        MEMORY_BASIC_INFORMATION memInfo;
        if (!VirtualQuery(addr, &memInfo, sizeof(memInfo))) {
            return FALSE;
        }
        if (memInfo.Type != MEM_MAPPED) {
            return VirtualFree(addr, 0, MEM_RELEASE);
        }
    }

    if ((size % cAllocationGranularity) != 0) {
        return FALSE;
    }

    size_t contiguousBlocksToFree = size / cAllocationGranularity;
    if (contiguousBlocksToFree == 0 ||
        contiguousBlocksToFree >
            (size_t) g_pQForkControl->maxAvailableBlocks) {
        return FALSE;
    }

    size_t ptrDiff = (size_t) (address - heapStart);
    if ((ptrDiff % cAllocationGranularity) != 0) {
        return FALSE;
    }

    size_t blockStartIndex = ptrDiff / cAllocationGranularity;
    if (blockStartIndex >
            (size_t) g_pQForkControl->maxAvailableBlocks -
                contiguousBlocksToFree) {
        return FALSE;
    }

    for (size_t i = 0; i < contiguousBlocksToFree; i++) {
        size_t index = blockStartIndex + i;
        if (index >= (size_t) g_pQForkControl->numMappedBlocks ||
            g_pQForkControl->heapBlockList[index].state !=
                BlockState::bsMAPPED_IN_USE ||
            g_pQForkControl->heapBlockList[index].heapMap == NULL) {
            return FALSE;
        }
    }

    for (size_t i = 0; i < contiguousBlocksToFree; i++) {
        g_pQForkControl->heapBlockList[blockStartIndex + i].state = BlockState::bsMAPPED_FREE;
    }

    // TODO: use a linked list of free blocks

    if ((size_t) g_pQForkControl->blockSearchStart > blockStartIndex) {
        g_pQForkControl->blockSearchStart = (int) blockStartIndex;
    }

    return TRUE;
}

BOOL PurgePages(LPVOID addr, size_t length) {
    // VirtualAlloc is called for all cases regardless the value of
    // g_BypassMemoryMapOnAlloc and g_HasMemoryMappedHeap
    QForkHeapLockGuard lock;
    VirtualAlloc(addr, length, MEM_RESET, PAGE_READWRITE);
    return TRUE;
}

void SetupLogging() {
    bool serviceRun = g_argMap.find(cServiceRun) != g_argMap.end();
    string syslogEnabledValue = (g_argMap.find(cSyslogEnabled) != g_argMap.end() ? g_argMap[cSyslogEnabled].at(0).at(0) : cNo);
    bool syslogEnabled = (syslogEnabledValue.compare(cYes) == 0) || serviceRun;
    string syslogIdent = (g_argMap.find(cSyslogIdent) != g_argMap.end() ? g_argMap[cSyslogIdent].at(0).at(0) : cDefaultSyslogIdent);
    string logFileName = (g_argMap.find(cLogfile) != g_argMap.end() ? g_argMap[cLogfile].at(0).at(0) : cDefaultLogfile);

    RedisEventLog().EnableEventLog(syslogEnabled);
    if (syslogEnabled) {
        RedisEventLog().SetEventLogIdentity(syslogIdent.c_str());
    }
    else {
        setLogFile(logFileName.c_str());
    }
}

BOOL IsPersistenceDisabled() {
    if (g_argMap.find(cPersistenceAvailable) != g_argMap.end()) {
        return (g_argMap[cPersistenceAvailable].at(0).at(0) == cNo);
    }
    else {
        return FALSE;
    }
}

BOOL IsForkedProcess() {
    if (g_argMap.find(cQFork) != g_argMap.end()) {
        return TRUE;
    }
    else {
        return FALSE;
    }
}

void SetupQForkGlobals(int argc, char* argv[]) {
    // To check sentinel mode we use the antirez code to avoid duplicating code
    g_SentinelMode = checkForSentinelMode(argc, argv, argv[0]);

    g_IsForkedProcess = IsForkedProcess();
    g_PersistenceDisabled = IsPersistenceDisabled();

    g_BypassMemoryMapOnAlloc = g_IsForkedProcess || g_PersistenceDisabled || g_SentinelMode;
    g_HasMemoryMappedHeap = !g_PersistenceDisabled && !g_SentinelMode;
}

static volatile LONG g_mainInvocationDepth;

class MainInvocationGuard {
public:
    MainInvocationGuard() : depth(InterlockedIncrement(&g_mainInvocationDepth)) {}
    ~MainInvocationGuard() { InterlockedDecrement(&g_mainInvocationDepth); }
    bool IsTopLevel() const { return depth == 1; }
private:
    LONG depth;
};

extern "C"
{
    // The external main() is redefined as redis_main() by Win32_QFork.h.
    // The CRT will call this replacement main() before the previous main()
    // is invoked so that the QFork allocator can be setup prior to anything
    // Redis will allocate.
    int main(int argc, char* argv[]) {
        MainInvocationGuard invocation;
        if (invocation.IsTopLevel()) {
            char **utf8Argv = NULL;
            int utf8Argc = 0;
            if (win32_get_utf8_argv(&utf8Argc, &utf8Argv) != 0) {
                fprintf(stderr, "Unable to decode the Windows command line as UTF-8: %s\n",
                        strerror(errno));
                return 1;
            }
            argc = utf8Argc;
            argv = utf8Argv;
            /* This top-level vector intentionally lives until process exit.
             * ServiceWorkerThread recursively invokes this wrapper while the
             * outer SCM dispatcher is still active and supplies its own
             * already-UTF-8 argv. */
        }
        try {
            //[tporadowski/#2] check if started as "redis-check-rdb" tool
            string executable(argv[0]);
            transform(executable.begin(), executable.end(), executable.begin(), ::tolower);
            //copy lowercase back to argv[0] as later on main function from server.c checks for "redis-check-aof"
            //  or "redis-check-rdb" (using case-sensitive checking)
            strncpy(argv[0], executable.c_str(), executable.length());

            g_StartedAsCheckAofOrRdbTool = (executable.find("redis-check-rdb") != string::npos)
                || (executable.find("redis-check-aof") != string::npos);

            InitTimeFunctions();

            //[tporadowski/#2] when running as "redis-check-rdb"/"redis-check-aof" expected command-line parameter is path to *.rdb/*.aof?
            //  file and not a config file as for "redis-server", so skip this step
            if (!g_StartedAsCheckAofOrRdbTool) {
                ParseCommandLineArguments(argc, argv);
            }

            SetupQForkGlobals(argc, argv);
            SetupLogging();
            StackTraceInit();
            InitThreadControl();
        }
        catch (const system_error& syserr) {
            string errMsg = string("System error during startup: ") + syserr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }
        catch (const runtime_error& runerr) {
            string errMsg = string("System error during startup: ") + runerr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }
        catch (invalid_argument &iaerr) {
            string errMsg = string("Invalid argument during startup: ") + iaerr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }
        catch (const exception& othererr) {
            string errMsg = string("An exception occurred during startup: ") + othererr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }

        try {
#ifdef DEBUG_WITH_PROCMON
            hProcMonDevice =
                CreateFileW(
                    L"\\\\.\\Global\\ProcmonDebugLogger",
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL);
#endif

            // Service commands do not launch an instance of redis directly
            if (HandleServiceCommands(argc, argv) == TRUE) {
                return 0;
            }

            if (g_PersistenceDisabled || g_SentinelMode) {
                return redis_main(argc, argv);
            }
            else {
                StartupStatus status = QForkStartup();
                if (status == ssCONTINUE_AS_PARENT) {
                    int retval = redis_main(argc, argv);
                    QForkShutdown();
                    return retval;
                }
                else if (status == ssCHILD_EXIT) {
                    // Child is done - clean up and exit
                    QForkShutdown();
                    return g_ChildExitCode;
                }
                else if (status == ssFAILED) {
                    // Parent or child failed initialization
                    return 1;
                }
                else {
                    // Unexpected status return
                    return 2;
                }
            }
    }
        catch (const system_error& syserr) {
            RedisEventLog().LogError(string("Main: system error. ") + syserr.what());
            serverLog(LL_WARNING, "main: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
        }
        catch (const runtime_error& runerr) {
            RedisEventLog().LogError(string("Main: runtime error. ") + runerr.what());
            serverLog(LL_WARNING, "main: runtime error caught. message=%s\n", runerr.what());
        }
        catch (const exception& ex) {
            RedisEventLog().LogError(string("Main: an exception occurred. ") + ex.what());
            serverLog(LL_WARNING, "main: other exception caught.\n");
        }
}
}
