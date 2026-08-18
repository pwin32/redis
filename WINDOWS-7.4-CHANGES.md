# Redis 7.4.10 behavior and upgrade guide for Windows

## Scope and release identity

This is the authoritative behavior and upgrade guide for the unofficial Redis
Community Edition 7.4.10 Windows x64 MinGW port. The source baseline is the
exact upstream Redis `7.4.10` tag, commit
`f103d127b9747965e28f20615ef790332661fc68`, with the applicable upstream
security fix, integrated with this fork's IOCP networking, FDAPI descriptors,
QFork persistence, Windows service, Event Log, console, and MinGW64 layers.
The source tree and produced binaries report `7.4.10`.

The Windows release identity is:

- canonical maintenance branch: `mingw-7.4`;
- annotated Windows release tag: `v7.4.10-windows.1`;
- portable archive: `Redis-x64-7.4.10-mingw-r1.zip`; and
- adjacent checksum: `Redis-x64-7.4.10-mingw-r1.zip.sha256`.

Those names identify the first Windows packaging revision of the 7.4.10
baseline. They do not authorize an artifact by themselves. The tag must be
created only after all release gates in this guide pass against a clean source
commit and a never-before-used package extraction. The annotated tag, archive,
checksum, package contents, and tested source commit must agree exactly. Do not
move a published tag or silently replace an archive; a changed Windows package
requires a new Windows revision.

Except for the differences documented here, command, RESP, ACL, Lua, Function,
replication, Cluster, Sentinel, persistence, and Module API behavior follows
upstream Redis 7.4.10. `00-RELEASENOTES` is the source for upstream release and
command-level changes. This guide supersedes `WINDOWS-7.4-PORTING.md` for an
approved release; the porting ledger remains useful for implementation history.

### Redis 7.4.10 security-fix provenance

Redis 7.4.10 is an upstream `SECURITY` release. It rejects a crafted stream RDB
or `RESTORE` payload in which two consumers claim the same pending-entry NACK.
Accepting that invalid structure could leave one consumer with a dangling
pointer after the shared object was freed, creating a use-after-free that may
lead to remote code execution. The 7.4.10 loader now treats a second consumer
assignment as corrupt data, aborts the load with a bad-data error, and keeps the
server usable.

This fix is part of the exact 7.4.10 source baseline and is applicable to the
Windows port without changing its Redis-level semantics. Release qualification
must nevertheless run the crafted stream `RESTORE` regression with the MinGW
server and confirm both rejection and a successful subsequent `PING`. Redis
7.4.10 also contains the security fixes from earlier 7.4 maintenance releases;
do not substitute a 7.4.9 or earlier binary while retaining 7.4.10 package
metadata.

## Licensing and notices

Redis 7.4 changed the upstream license model. Upstream Redis 7.4.10 is offered
under the user's choice of the Redis Source Available License 2.0 (`RSALv2`) or
the Server Side Public License v1 (`SSPLv1`) as set out in `LICENSE.txt`.
`REDISCONTRIBUTIONS.txt` records the continued BSD-3-Clause treatment of the
applicable portions described there. The fork-specific Windows portability
code and modifications retain their BSD-3-Clause notices in
`WINDOWS-NOTICES.txt`.

The 7.4 Windows source and binary distributions must preserve, without
renaming or omission:

- `LICENSE.txt`;
- `REDISCONTRIBUTIONS.txt`;
- `WINDOWS-NOTICES.txt`; and
- `THIRD-PARTY-NOTICES.txt` plus the packaged CC0, GCC Runtime Library
  Exception, GPLv3, and MinGW-w64 runtime license texts.

The old 7.2 `COPYING`/lowercase `license.txt` package convention is not the
7.4 license payload. In particular, a Windows case-insensitive filesystem must
not be used to collapse differently named historical license files into an
ambiguous result. Publication requires explicit review of the selected license,
source/object distribution obligations, notices, package metadata, and any
downstream MSI, NuGet, or Chocolatey metadata. This document is an engineering
guide, not legal advice.

## Linux/POSIX and Windows behavior differences

