# Redis 6.2 behavior in the Windows port

## Scope and identity

This is an unofficial Windows x64 port of the Redis 6.2.22 core, based on
upstream commit `b0f7a7f4d97`, combined with this fork's IOCP networking,
QFork persistence, Windows service, Event Log, console, and MinGW64 packaging
layers.

The completed Windows line reports `6.2.22` through `INFO server`, executable
`--version` output, module version APIs, RDB metadata, and the portable package
filename. Retain the ZIP checksum and source commit when identifying an exact
Windows build.

RESP2 remains the default protocol, so existing Redis 5 clients normally keep
working. RESP3, ACLs, client-side caching, and other Redis 6 behavior are
opt-in unless configured.

## Commands, protocol, and security

No Redis 5 top-level command was removed. New top-level commands include:

- Security and protocol: `ACL`, `HELLO`, and `RESET`.
- Strings and algorithms: `BITFIELD_RO`, `GETEX`, `GETDEL`, and `STRALGO`.
- Lists, sets, and hashes: `LPOS`, `LMOVE`, `BLMOVE`, `SMISMEMBER`, and
  `HRANDFIELD`.
- Sorted sets and geospatial indexes: `ZMSCORE`, `ZRANDMEMBER`, `ZDIFF`,
  `ZDIFFSTORE`, `ZINTER`, `ZUNION`, `ZRANGESTORE`, `GEOSEARCH`, and
  `GEOSEARCHSTORE`.
- Streams and operations: `XAUTOCLAIM`, `COPY`, and coordinated `FAILOVER`.

Existing commands also gain Redis 6.2 options, including `SET GET/EXAT/PXAT`,
`ZADD GT/LT`, unified `ZRANGE BYSCORE/BYLEX/REV`, `XADD NOMKSTREAM/MINID/LIMIT`,
additional `XPENDING` ranges and idle filtering, `CLIENT PAUSE WRITE`, and
richer client and Sentinel introspection.

ACLs support named users, command categories, key patterns, Pub/Sub channel
patterns, an in-memory ACL log, external ACL files, and `AUTH username
password`. The legacy `requirepass` option remains a compatibility layer for
the `default` ACL user. `HELLO 3` enables RESP3, including push replies used by
client tracking and Pub/Sub. Client-side caching is available through `CLIENT
TRACKING`; its table is bounded by `tracking-table-max-keys`.

The Windows IOCP listener preserves the address family of IPv4 and IPv6
connections. Protected mode recognizes both `127.0.0.1` and `::1` as loopback,
including when replication or Sentinel hostname resolution selects IPv6 first.

## Compatibility changes from Redis 5

Applications should review these Redis 6 behavior changes:

- `SPOP key count` returns an empty collection, rather than null, when the key
  is missing.
- `GEORADIUS`/`GEORADIUSBYMEMBER` with `STORE` or `STOREDIST` now deletes an
  existing destination and returns zero when the source key is missing.
- `PUBSUB NUMPAT` counts unique pattern strings rather than total pattern
  subscriptions.
- `EXISTS` no longer updates LRU/LFU state, and `OBJECT` does not expose
  logically expired keys.
- Expiration of a watched key can invalidate `EXEC`; `SWAPDB` also invalidates
  affected watches.
- Some `AUTH`, `HELLO`, `SELECT`, and `MOVE` errors changed. Clients should not
  depend on exact error strings.
- `SORT` behavior on writable replicas and some RESP3 reply shapes differ from
  Redis 5.
- A requested bind address that is unavailable now aborts startup unless the
  address is prefixed with `-`, such as `-::1`. Addresses already in use still
  fail startup.
- New or extended commands may be written to AOF or replication streams. After
  using them, an older Redis 5 replica or downgrade target may not understand
  the stream.

The RDB format version remains 9, but always back up RDB, AOF, configuration,
Sentinel, and ACL files before upgrading. Test rollback before using Redis
6-only commands or module data.

The old `SLAVEOF` command and `slave-*` configuration spellings remain aliases.
New configurations should use `REPLICAOF` and `replica-*` names.

