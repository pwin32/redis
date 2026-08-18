# Redis 7.2 behavior in the Windows port

## Scope and release identity

This is an unofficial Windows x64 integration of the Redis 7.2.14 behavior
baseline, including its upstream security fixes, onto this fork's IOCP
networking, FDAPI descriptors, QFork persistence,
Windows service/Event Log, console, and MinGW64 build layers. The source tree
reports `7.2.14`; the canonical `mingw-7.2` branch is the maintenance line.
The immutable `v7.2.14` tag identifies the original promotion archive. The
corrected Windows maintenance revision is identified as `v7.2.14-windows.1`
and packaged as `Redis-x64-7.2.14-mingw-r1.zip` after its gates pass.

This guide documents behavior that differs from the original Redis 7.2.14
Linux/POSIX distribution, plus the upgrade boundaries that matter when moving
from this fork's Redis 6.2 line. Redis command, protocol, persistence-format,
replication, Cluster, Sentinel, ACL, Lua, and Function behavior otherwise
follows upstream Redis 7.2.14. See `00-RELEASENOTES` for the complete upstream
release history and command-level compatibility notes.

The required clean build, focused and full test suites, package extraction,
long persistence soak, and exact service-restart gates are the maintenance
revision's release gates. The original `v7.2.14` archive remains immutable.

### Upstream security-fix provenance

The 7.2.14 source baseline includes the applicable upstream security fixes for
Lua parser/runtime isolation, blocked-client eviction, RESTORE/RDB validation,
and full-sync while a yielding script or Function is running. The Windows
full-sync path retains an additional IOCP disconnect cleanup guard in
`12c1730f7`; do not replace it with the unadapted POSIX patch. XACKDEL-specific
fixes are not applicable because that command is absent from this 7.2 line.
The standard package is x64 and excludes TLS. Visual Studio/MSI,
clean-machine, antivirus, and unrelated external deployment checks are not
covered by this local release matrix.

## Linux/POSIX and Windows behavior differences

| Area | Original Redis 7.2.14 on Linux/POSIX | This Windows port |
| --- | --- | --- |
| Event loop | OS-specific POSIX backend such as `epoll` or `kqueue` | IOCP with synthetic FDAPI descriptors |
| Network transports | TCP, optional Unix sockets, and optional TLS builds | TCP only in the standard package; Unix sockets are rejected and TLS is not built |
| Client I/O threads | Configurable multi-threaded reads/writes | `io-threads 1` and `io-threads-do-reads no` are required and enforced at startup |
| Background persistence | POSIX `fork()` copy-on-write child | QFork starts a fresh Windows process and maps a copy-on-write Redis heap at fixed virtual addresses |
| Native modules | Usually ELF shared objects; module fork API available | x64 PE modules; module-created fork children are unsupported and the fork API entries are unavailable in the Windows module table; persistence callbacks have a restricted QFork contract |
| Lua debugger | Synchronous or forked debugger session | `SCRIPT DEBUG sync` only; forked `SCRIPT DEBUG yes` is rejected when execution starts |
| Daemon/supervision | `daemonize`, systemd/upstart integration, Unix signals | Console process or Windows SCM service; no POSIX daemon or systemd/upstart behavior |
| System integration | syslog, Linux OOM score, THP checks, CPU affinity, process titles | Windows Application Event Log and SCM; the listed Linux controls do not have equivalent behavior in this build |
| Distribution | Source build/install conventions vary by platform | Flat x64 ZIP; no external MinGW runtime, MSYS2, Tcl, or Visual Studio installation is required at runtime |

## Networking, CLI, and benchmark behavior

- The server uses the Windows IOCP event backend and the fork's FDAPI mapping.
  TCP over IPv4 and IPv6 is supported. The standard package rejects a configured
  Unix-domain socket and rejects TLS listeners, TLS replication, and TLS Cluster
  links because OpenSSL is not included.
- Listener setup uses Winsock's exclusive-address behavior rather than relying
  on the POSIX `SO_REUSEADDR` model. A port can remain unavailable briefly after
  a process exits; service automation must wait for the exact old process and
  port to close before starting a replacement.
- `tcp-keepalive` is translated to Winsock `SIO_KEEPALIVE_VALS`: the configured
  interval controls the first probe and the retry interval is derived from it,
  while the remaining probe/failure policy is controlled by Windows. Validate
  firewalls, load balancers, and idle timeouts on the target host.
- Protected mode recognizes both `127.0.0.1` and `::1` as loopback. The shipped
  server examples bind only to `127.0.0.1`; add `::1` deliberately if IPv6
  loopback is required and validate the host's IPv6 configuration.
- Configuration `include` paths are literal on Windows. POSIX wildcard/glob
  expansion is not available, so list every included file explicitly instead of
  using a pattern such as `conf.d/*.conf`.