| Area | Original Redis 7.4.10 on Linux/POSIX | This Windows port |
| --- | --- | --- |
| Event loop | A POSIX backend such as `epoll` or `kqueue` | IOCP with synthetic FDAPI descriptors and one-shot readiness rearming |
| Network transports | TCP, optional Unix sockets, and optional TLS builds | TCP over IPv4/IPv6 only in the standard package; Unix sockets and TLS modes are rejected |
| Client I/O threads | Multiple client read/write I/O threads may be configured | `io-threads 1` and `io-threads-do-reads no` are required and enforced at startup |
| Background persistence | POSIX `fork()` copy-on-write child | QFork launches a fresh Windows process and maps the Redis heap at matching virtual addresses |
| Address randomization | Normal PIE/ASLR conventions | MinGW executables disable dynamic-base and high-entropy ASLR so QFork can preserve addresses |
| Lua allocator | Dedicated jemalloc arena plus a private explicit tcache | Dedicated arena retained; QFork builds use `MALLOCX_TCACHE_NONE` and do not create/destroy the private Lua tcache |
| BIO completion notification | Pipe readiness follows the selected POSIX event backend | IOCP handler drains and explicitly rearms the one-shot completion pipe |
| Crash/watchdog path | POSIX signals, `sigaction`, and SIGALRM software watchdog | Native SEH and abort reporting; no POSIX signal recursion contract and no functional SIGALRM watchdog |
| Interactive CLI | POSIX terminal/TTY handling | Native `ReadConsoleInput` and ANSI console translation for a real console; separate binary fake-TTY path for tests |
| ACL connection close | Descriptor teardown is normally observed immediately by tests | Revocation is immediate, but Winsock close-after-reply draining can briefly retain the client in `CLIENT LIST` |
| Native modules | Usually ELF shared objects; module fork API available | x64 PE DLLs; module-created fork children are unsupported and QFork persistence callbacks have a restricted contract |
| Service integration | Daemon/systemd/upstart/syslog conventions | Console or Windows SCM service with Windows Application Event Log support |
| Benchmark startup | Initial connection setup depends on the POSIX client path | Initial TCP connections complete before IOCP attachment and before measured time starts |
| Distribution | Source/install conventions vary | Flat Windows x64 ZIP after release gates; no MSYS2 or MinGW installation is required at runtime |

The single client-I/O-thread restriction does not mean the whole process has
only one thread. Redis still uses background I/O and other internal workers
where supported. It means client read/write ownership remains on the main
thread because the Windows IOCP, FDAPI, and QFork combination has not been
qualified for upstream's multi-threaded client-I/O mode.

## Networking, FDAPI, CLI, and benchmark behavior

### IOCP and transports

The server uses Windows IO completion ports rather than a POSIX readiness
backend. The FDAPI layer presents Redis with small synthetic file descriptors
that map to Winsock sockets, files, pipes, or Windows handles. Code and modules
must not assume that one of these values is a raw `SOCKET`, CRT descriptor, or
POSIX file descriptor, nor bypass the owning API to close or duplicate it.
IOCP readiness is often one-shot, so a handler that expects another event must
queue the next operation explicitly.

TCP over IPv4 and IPv6 is supported. The standard MinGW package does not build
OpenSSL and rejects TLS listeners, TLS replication, and TLS Cluster links rather
than accepting settings it cannot honor. Unix-domain sockets are unsupported
and an active `unixsocket` configuration is rejected. Use a separate trusted
TLS proxy or VPN only after evaluating its authentication, failure, timeout,
and operational model; it is not equivalent to a Redis TLS build.

Listener setup uses Winsock exclusive-address behavior rather than depending
on the POSIX `SO_REUSEADDR` model. After stop or crash, deployment automation
must wait for the exact old process to exit and for its port to close before
starting a replacement. `tcp-keepalive` is translated to Winsock
`SIO_KEEPALIVE_VALS`: Redis supplies the initial and retry timing while Windows
retains control of the remaining probe/failure policy. Validate host, firewall,
proxy, and load-balancer idle timeouts together.

Protected mode recognizes `127.0.0.1` and `::1` as loopback. The packaged
examples use the conservative `127.0.0.1` bind; add IPv6 or non-loopback
addresses deliberately and secure them with ACLs, authentication, firewall
rules, and transport protection. Configuration `include` paths are literal on
Windows: POSIX wildcard expansion such as `conf.d/*.conf` is unavailable, so
list every file explicitly.