## Configuration and defaults

The packaged console and service examples bind to `127.0.0.1`, keep protected
mode enabled, leave AOF disabled, use `tcp-keepalive 300`, and use these
snapshot points:

```text
save 3600 1
save 300 100
save 60 10000
```

Review those values when replacing an older Windows configuration. Important
Redis 6 options include:

- ACLs: `aclfile`, inline `user` rules, `acllog-max-len`, and
  `acl-pubsub-default`.
- Networking and performance: `tracking-table-max-keys`,
  `active-expire-effort`, and `maxmemory-eviction-tenacity`.
- Persistence and replication: `repl-diskless-load`, `rdb-del-sync-files`,
  `sanitize-dump-payload`, `lazyfree-lazy-user-flush`, and `masteruser`.
- Cluster and replica behavior: `cluster-allow-replica-migration`,
  `cluster-allow-reads-when-down`, and `replica-announced`.

`CONFIG REWRITE` needs write access to the configuration file's directory.
The Windows service installer grants its NetworkService account access to the
main configuration/include directories and the configured Redis `dir`.
Service-relative paths resolve from the executable directory at runtime, while
install-time ACL discovery resolves them from the install command's current
directory; install from the executable directory or use absolute paths. Grant
permissions separately for external ACL files, log-file directories, modules,
and other paths. Starting Redis with command-line options does not disable the
built-in `save` defaults; use `--save ""` when persistence must be disabled.

Server multi-I/O-thread mode is unavailable in this Windows build. Startup
rejects `io-threads` other than 1 and `io-threads-do-reads yes`; retain the safe
defaults `io-threads 1` and `io-threads-do-reads no`.

The packaged Windows configuration files are intentionally compact examples,
not exhaustive Redis 6 references. Use `CONFIG GET *` and the source tree's
upstream-aligned `redis.conf` for additional options, while ignoring POSIX-only
settings. That upstream reference file is not part of the portable ZIP.

## Windows text, naming, and filesystem contract

Redis keeps arguments, configuration values, environment values, paths, and
diagnostics as UTF-8. Invalid UTF-8 is rejected; valid strings are converted
to UTF-16 for Windows `W` APIs rather than interpreted through the active code
page. Environment-variable names and the Sentinel/checker executable aliases
follow Windows case-insensitive rules. DNS hostnames and resolver service
labels remain ASCII-only; use punycode (`xn--...`) for international hostnames.
This resolver rule is separate from the Windows SCM `service-name`, which uses
the UTF-8-to-UTF-16 service boundary.