- Server I/O-thread mode is deliberately unavailable. Startup fails when
  `io-threads` is not 1 or `io-threads-do-reads` is `yes`; this prevents unsafe
  client ownership and QFork interactions in the IOCP path.
- Windows raw-pipe input and `redis-cli --rdb` file or standard-output paths use
  binary mode, so the CRT does not translate line endings in RESP or RDB bytes.
- `redis-benchmark` completes its initial TCP client connections before attaching
  them to IOCP event loops and before timing begins. Large or remote client sets
  may therefore take longer to initialize. With `-k 0`, replacement connections
  happen during the measured run and may reduce throughput or briefly pause a
  worker loop; per-request timing begins after the reconnect.

## Packaged configuration defaults

The packaged `redis.windows.conf` and `redis.windows-service.conf` are
Windows-oriented compatibility templates, not substitutes for reviewing every
Redis 7.2 option. Their active directives are validated by the release tests;
comments retained for less common options are reference material. Compare a
deployment configuration with the source tree's upstream-aligned `redis.conf`
and then apply these intentional package differences:

| Setting | Packaged Windows examples | Upstream 7.2.14 example/default | Notes |
| --- | --- | --- | --- |
| `bind` | `127.0.0.1` | `127.0.0.1 -::1` in `redis.conf` | Conservative IPv4-loopback package default |
| `repl-diskless-sync` | `no` | `yes` | The examples prefer disk-backed full synchronization; diskless QFork replication remains supported and tested |
| `io-threads` / reads | `1` / `no` | Same runtime defaults | Explicit in the Windows templates because other values are rejected |
| Console logging | empty `logfile`; Event Log off | empty `logfile`; syslog off | Writes to the launching console |
| Service logging | `server_log.txt`; Event Log on | Not applicable | Writes to both the file and Windows Application Event Log |
| `appendonly` | `no` | `no` | Enable only after planning the complete multi-part AOF directory |

Windows is not a POSIX daemon environment. Keep `daemonize no` and
`supervised no`; use the console or the Windows service interface. Setting
`daemonize yes` does not detach on Windows; it logs a warning and still attempts
the best-effort default POSIX pidfile path (`/var/run/redis.pid`) unless an
explicit writable pidfile is configured, so do not use it. Linux `oom-score-adj`
is unsupported and enabling it makes startup/`CONFIG SET` fail. Transparent-
huge-page checks, CPU-affinity and
process-title controls, watchdog signal behavior, and other POSIX signal
semantics do not map directly to this build. Windows crash reporting and
service stop notifications use the native portability layer instead.

On Windows, `syslog-enabled yes` selects the Application Event Log and
`syslog-ident` is included in the message text. `syslog-facility` is unsupported
and is not registered by the Windows parser; remove or comment it out because
an active `syslog-facility` directive makes startup fail.

`CONFIG REWRITE` needs write access to the configuration file's directory.
Relative paths in an SCM service process resolve from the executable directory.
At installation time, path discovery resolves relative `dir` and include paths
from the elevated install command's current directory, and the installer grants
its default `NT AUTHORITY\NetworkService` account access to the discovered
configuration/include and data directories. Use absolute paths or install from
the extracted executable directory. Grant permissions separately for ACL files,
log directories, modules, and any other external path.

The Windows-only `persistence-available no` option is intended for diagnostics
and deliberately removes commands that require QFork persistence or full-sync
state. It is not a general production tuning option.

Cluster configuration files are atomically replaced on Windows and use a
companion `<cluster-config-file>.lock` held with `LockFileEx` for the life of
the node. Give each node a distinct writable configuration path. Treat the
companion as a runtime lock artifact rather than portable dataset state: its
mere presence is normal, it need not be backed up or deleted, and only the
held OS lock—not file existence—indicates that another node owns the path.

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
points, and other reparse points in Redis-owned trees. Multi-part AOF
old-style/upgrade filename identity follows the same case-insensitive rule.
Atomic replacement is
same-volume only and uses write-through `MoveFileExW`; Windows has no direct
equivalent of POSIX directory `fsync()`. Storage caches and transient sharing
violations from antivirus, backup, or indexing software can therefore still
affect crash durability or cause an operation to fail after its bounded retry.

## Persistence, QFork, and replication

Windows background RDB saves, AOF rewrites, and diskless full synchronization
use QFork instead of POSIX `fork()`. QFork launches a fresh copy of the server,
maps the Redis heap at the same virtual addresses with copy-on-write protection,
reconnects selected Redis roots, performs the requested child operation, and
then discards the child's private writes. It is a persistence mechanism, not a
general-purpose fork implementation.