Startup fails if `io-threads` is not `1` or if `io-threads-do-reads` is `yes`.
Treat that failure as an unsupported configuration, not a tunable warning.

### BIO completion pipe

Redis 7.4 uses a pipe to notify the event loop about completed background I/O
work, including lazy-free work used by synchronous database flushes. POSIX
readiness backends naturally report later bytes. On this IOCP port, read
readiness is one-shot: the Windows handler drains the notification pipe and
then calls the IOCP rearm path before dispatching completions. Without the
rearm, one `FLUSHDB` can finish while the next client waits indefinitely.

Release tests must exercise at least two successive synchronous `FLUSHDB`
operations, verify the resulting `DBSIZE`, and confirm that the server remains
responsive. Any future completion-pipe consumer must preserve the same
drain-then-rearm ownership rule.

### Real console, redirected I/O, and fake TTYs

In an actual Windows console, `redis-cli` uses `ReadConsoleInput` for keyboard
events and the native ANSI console translator for interactive output. This is
the supported human-interactive path and must be tested in a real console, not
inferred from redirected test results.

The test-only `FAKETTY_WITH_PROMPT` mode is deliberately different. It treats
stdin as a binary pipe, reads through ordinary descriptors, uses a fixed
80-column width, and relies on blocking, flushed Tcl writes so prompt-driven
tests cannot deadlock. `FAKETTY` can make output take TTY-oriented formatting,
but it does not turn redirected stdin into a Win32 console. Redirected
stdout/stderr bypass the console ANSI parser and use CRT byte writes. Raw RESP,
RDB, and pipe output uses binary mode to prevent CR/LF translation.

Neither fake-TTY environment variable is a deployment interface. Scripts
should use non-interactive CLI modes and must not depend on the prompt parser,
console escape translation, or console window width.

### ACL close timing

ACL deletion or revocation follows upstream semantics and immediately removes
the client's authorization. The Windows close-after-reply path may retain the
socket briefly so Winsock can drain the final response. During that interval,
`CLIENT LIST` can still show the revoked connection even though it cannot
continue authorized work. Tests and orchestration must wait with a bounded
poll for disappearance instead of assuming POSIX-immediate descriptor teardown.

### `redis-benchmark` timing

On Windows, `redis-benchmark` completes its initial TCP connections before it
attaches those clients to the IOCP event loops and before the measured interval
begins. A high client count or remote target can therefore make startup visibly
longer without that time appearing in reported request latency or throughput.
Connection-establishment performance must be measured separately when it
matters.

With `-k 0`, replacement connections occur during the measured run; their cost
can reduce throughput or pause a worker loop, and per-request timing resumes
after reconnect. Release integration coverage proves functional benchmark
operation only. A release performance claim requires an explicit, repeatable
Windows benchmark protocol, recorded hardware/configuration, warmup, multiple
runs, and an approved regression threshold.

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

## Configuration contract

The packaged `redis.windows.conf` and `redis.windows-service.conf` are Windows
compatibility templates, not complete replacements for reviewing upstream
`redis.conf`. Before release, every active template directive must be checked
against Redis 7.4.10. A deployment should compare its configuration with the
upstream-aligned file and then apply these intentional Windows choices:

| Setting | Windows package contract | Notes |
| --- | --- | --- |
| `bind` | `127.0.0.1` in the examples | Add `::1` or non-loopback addresses only after a security review |
| `port` | Plain TCP listener | Standard package has no TLS listener |
| `unixsocket` | Disabled and unsupported | Remove active Unix-socket directives |
| `tls-port`, `tls-replication`, `tls-cluster` | Disabled and rejected | OpenSSL is not included |
| `io-threads` / reads | `1` / `no` | Other values fail startup |
| `repl-diskless-sync` | `no` in Windows examples | Diskless QFork sync remains a workload-specific option requiring validation |
| Console logging | Empty `logfile`; Event Log off | Writes to the launching console |
| Service logging | File log plus Event Log | Paths resolve under the service process rules below |
| `appendonly` | `no` in examples | Enabling it requires managing the complete multi-part AOF directory |
| `persistence-available` | `yes` for normal use | Windows-only `no` is a diagnostic mode, not production tuning |