Use ordinary UTF-8 drive-absolute or UNC paths. File operations support Unicode
and internally generated extended-length paths, but raw `\\?\`, `\\.\`, device,
or verbatim namespaces are not a portable configuration contract. Keep the
process `dir` short, absolute, and below `MAX_PATH`, because the global working
directory also affects relative CRT and Win32 operations.

Ordinary Windows volumes are treated case-insensitively and slash styles are
equivalent. Redis does not normalize Unicode composition or qualify
per-directory case-sensitive NTFS mode. Windows `redis_lstat` is stat-like and
does not promise POSIX symlink semantics, so avoid symlinks, junctions, mount
points, and other reparse points in Redis-owned trees. Atomic replacement is
same-volume only and uses write-through `MoveFileExW`; Windows has no direct
equivalent of POSIX directory `fsync()`. Storage caches and transient sharing
violations from antivirus, backup, or indexing software can therefore still
affect crash durability or cause an operation to fail after its bounded retry.

## Persistence and replication

RDB background saves and AOF rewrites use the Windows QFork implementation and
a customized jemalloc 5.2.1 heap-tracking layer. Diskless replication is
supported on both sides: a primary can stream a QFork-generated RDB, and a
replica can use `repl-diskless-load disabled`, `on-empty-db`, or `swapdb`.
`swapdb` retains the old dataset until the new stream is usable and restores
both data and the cluster slot map if loading fails.

Redis 6 replication supports ACL credentials through `masteruser` and
`masterauth`, improved partial resynchronization, full-sync timeouts,
replication progress fields in `INFO`, and `FAILOVER` for coordinated
primary/replica handoff.

QFork is a Windows-specific substitute for the RDB/AOF fork paths, not a
general POSIX `fork()` implementation. Evaluate memory use and fork latency on
production-sized datasets before rollout.

## Cluster and Sentinel

Redis Cluster uses Redis 6.2 command and failover behavior over the Windows
IOCP transport, including replica migration controls, resharding, manual
takeover, diskless `swapdb` recovery, and newer `INFO` error statistics.

The package includes `redis-sentinel.exe`, implemented by the server executable
in Sentinel mode, plus `sentinel.conf`. Sentinel supports ACL-protected
monitored instances (`sentinel auth-user` and `auth-pass`), ACL protection for
Sentinel itself, separate `sentinel-user` and `sentinel-pass` credentials
between Sentinels, stable `SENTINEL MYID`, hostname resolution and announcement
options, command introspection, and configuration rewrite.

Do not enable TLS-only Cluster or Sentinel settings in the standard MinGW
package; TLS is not built into it.

## Modules and Lua debugging

The Redis 6.2 module API is available for native Windows PE modules, including
authentication, command filters, blocked clients and blocked-on-key operations,
timers, streams, keyspace and server events, INFO fields, RDB/AUX persistence,
propagation, scan, lazy-free, and datatype callbacks. Modules must be built for
Windows x64 against this tree's `src/redismodule.h`; Linux shared objects cannot
be loaded. The upstream test build uses a `.so` suffix for harness compatibility,
but the file is still a Windows DLL.

No modules are bundled in the portable ZIP. Existing Redis 5 Windows modules
should be rebuilt and retested before production use.

Callbacks executed inside a Windows QFork persistence child have a narrower
contract than ordinary live-parent module callbacks. A compatible module is a
single x64 PE DLL whose RDB, AUX, AOF-rewrite, and persistence-event callbacks
reside in that primary image. Fork-visible globals must be plain data or point
to memory obtained through the Redis module allocator so the target lives in
the QFork heap. Delay imports, CFG/XFG or other nonzero extended PE load-config
state, and module-defined static TLS are rejected. At each persistence
snapshot, the primary DLL is resolved to its final volume path and held open
without write or delete sharing until the child has terminated.

Persistence callbacks must not depend on process-private CRT/C++ state,
`malloc` pointers, `thread_local` or dynamically allocated TLS, custom
`DllMain`/`atexit` behavior, Win32 handles, sockets, mutexes, helper-DLL mutable
state, or concurrently changing module-thread state. Modules must quiesce or
synchronize their own threads around fork-visible globals. Treat pre-fork
Redis/module-allocator objects as read-only: do not insert into, remove from,
resize, or free snapshot-owned containers or objects. Redis allocation APIs
may be used for child-local scratch objects that are not linked into those
containers. Keep persistence callbacks limited to persistence IO, logging,
Redis strings/contexts, and such scratch allocation; blocking, networking,
process, and thread-safe-context work is unsupported in the QFork child.
Detectable PE or callback-image violations fail module loading or the
background persistence start cleanly.

The general `RedisModule_Fork` family is not exposed on Windows. Active
defragmentation is supported through this port's customized jemalloc allocation
hints; the Windows regression covers real movement with 4 MiB allocator pages
and a QFork save/reload cycle. Lua debugging is supported only in synchronous
mode: use `SCRIPT DEBUG sync` or `redis-cli --ldb-sync-mode`; forked `SCRIPT
DEBUG yes` sessions are unsupported.

## CLI, benchmark, service, and console

`redis-cli` adds ACL authentication (`--user`, `--pass`, `--askpass`), RESP3
(`-3`), RESP3 push display control, URI usernames, command-error exit status
(`-e`), and newer key and memory inspection behavior. `redis-benchmark` adds
ACL users, multi-thread mode, cluster mode, client tracking, error display,
precision control, and HDR Histogram latency accounting.

On Windows, `redis-benchmark` completes its initial TCP client connections
before attaching them to IOCP event loops and before benchmark timing begins.
Very large or remote initial client sets may therefore take longer to
initialize. With `-k 0`, replacement connections occur during the measured run
and can reduce total throughput or pause a worker loop, although per-request
latency timing begins after each reconnect.

Windows raw-pipe input and `redis-cli --rdb` file or standard-output paths use
binary mode, so redirected RESP streams and RDB bytes are not altered by CRT
newline translation.

The Windows service interface remains:

```text
redis-server.exe --service-install [--service-name name] <config and options>
redis-server.exe --service-start [--service-name name]
redis-server.exe --service-stop [--service-name name]
redis-server.exe --service-uninstall [--service-name name]
```

Service management requires elevation. The default installed service runs as
`NT AUTHORITY\NetworkService`. Service mode always logs to the Windows
Application Event Log; console mode does so only with `syslog-enabled yes`.
The Event Source is fixed as `redis`, while `syslog-ident` is included in the
message text. Built-in installation registers the service executable's embedded
message table; the ZIP also carries `EventLog.dll` as a standalone/legacy
message resource.

All named services share that one Event Log registration. The first built-in
install that creates it supplies `EventMessageFile`; later named installs do
not refresh the path, and built-in uninstall of any name removes the shared
registration. For multiple or side-by-side services, manage individual service
deletion with SCM tools, keep the registered executable in place, and recreate
the shared source from the intended surviving executable after removal or a
path change. Windows does not support Redis daemonization; use console mode or
the service interface.

## Portable package and runtime dependencies

The flat x64 ZIP contains `redis-server.exe`, `redis-sentinel.exe`,
`redis-cli.exe`, `redis-benchmark.exe`, `redis-check-aof.exe`,
`redis-check-rdb.exe`, `EventLog.dll`, console/service/Sentinel configuration
examples, `README.txt`, the license, release notes, and this behavior guide. A
separate `.sha256` file provides an integrity checksum for a ZIP obtained from a
trusted channel.

The MinGW support runtimes (`libgcc`, `libstdc++`, and winpthreads) are linked
without external MinGW DLL dependencies. Normal Windows system DLLs, including
MSVCRT, remain imports. The target does not need MSYS2, Tcl, or Visual Studio;
test modules and build-only validation binaries are not shipped.

## Windows-specific constraints

- x64 MinGW is the supported release build. The Visual Studio/MSI projects are
  legacy best-effort files.
- The standard package has no OpenSSL/TLS support. `tls-port`, `rediss://`, TLS
  replication, and TLS cluster links are unavailable.