The MinGW executables intentionally disable dynamic-base and high-entropy ASLR
so the mapped heap can occupy the same addresses in parent and child. The port
uses the bundled jemalloc 5.3.0 tree and its Windows heap/page-tracking hooks;
do not substitute the completed 6.2 line's jemalloc 5.2.1 tree. QFork also
depends on sufficient Windows commit/pagefile capacity for the mapped heap and
copy-on-write pages. Size and monitor system commit for the real dataset,
fragmentation, modules, and concurrent background work rather than relying on
old fixed `--maxheap` or pagefile formulas.

Disabling image-base randomization is a deliberate QFork compatibility and
security tradeoff. The executables retain NX compatibility, but deployments
must account for the reduced ASLR protection when evaluating host isolation,
network exposure, patching, and access controls.

Redis 7.2 multi-part AOF uses a manifest under the configured `appenddirname`
directory (default `appendonlydir`), with base and
incremental files, rotations, history cleanup, and atomic replacement. Windows
close/share/rename rules are handled by this port, but backup and restore must
always treat the entire AOF directory as one unit. Keep the manifest, every
referenced AOF part, RDB files, configuration, ACL data, Sentinel state, and
module binaries together.

Disk-backed and diskless full synchronization are both supported. The package
examples intentionally set `repl-diskless-sync no`, unlike upstream's default
of `yes`; enable it only after validating network, memory, credentials, module
loading, and QFork child cleanup under the intended workload. Replica diskless
load modes (`disabled`, `on-empty-db`, and `swapdb`) retain upstream semantics.
Failed child creation, socket handoff, or load must not leave a stale QFork
process or a partial persistence file.

For an RDB-only full sync, the Windows sender half-closes its socket and drains
the peer before final close (or until `repl-timeout`) so pending overlapped bytes
are not discarded. File cleanup also obeys Windows sharing rules: a file cannot
be unlinked while a conflicting handle remains open, so some replication cleanup
is synchronous and can block or fail until the handle is released.

Active defragmentation is supported through the customized jemalloc allocation
hints and is disabled by default, as upstream. The Windows regression suite
covers real movement together with a QFork save/reload cycle; production users
should still measure latency and memory behavior on their own dataset.

## Redis 6.2 to 7.2 upgrade boundaries

- Redis 7.2 writes RDB format version 11. Older Redis versions that support only
  an earlier RDB version cannot load a newly written 7.2 RDB. Do not plan an
  in-place binary downgrade after Redis 7.2 has persisted the dataset.
- Redis 7 uses the multi-part AOF directory described above rather than the
  single-file layout used by older releases. Back up and move the complete
  directory, not only `appendonly.aof`.
- Older ziplist-based RDB encodings remain loadable but Redis 7 uses listpack
  encodings for current data structures. Confirm memory and module assumptions
  after conversion and rewrite.
- Redis 7.x adds Functions (`FUNCTION`, `FCALL`, `FCALL_RO`), sharded Pub/Sub,
  list and sorted-set pop families such as `LMPOP`/`ZMPOP`, `WAITAOF`, newer
  `CLIENT` metadata/touch controls, and Cluster shard identity. Replication and
  AOF streams containing new commands are not suitable for an older server.
- Rebuild and retest every native module against this tree's `redismodule.h` and
  the QFork restrictions below. Never copy Linux module binaries into this
  package.

## Functions, Lua, and native modules

Redis Functions add persisted engine and library state beyond ordinary
`redisServer` roots. Function load, RDB/AOF serialization, replication load,
and QFork child reconstruction are integrated, but the fresh child does not
inherit a Linux forked Lua runtime. Exercise Function execution in the live
parent and persistence/reload as separate gates.

Lua debugging on Windows is synchronous only. Use `SCRIPT DEBUG sync` or
`redis-cli --ldb-sync-mode`. A non-sync debugger request may be accepted by the
configuration command, but execution is rejected rather than creating a forked
debugger child.

Native modules must be x64 Windows PE images built against this tree's
`src/redismodule.h`; Linux ELF `.so` files are not loadable. The test harness
uses a `.so` filename for compatibility, but those files are Windows DLLs. No
modules are bundled in the portable ZIP.

The general `RedisModule_Fork`, child heartbeat/exit, and fork-child kill API
pointers are not populated in the Windows module API table. Module-created fork
children are unsupported; modules must feature-detect these pointers and avoid
them rather than dereferencing a missing API. RDB, AUX, AOF-rewrite, and
persistence-event callbacks that run in a QFork child have a narrower contract:

- The callback and its writable globals must live in the primary module image.
  Do not depend on mutable state in helper DLLs.
- Windows-only `RedisModule_Win32Pipe`, `RedisModule_Win32PipeRead`,
  `RedisModule_Win32PipeWrite`, and `RedisModule_Win32PipeClose` are the
  supported pipe primitives for module event-loop integration. A private CRT
  pipe does not participate in the process-wide synthetic FD map; handle
  nonblocking partial reads and writes.