Keep `daemonize no` and `supervised no`; Windows uses a console process or the
SCM service interface. `daemonize yes` does not create a POSIX daemon and can
attempt an unsuitable default pidfile path. Linux OOM-score adjustment, THP
checks, CPU-affinity behavior, process-title controls, systemd/upstart, and
POSIX signal behavior do not map directly to this build. Enabling unsupported
Linux-only controls can fail startup or `CONFIG SET`.

On Windows, `syslog-enabled yes` selects the Application Event Log and
`syslog-ident` is included in message text. `syslog-facility` is not registered
by the Windows parser; remove or comment it out because an active directive
makes startup fail. Review Redis 7.4's `hide-user-data-from-log` setting for the
deployment's diagnostics/privacy balance. Also review the 7.4 connection-rate
settings rather than copying an older configuration unchanged.

`CONFIG REWRITE` requires write permission on the configuration directory.
Relative paths in an SCM service process resolve from the executable directory;
paths discovered during installation can also depend on the elevated install
command's current directory. Prefer absolute paths for configuration, includes,
data, AOF, ACL, log, module, and Sentinel state files, and grant only the service
identity the required access.

The Windows-only `persistence-available no` option deliberately removes
commands that need QFork persistence or full-sync state. It exists for isolated
diagnostics and service tests, not as a general way to run production Redis.

Cluster configuration is atomically replaced and uses a companion
`<cluster-config-file>.lock` held with `LockFileEx` for the life of the node.
Each node needs a distinct writable path. The lock file's presence is normal;
only the held OS lock indicates ownership, so do not delete it merely because
it exists or treat it as portable dataset state.

## Persistence, QFork, Lua, and replication

### QFork and fixed virtual addresses

Windows background RDB saves, AOF rewrites, and diskless full synchronization
use QFork instead of POSIX `fork()`. QFork starts a fresh copy of the server,
maps the Redis heap at the same virtual addresses with copy-on-write protection,
reconnects selected Redis roots, performs the child operation, and then
discards the child's private writes. It is a Redis persistence mechanism, not a
general-purpose process fork API.

The MinGW link uses `--disable-dynamicbase` and
`--disable-high-entropy-va` so the QFork child can reproduce address-sensitive
heap and image mappings. NX compatibility remains, but reduced ASLR is an
intentional security tradeoff. Account for it in host isolation, network
exposure, patching, ACLs, endpoint protection, and risk review. The final PE
audit must confirm the flags expected by the QFork design rather than applying
a generic hardening tool that silently re-enables incompatible relocation.

This line uses the customized `deps/jemalloc` 5.3.0-redis tree and its Windows
heap/page tracking. Do not replace it with the older 6.2 tree or an unmodified
upstream allocator. QFork needs enough Windows commit/pagefile capacity for
the mapped heap and copy-on-write pages. Capacity planning must include the
real dataset, fragmentation, modules, active defragmentation, and concurrent
background activity.

### Lua arena and tcache difference

Redis 7.4 allocates Lua VM memory from a dedicated jemalloc arena so Lua code
memory is counted and does not obstruct defragmentation. Upstream also creates
a private explicit Lua tcache for each VM. That tcache is incompatible with
the Windows QFork external heap's 4 MiB page/fill metadata.

On `_WIN32` QFork builds, Lua keeps the dedicated arena but allocates with
`MALLOCX_TCACHE_NONE`; it does not create or destroy the explicit private
tcache. Linux and Windows `NO_QFORKIMPL` utility builds retain upstream's
normal tcache path. This is an allocator implementation difference, not a
change to Lua command or Function semantics. Release validation must include
Lua `EVAL`, Functions, memory accounting, persistence/reload, and QFork child
activity in the same build.

Lua debugging remains synchronous on Windows. Use `SCRIPT DEBUG sync` or
`redis-cli --ldb-sync-mode`. A forked debugger session is unsupported because
QFork is not a general interactive fork implementation.

### RDB, AOF, and replication