- Unix domain sockets are unavailable; use TCP on loopback or another bound
  interface.
- Server multi-I/O-thread mode is unavailable and rejected at startup; keep
  `io-threads 1` and `io-threads-do-reads no`.
- General module fork APIs and asynchronous/forked Lua debugger sessions are
  unavailable.
- POSIX daemon, systemd/upstart, Linux OOM-score, CPU-affinity, process-title,
  THP, and signal behavior does not map directly to Windows; use the Windows
  service and Event Log facilities.
- QFork requires a stable virtual-address layout. The MinGW executables
  intentionally disable dynamic-base and high-entropy ASLR at link time.
- This is an unofficial port. Validate persistence, replication, modules,
  service permissions, antivirus policy, and performance under the exact
  Windows version and workload used in production.

## Upgrade checklist

1. Back up RDB, AOF, configuration, Sentinel, and ACL files.
2. Install the portable files side-by-side and retain the ZIP checksum/source
   commit. If using built-in services, account for the shared Event Log
   registration described above.
3. Review snapshot defaults, directory permissions, protected mode, bind
   addresses, ACLs, and replication credentials.
4. Keep clients on RESP2 initially; enable RESP3 deliberately.
5. Rebuild native modules, retest any defragmentation callbacks, and confirm
   they do not require module fork.
6. Avoid old Redis 5 replicas or downgrade after using Redis 6-only commands.
7. Exercise `PING`, authentication, representative commands, `BGSAVE`, AOF
   rewrite, restart/reload, replication, and any Cluster, Sentinel, or service
   paths used by the deployment.
