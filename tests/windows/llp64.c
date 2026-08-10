#include "server.h"

#include <stdint.h>

_Static_assert(sizeof(dict_ulong) == sizeof(uint64_t),
               "Win64 dictionary sizes and cursors must be 64-bit");
_Static_assert(sizeof(((dict *)0)->ht[0].used) == sizeof(uint64_t),
               "Win64 dictionary used counts must be 64-bit");
_Static_assert(sizeof(((dict *)0)->ht[0].size) == sizeof(uint64_t),
               "Win64 dictionary sizes must be 64-bit");
_Static_assert(sizeof(((dict *)0)->rehashidx) == sizeof(int64_t),
               "Win64 dictionary rehash indexes must be 64-bit");
_Static_assert(sizeof(((quicklist *)0)->count) == sizeof(uint64_t),
               "Win64 quicklist entry counts must be 64-bit");
_Static_assert(sizeof(((quicklist *)0)->len) == sizeof(uint64_t),
               "Win64 quicklist node counts must be 64-bit");
_Static_assert(sizeof(((zskiplist *)0)->length) == sizeof(uint64_t),
               "Win64 sorted-set lengths must be 64-bit");
_Static_assert(sizeof(((zskiplistNode *)0)->level[0].span) == sizeof(uint64_t),
               "Win64 sorted-set spans must be 64-bit");
_Static_assert(sizeof(((client *)0)->bulklen) == sizeof(int64_t),
               "Win64 protocol bulk lengths must be 64-bit");
_Static_assert(sizeof(((client *)0)->duration) == sizeof(int64_t),
               "Win64 command duration must be 64-bit");
_Static_assert(sizeof(((redisDb *)0)->expires_cursor) == sizeof(uint64_t),
               "Win64 expiration cursors must be 64-bit");
_Static_assert(sizeof(((struct redisServer *)0)->aof_delayed_fsync) == sizeof(uint64_t),
               "Win64 delayed-fsync counters must be 64-bit");
_Static_assert(sizeof(((struct redisServer *)0)->slowlog_max_len) == sizeof(uint64_t),
               "Win64 slowlog limits must be 64-bit");
_Static_assert(sizeof(((struct redisServer *)0)->active_defrag_max_scan_fields) == sizeof(uint64_t),
               "Win64 active-defrag limits must be 64-bit");
_Static_assert(sizeof(((struct redisServer *)0)->acllog_max_len) == sizeof(uint64_t),
               "Win64 ACL log limits must be 64-bit");
_Static_assert(sizeof(unsigned long) == 4,
               "This regression test must run against the Win64 LLP64 ABI");
_Static_assert(sizeof(((struct redisFunctionSym *)0)->pointer) == sizeof(void *),
               "Function symbol addresses must preserve the full pointer width");

int win32_llp64_interop_test(void) {
    const uint64_t high = (UINT64_C(1) << 32) + 17;
    dict d = {0};
    quicklist ql = {0};
    zskiplist zsl = {0};
    client client_widths = {0};
    struct redisServer server_widths = {0};
    struct redisFunctionSym function_symbol = {0};

    d.ht[0].used = high;
    d.ht[0].size = high + 1;
    d.rehashidx = (int64_t)high;
    ql.count = high;
    ql.len = high + 1;
    zsl.length = high + 2;
    client_widths.duration = (ustime_t)(high + 3);
    server_widths.slowlog_max_len = high + 4;
    server_widths.active_defrag_max_scan_fields = high + 5;
    server_widths.acllog_max_len = high + 6;
    server_widths.aof_delayed_fsync = high + 7;
    function_symbol.pointer = (uintptr_t)&function_symbol;

    return d.ht[0].used == high && d.ht[0].size == high + 1 &&
           d.rehashidx == (int64_t)high &&
           ql.count == high && ql.len == high + 1 &&
           zsl.length == high + 2 &&
           client_widths.duration == (ustime_t)(high + 3) &&
           server_widths.slowlog_max_len == high + 4 &&
           server_widths.active_defrag_max_scan_fields == high + 5 &&
           server_widths.acllog_max_len == high + 6 &&
           server_widths.aof_delayed_fsync == high + 7 &&
           function_symbol.pointer == (uintptr_t)&function_symbol;
}