Redis 7.4 writes RDB format version 12. Redis 7.2 RDB version 11 files remain
loadable by 7.4, but Redis 7.2 cannot load a newly written RDB12 file. AOF
continues to use the Redis 7 multi-part manifest layout under
`appenddirname`; back up, restore, and move the entire directory, including the
manifest and every referenced base/incremental file.

Windows sharing, close, and rename rules differ from POSIX. The port adapts
temporary persistence files and atomic replacement, but a file cannot be
removed while an incompatible handle remains open. Backups must also include
RDB files, configuration, ACL data, Sentinel state, and native module binaries.

Disk-backed and diskless full synchronization are supported through QFork.
The Windows examples prefer `repl-diskless-sync no`; enabling diskless sync
requires workload-specific validation of network failure, authentication,
memory pressure, child cleanup, and replica load modes. For an RDB-only full
sync, the Windows sender half-closes and drains the peer before final close, or
until `repl-timeout`, so overlapped bytes are not discarded. Failed QFork
creation, socket handoff, load, or cleanup must not leave a child process or a
partial persistence file.

## Redis 7.2 to 7.4 upgrade boundaries

Redis 7.4.10 can load a 7.2.14 RDB11 dataset, so the supported direction is to
back up the complete 7.2 deployment and start 7.4 against a copy. The reverse
direction is not symmetric:

- Redis 7.4 writes RDB12 even if the application has not yet used a new 7.4
  command. Once 7.4 has persisted the dataset, do not plan an in-place binary
  downgrade to 7.2.
- Redis 7.4 adds expiration of individual hash fields through `HEXPIRE`,
  `HPEXPIRE`, `HEXPIREAT`, `HPEXPIREAT`, `HPERSIST`, `HEXPIRETIME`,
  `HPEXPIRETIME`, `HTTL`, and `HPTTL`. Their state is represented by the new
  hash-field expiration structures and RDB12 encoding. AOF or replication
  streams containing these commands are unsuitable for 7.2.
- The new hash-field expiration engine adds active expiration behavior,
  `hexpired` keyspace events, `subexpiry` keyspace information, and related
  memory/expiry accounting. Validate clients that parse `INFO`, keyspace
  notifications, TTL results, or memory metrics.
- Redis 7.4 bounds scripts created through `EVAL` with LRU eviction and exposes
  the `evicted_scripts` metric. Applications that treated every evaluated
  script as permanently cached must use `SCRIPT LOAD`/`EVALSHA` deliberately
  and handle `NOSCRIPT`.
- `ACL LOAD` no longer disconnects all clients. Audit automation that depended
  on the older disconnect side effect, and separately test explicit user
  revocation with the Windows close timing described above.
- Redis 7.4 adds or changes client-visible behavior including `XREAD` with the
  `+` ID, `HSCAN NOVALUES`, same-slot `SORT`/`SORT_RO BY` and `GET` in Cluster,
  `CLIENT KILL MAXAGE`, Lua `os.clock()`, and additional INFO/MEMORY/CLIENT
  metrics. Review command allowlists, ACL categories, proxies, client parsers,
  and monitoring.
- Keep the complete multi-part AOF directory together. New commands in AOF or
  replication streams cannot be replayed by an older server even though the
  directory layout already existed in 7.2.
- Rebuild and retest every native module against the 7.4.10
  `src/redismodule.h`. Never carry a Linux `.so` or assume that a 7.2 Windows
  DLL is ABI- and behavior-safe merely because it loads.
- Review every active configuration directive against the 7.4.10 examples.
  Do not copy TLS, Unix-socket, client-I/O-thread, Linux supervision, or POSIX
  path assumptions into the Windows deployment.

A conservative standalone upgrade stops writes, takes an offline-consistent
backup, verifies the backup with the 7.2 checkers, starts 7.4.10 on a copy,
tests the application and persistence cycle, and retains the untouched 7.2
backup for rollback to a separate environment. Replication, Sentinel, and
Cluster upgrades require the normal upstream version-ordering and failover
planning plus Windows-specific QFork, port-close, and service checks. Do not
attempt rollback by pointing 7.2 at files already rewritten by 7.4.

## Native modules

Native modules must be x64 Windows PE DLLs built against this tree's
`src/redismodule.h`. Test fixtures may retain a `.so` filename for upstream
harness compatibility, but their contents are Windows DLL images. The portable
package does not bundle application modules.