- Fork-visible data must be plain data or live in Redis/module-allocator memory.
  Treat inherited Redis and module containers as immutable; child-local scratch
  allocation is allowed only when it is not linked back into inherited state.
- Do not depend on process-private CRT/C++ state, `malloc` allocations,
  `thread_local` or dynamic TLS, `DllMain`/`atexit` side effects, Win32 handles,
  sockets, mutexes, or concurrently changing module-thread state.
- Delay imports, CFG/XFG, non-boilerplate static TLS, unsafe writable PE
  sections, and extended load-config state are rejected by the module-image
  validator. The resolved primary DLL is held open without write/delete sharing
  until the QFork child exits.
- APIs requiring the parent IOCP event loop, client networking, blocking work,
  timers, new threads, thread-safe contexts, propagation, or mutation of the
  inherited snapshot are rejected in the persistence child. Parent execution of
  the normal module APIs remains available.

## Service, Event Log, and console behavior

Run service-management commands from an elevated command prompt. The service
action must be the first argument, and `--service-name` must precede the Redis
configuration and overrides:

```bat
redis-server.exe --service-install --service-name Redis7214 redis.windows-service.conf
redis-server.exe --service-start --service-name Redis7214
redis-server.exe --service-stop --service-name Redis7214
redis-server.exe --service-uninstall --service-name Redis7214
```

Installation creates an automatic-start service running as
`NT AUTHORITY\NetworkService`, but does not start it. Uninstallation requests
SCM deletion but does not stop a running service; a running service remains
marked for deletion until it stops and open SCM handles are closed. Retain the
install/start/stop/uninstall sequence. Use a unique service name, absolute
executable/configuration paths when outside the extracted directory, and exact
PIDs when verifying or cleaning up a test. Never stop or remove an unrelated
installed `Redis` service.

SCM can report the service as `Running` before the Redis worker has finished
initializing its listener. After every start or restart, wait for a successful
authenticated `PING` and any deployment-specific readiness checks before
accepting traffic.

SCM `STOP` and `PRESHUTDOWN` notifications are converted into Redis' graceful
shutdown path on the main thread. The service reports stopped only after the
actual worker has exited; callers should still wait for the SCM state, process
exit, and port closure before restart or replacement.

In console mode, Redis writes to stdout when `logfile` is empty or the literal
`stdout`, and to the named file otherwise. `syslog-enabled yes` additionally
writes to the Windows Application Event Log. SCM service mode enables
Application Event Log output automatically and retains the configured logfile
sink, so the packaged service example writes both `server_log.txt` and the
Application Event Log.

All named Redis services share the Event Log source named `redis`. The first
installation supplies the registered message-resource path; installing another
name does not refresh it. Uninstalling any one named Redis service removes the
shared registration and may break message rendering for surviving services
until a Redis service is installed again. Coordinate side-by-side service
removal and keep the registered executable path available.

Sentinel's external notification and reconfiguration helpers use Windows
process creation and wait/terminate APIs. A timed-out helper exits with a
failure status and is retried; do not assume POSIX signal-kill or shell quoting
semantics when deploying `notification-script`, `client-reconfig-script`, or
`deny-scripts-reconfig` hooks. Batch or command scripts need an explicit
interpreter such as `cmd.exe /c`; they are not direct POSIX `execve` targets.

## Package contents and deployment checklist

The flat x64 ZIP contains `redis-server.exe`, the `redis-sentinel.exe` alias,
`redis-cli.exe`, `redis-benchmark.exe`, RDB/AOF checkers, `EventLog.dll`, the
console/service/Sentinel examples, `README.txt`, release notes, license, and
this guide. The built-in service registers the message resource embedded in
`redis-server.exe`; `EventLog.dll` is also shipped as a standalone legacy
message-resource artifact. The executables statically link the MinGW runtime
components used by the build; normal Windows system DLLs remain expected
imports.

The historical Visual Studio, NuGet, Chocolatey, and MSI files under `msvs/`
consume an older signed-binary layout and are not the Redis 7.2 package path or
a release gate. Use `package-mingw.sh`, retain its adjacent SHA-256 file, and
record the exact source commit that produced the archive.

Before deployment:

1. Back up the RDB, complete multi-part AOF directory, configuration and ACL
   files, Sentinel state, and module binaries.
2. Rebuild native modules for this exact ABI and review every QFork persistence
   callback against the restricted contract.
3. Review bind/protected-mode, ACLs, authentication, absolute paths, directory
   permissions, service identity, Windows commit/pagefile capacity, and
   antivirus policy.
4. Run representative `PING`, authentication, command, `BGSAVE`, AOF rewrite,
   restart/reload, replication, Cluster, Sentinel, module, and exact service
   restart checks with the production configuration and workload.
5. Retain the source commit, package checksum, and test evidence. Do not
   downgrade after writing Redis 7.2-specific commands or persistence formats.