The MinGW server uses the fork's small native `LoadLibraryExW`/
`GetProcAddress` adapter for module loading. The former LGPL dlfcn-win32 shim
is not part of this release, and CLI/benchmark binaries do not link a dynamic
loader adapter. Module load/unload and QFork image restoration must still be
validated with real PE test modules before tagging.

The general `RedisModule_Fork`, child heartbeat/exit, and fork-child kill API
pointers are not populated in the Windows module API table. A module must
feature-detect those APIs and avoid them. RDB, AUX, AOF-rewrite, and
persistence-event callbacks that execute in a QFork child have this narrower
contract:

- callback code and writable globals must live in the primary module image;
- fork-visible data must be plain data or Redis/module-allocator memory, and
  inherited Redis/module containers must be treated as immutable;
- child-local scratch allocation must not be linked back into inherited state;
- code must not depend on process-private CRT/C++ state, libc `malloc` state,
  `thread_local`/dynamic TLS, `DllMain` or `atexit` effects, mutable helper-DLL
  globals, Win32 handles, sockets, mutexes, or concurrently changing module
  threads;
- delay imports, CFG/XFG, unsupported static TLS, unsafe writable PE sections,
  and extended load-config state can be rejected by the QFork module-image
  validator; and
- APIs that require the parent IOCP event loop, clients, blocking operations,
  timers, new threads, propagation, or mutation of the inherited snapshot are
  unavailable in the persistence child.

Windows-only `RedisModule_Win32Pipe`, `RedisModule_Win32PipeRead`,
`RedisModule_Win32PipeWrite`, and `RedisModule_Win32PipeClose` are the supported
pipe primitives for module event-loop integration. A private CRT pipe does not
participate in the process-wide FDAPI mapping. Modules must handle nonblocking
partial reads/writes and follow descriptor ownership rules.

Parent-process Module API behavior otherwise follows upstream 7.4.10,
including the 7.4 additions. Release qualification requires the full Module API
suite, real Windows PE fixtures, load/unload and persistence callbacks, module
defragmentation, RDB/AOF reload, crash isolation, and a QFork cycle.

## Crash reporting and watchdog behavior

Windows crash diagnostics are owned by the native vectored/SEH and abort
handlers in `Win32_StackTrace`. They should produce the Windows exception and
stack information available from the running executable and its symbols.
Redis' POSIX `sigaction` crash handlers, recursive signal-log assertions, and
SIGALRM/`setitimer` software watchdog do not exist in this portability layer.
`watchdog-period` therefore does not provide the upstream POSIX watchdog
guarantee.

Changing `crash-log-enabled` does not install or remove the native handler in
the same manner as a POSIX build. Skipping upstream signal-recursion tests on
Windows is not evidence that native crash reporting works. Release gates must
exercise a controlled native exception and abort in isolated processes, retain
the resulting log, and verify that no test service, listener, or QFork child is
left behind.

## Windows service, Event Log, and Sentinel helpers

Run service management from an elevated Windows command prompt. The service
action is the first argument and `--service-name` must precede the Redis
configuration and overrides:

```bat
redis-server.exe --service-install --service-name Redis7410 redis.windows-service.conf
redis-server.exe --service-start --service-name Redis7410
redis-server.exe --service-stop --service-name Redis7410
redis-server.exe --service-uninstall --service-name Redis7410
```

Installation creates an automatic-start service using
`NT AUTHORITY\NetworkService` by default but does not start it. Uninstallation
requests deletion but does not stop a running service; retain the exact
install/start/stop/uninstall sequence. Use a unique service name and exact PID
when testing. Never stop or remove an unrelated installed Redis service.

SCM can report `Running` before Redis has completed listener initialization.
After every start or restart, wait for an authenticated `PING` and the
deployment's readiness checks. `STOP` and `PRESHUTDOWN` notifications enter the
graceful shutdown path on the main thread. Before replacing or restarting a
binary, wait for SCM state, exact process exit, and port closure.

Console mode writes to stdout when `logfile` is empty or `stdout`. In service
mode, the configured file sink remains active and Application Event Log output
is enabled. All named Redis services share the Event Log source `redis`. The
first service installation registers the message-resource path; installing
another service name does not refresh it. Uninstalling any named Redis service
can remove the shared source registration and impair message rendering for
surviving services until a Redis service is installed again. Coordinate
side-by-side service lifecycle and keep the registered executable available.

Sentinel notification and reconfiguration helpers use Windows process creation,
wait, and termination semantics. Batch scripts require an explicit interpreter
such as `cmd.exe /c`; they are not POSIX `execve` targets. Validate quoting,
timeouts, retry status, working directory, credentials, and file permissions
with the exact production helper.

## Package contents and deployment checklist

The release-gated flat x64 ZIP must contain at least the server and Sentinel
alias, CLI, benchmark, RDB/AOF checkers, Event Log resource, Windows console,
service and Sentinel examples, release notes, this guide, and the complete
license/notice payload listed above. Runtime executables must not require an
external MSYS2 or MinGW installation. The legacy Visual Studio/MSI projects are
best-effort and are not substitutes for validating the MinGW package.

Before creating `v7.4.10-windows.1`, all of the following are required against
the exact candidate commit:

1. Perform a clean `./build-mingw.sh -j2` build and verify server, CLI,
   benchmark, checker, Sentinel alias, and Event Log resources identify the
   intended 7.4.10 source.
2. Run the full root, dedicated Cluster, dedicated Sentinel, and full Module
   API suites with the repository-owned MinGW wrappers.
3. Run focused hash-field-expiration/RDB12, crafted corrupt-stream `RESTORE`,
   ACL close, CLI real-console and fake-TTY, two-`FLUSHDB` BIO, AOF, AOF-race,
   replication, active-defrag, QFork Lua/Function, and module persistence tests.
4. Run long RDB save/reload, AOF rewrite/restart, disk-backed and diskless full
   synchronization, QFork allocation, failpoint, and repeated restart soaks
   under representative memory and Windows commit pressure.
5. Build `Redis-x64-7.4.10-mingw-r1.zip`, verify its SHA-256 file and manifest,
   and audit PE architecture, imports, fixed-address/ASLR flags, resources,
   license files, absence of build/debug artifacts, and absence of external
   MinGW runtime dependencies.
6. Extract the archive into a never-before-used directory and run only the
   extracted binaries: version checks, authenticated command smoke, RDB/AOF
   checker use, persistence/restart, Sentinel/Cluster where applicable, and
   the benchmark functional and approved performance protocol.
7. From an elevated token, install a uniquely named isolated service, verify
   the exact executable and two distinct start/restart PIDs, authenticated
   reads/writes, graceful stop, file logging, Event Log message rendering,
   uninstall, process exit, port closure, service deletion, and test-directory
   cleanup.
8. Review deployment configuration, license selection and notices, source
   publication obligations, release notes, behavior documentation, archive
   checksum, and the exact commit/tag relationship. Create and push the
   annotated tag only after every required gate is accepted.

Before deploying the accepted archive:

1. Back up the RDB, complete multi-part AOF directory, configuration and ACL
   files, Sentinel/Cluster state, module DLLs, and service command line. Keep an
   untouched 7.2-compatible copy for rollback to a separate environment.
2. Rebuild native modules for this exact source and audit every QFork child
   callback against the restricted Windows contract.
3. Review bind/protected-mode, ACL/authentication, firewall and transport
   security, absolute paths, directory ACLs, service identity, Event Log
   registration, Windows commit/pagefile capacity, antivirus/EDR exclusions,
   backup software, and port-close timing.
4. Compare every active configuration directive with Redis 7.4.10; remove TLS,
   Unix-socket, client-I/O-thread, Linux daemon/supervision, wildcard include,
   and unsupported syslog settings.
5. Exercise representative application commands, hash-field expiration,
   scripts/Functions, `BGSAVE`, AOF rewrite, restart/reload, replication,
   Cluster, Sentinel, modules, service restart, monitoring, backup, and restore
   using the production configuration and dataset scale.
6. Record the annotated tag, source commit, archive checksum, host/toolchain,
   configuration, module builds, and test evidence. Do not downgrade in place
   after Redis 7.4 has written RDB12 or 7.4-only AOF/replication commands.
