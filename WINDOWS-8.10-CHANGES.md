# Redis 8.10.1 core behavior and upgrade guide for Windows

## Scope and prospective release identity

This guide describes the unofficial Redis 8.10.1 Windows x64 MinGW port. The
Redis core baseline is the exact upstream Redis 8.10.1 tag at commit
`3399357e7c17b668289386b8a15a3037bc4527b1`, integrated with this fork's IOCP
networking, FDAPI descriptors, QFork persistence, Windows service, Event Log,
console, and MinGW64 layers.

If CI later promotes this source, the expected package naming is:

- canonical branch: `mingw-8.10`;
- package revision tag: `v8.10.1-windows.1`;
- portable archive: `Redis-x64-8.10.1-mingw-r1.zip`; and
- adjacent checksum: `Redis-x64-8.10.1-mingw-r1.zip.sha256`.

No public tag or archive is created during this pre-publish phase. A future CI
release must write `BUILDINFO.txt` with the exact source commit and tree, then
verify that the tag, archive, checksum, contents, and tested extraction agree.
A changed Windows package requires a new revision. Revision 1 combines the
completed Windows portability hardening with the upstream 8.10.1 security and
correctness fixes. The imported upstream ancestry is retained in Git history.

### Post-release source maintenance

Canonical `mingw-8.10` includes build and provenance safeguards found while
comparing the completed older maintenance lines: the standalone hiredis test
links Redis assertions, MinGW builds
fingerprint the committed jemalloc tree, checkout mtimes do not regenerate the
checked-in jemalloc `configure` script, future authorized package builds start
from `distclean` and must leave tracked sources unchanged, and release metadata
detects staged as well as unstaged `src/` and `deps/` changes.

This maintenance work does not change Redis runtime behavior. Any future public
binary must be rebuilt by CI from its selected source and pass a new Windows
package qualification matrix.

## Core-only distribution

This archive is Redis 8.10.1 core only. It does not build or ship the bundled
Redis Search, JSON, TimeSeries, probabilistic, or Vector Sets modules included
with the full upstream Redis 8 distribution. Those components have independent
build systems, worker-thread behavior, persistence paths, dependencies, and
license obligations that have not been qualified for this Windows port.

The core package includes the Redis server, Sentinel mode, CLI, benchmark, RDB
and AOF checkers, Event Log message resources, Windows configurations, and the
licenses and notices listed in the package README. Redis 8.10 core features in
this server include compact hash templates and `HIMPORT`, node-side `BACKUP`,
`LMOVEM`, `BLMOVEM`, `SDIFFCARD`, `SUNIONCARD`, and the `MAXCOUNT`/`MAXSIZE`
reply limits for `XREAD` and `XREADGROUP`.

Do not advertise this ZIP as the complete upstream Redis 8 binary
distribution. Native third-party modules must be built as x64 PE DLLs against
the included Redis Module API and validated independently.

## Licensing and notices

Redis 8 uses the tri-license payload in `LICENSE.txt`: recipients may use the
applicable Redis code under the Redis Source Available License 2.0, Server Side
Public License v1, or GNU Affero General Public License v3 terms described
there. `REDISCONTRIBUTIONS.txt` documents contribution licensing and the
portions retaining BSD-3-Clause treatment.

Binary redistribution must preserve:

- `LICENSE.txt`;
- `REDISCONTRIBUTIONS.txt`;
- `WINDOWS-NOTICES.txt`;
- `THIRD-PARTY-NOTICES.txt`;
- the Zstandard, CC0, GCC Runtime Library Exception, GPLv3, and MinGW-w64
  companion texts;
- this guide, the Windows release notes, and BUILDINFO; and
- access to the exact corresponding source revision.

The MinGW server statically links Zstandard from the MSYS2 MinGW64 package
resolved by the release workflow. `BUILDINFO.txt` records the exact package
version. The executable does not import a Zstandard DLL, but its BSD license
remains part of the binary redistribution payload.
The existing fast_float, TRE, xxHash, hiredis, jemalloc, Lua, HdrHistogram,
fpconv, linenoise, CRC, hash, and toolchain notices are reproduced in
`THIRD-PARTY-NOTICES.txt`. This guide records engineering scope and is not
legal advice.

## Windows behavior differences

| Area | Upstream Linux/POSIX behavior | This Windows core package |
| --- | --- | --- |
| Event loop | epoll, kqueue, or another POSIX backend | IOCP with synthetic FDAPI descriptors and one-shot readiness rearming |
| Network transports | TCP, Unix sockets, and optional TLS | Plain TCP over IPv4/IPv6; Unix sockets and TLS are unsupported |
| Client I/O threads | Multiple read/write I/O threads may be configured | Exactly one client I/O thread is enforced |
| Replication compression | Requires multiple client I/O threads | Compiled with static Zstandard but must remain disabled |
| Background persistence | fork copy-on-write child | QFork starts a Windows child and restores the tracked heap at matching addresses |
| Executable layout | Normal PIE/ASLR conventions | Dynamic-base and high-entropy ASLR are disabled for QFork address stability |
| Service integration | daemon, systemd, or syslog conventions | Foreground console or Windows SCM service with Application Event Log support |
| Native modules | ELF shared objects and POSIX fork assumptions | x64 PE DLLs; module-created fork children are unsupported |
| Interactive CLI | POSIX terminal handling | Native Windows console input and ANSI translation; redirected input is a separate path |
| Distribution | Upstream source/full module distribution | Flat core-only Windows x64 ZIP |

The single client-I/O-thread rule does not mean Redis has only one thread.
Background I/O and other internal workers still run where supported. It means
client socket ownership remains on the main thread until IOCP-specific
qualification proves upstream's multi-threaded client I/O safe.

## Networking, security, and replication compression

The package uses IOCP rather than a POSIX readiness backend. FDAPI values are
synthetic handles; code and modules must not treat them as raw Winsock SOCKETs
or CRT descriptors. IOCP notifications may be one-shot, so handlers that need
future events must use the owning rearm path.

The Redis write-barrier contract is preserved across separate IOCP read and
write completions. In particular, `appendfsync always` flushes the AOF before a
reply protected by the connection barrier is written.

The example configurations bind to `127.0.0.1` and keep protected mode
enabled. Before adding non-loopback addresses, configure ACL authentication,
firewall rules, and a trusted transport boundary. This package has no TLS
listener, TLS replication, TLS Cluster link, or Unix-domain socket support.
A TLS proxy or VPN is an external security component and must be assessed
separately.

Startup rejects client `io-threads` values other than 1 and rejects
`io-threads-do-reads yes`. Upstream Redis 8.10 replication compression requires
multiple client I/O threads, so keep these settings:

    io-threads 1
    io-threads-do-reads no
    repl-compression 0

Do not enable `repl-compression` merely because Zstandard is linked into the
binary. It remains outside the qualified Windows runtime surface.

Windows include paths are literal and do not support POSIX wildcard expansion.
List every included configuration file explicitly and use paths writable by
the intended console user or service account.

## Windows text, naming, and filesystem contract

Redis keeps arguments, configuration values, environment values, paths, and
diagnostics as UTF-8. The portability layer rejects invalid UTF-8 and converts
valid strings to UTF-16 for Windows `W` APIs; it does not reinterpret bytes
through the active code page. Environment-variable names and executable aliases
follow Windows case-insensitive rules. Consequently, Sentinel and checker
aliases are recognized even if their executable filename case changes.

DNS resolver input is a separate boundary. Hostnames and resolver service
labels must be ASCII; international hostnames must be supplied in punycode
form such as `xn--...`. This does not restrict the Windows SCM `service-name`
option, which is converted from UTF-8 to UTF-16, although the release service
gate uses the isolated ASCII name `RedisPortTest`.

Use normal UTF-8 drive-absolute or UNC paths in Redis configuration. File APIs
support Unicode and internally generated extended-length paths; callers should
not put raw `\\?\`, `\\.\`, device, or verbatim namespace paths into portable
configuration. The process `dir` setting is intentionally limited to a normal
path shorter than `MAX_PATH`, because the global working directory also affects
relative CRT and Win32 operations. Keep `dir` short and absolute.

On ordinary Windows volumes, slash and backslash are equivalent and filesystem
identity is case-insensitive. Redis does not normalize Unicode composition,
resolve final file IDs, or qualify per-directory case-sensitive NTFS mode. The
8.10 preload and manifest cleanup path uses Unicode ordinal case-insensitive
identity so a case-only spelling difference cannot delete the active manifest
or a referenced AOF part. Use normal case-insensitive directories and preserve
one canonical spelling for every Redis-owned file.

Windows `redis_lstat` is a stat-like compatibility operation, not a promise of
POSIX symlink semantics. Avoid symlinks, junctions, mount points, and other
reparse points in configuration, data, AOF, backup, log, and module trees.
Atomic replacement is same-volume only and uses `MoveFileExW` with replace and
write-through flags. File data is flushed, but Windows has no direct equivalent
of Redis's POSIX directory `fsync()`, so crash durability remains dependent on
the filesystem, device, and cache stack. Antivirus, backup, or indexing handles
can still cause a sharing violation after the bounded rename retry; Redis fails
the operation instead of copying across volumes or weakening atomicity.

## Compact hashes, persistence, and QFork

Redis 8.10 compact hashes share a field-name template between hashes with the
same schema. The QFork child reconnects the mapped template registry before
RDB, AOF, replication, or checker work. Template-based hashes are covered by
the source RDB/AOF, replication, corruption, defragmentation, and Module API
matrices.

RDB saves, AOF rewrites, full synchronization, and backup-related background
work use QFork. QFork starts the same `redis-server.exe` image as a child, maps
the tracked jemalloc heap at matching virtual addresses, restores changed
pages, and runs the upstream child operation. The package therefore keeps the
customized `jemalloc-5.3.0-redis` allocator and fixed-address executable
constraints.

Allow enough Windows commit/pagefile capacity for the parent, child, copied
pages, output buffers, compact-hash templates, and workload churn. Memory
pressure can fail a background operation even when the parent remains
responsive. Monitor Redis persistence status and Windows commit usage; do not
disable the pagefile for a QFork deployment without workload-specific evidence.

The examples retain disk-backed replication synchronization. Diskless
replication is not the default Windows contract and requires separate workload
qualification. For AOF deployments, preserve the complete append-only
directory and manifest together. Use the packaged checkers before recovery:

    redis-check-rdb.exe dump.rdb
    redis-check-aof.exe appendonlydir\appendonly.aof.manifest

Never copy or replace active persistence files without first stopping the
exact owning Redis process.

## BACKUP and preload-file on Windows

`BACKUP` creates a sealed multi-part AOF backup below the configured `dir`
using `backupdirname`. Keep `backupdirname` as a single directory name, not an
absolute path. The default package settings are:

    backupdirname "backupdir"
    backup-sealed-ttl 0

Backup files are hard-linked into that directory. The source and backup must
therefore be on the same Windows volume and the filesystem/account must allow
hard links. Check `BACKUP STATUS`, copy the sealed payload as one unit, and use
`BACKUP CLEANUP` deliberately; a TTL of 0 disables automatic cleanup.

`preload-file` accepts normalized drive-absolute paths with either slash style.
Forward slashes avoid configuration escaping ambiguity, for example:

    preload-file aof:C:/Redis/restore/appendonly.aof.manifest
    preload-file rdb:C:/Redis/restore/dump.rdb

The preload source must not contain `.` or `..` path components. Test restores
on an isolated port and directory before changing a production service.

## Windows service and Event Log

Service operations require an elevated Windows token. Always use an explicit,
unique service name; never experiment against an unrelated production `Redis`
service. The package README shows the supported action-first syntax.

The service example runs as the account configured by the installer or SCM and
writes inside its configured directory. Grant only the required read/write
permissions to the configuration, data, backup, and log paths. Installation
does not start a service, and uninstallation does not stop one.

`EventLog.dll` supplies message resources for the Windows Application Event
Log. Redis service registrations share the Redis Event Log source name, so
uninstalling one side-by-side service can remove the shared registration until
another service installs it again. The package service gate uses a unique
temporary name and verifies exact-image PID ownership and cleanup.

## Console and automation

In a real Windows console, redis-cli uses native console input and ANSI output
translation. Redirected stdin/stdout and the repository's fake-TTY test path
are intentionally different. Automation should use non-interactive CLI modes
and must not depend on prompt parsing, terminal width, or ANSI translation.

Server console mode writes to stdout when `logfile` is empty or `stdout`.
Service mode also enables Windows Event Log output. Review log destinations and
directory permissions before deployment.

## Upgrade and rollback

Before replacing an existing Redis instance:

1. Record the old executable version, source package, configuration, service
   name/account, persistence mode, ACLs, replication topology, and rollback
   procedure.
2. Produce and verify a final RDB or AOF backup with the old version's checker.
3. Extract this ZIP into a new directory; do not overwrite a running install.
4. Compare the deployment configuration with the packaged Windows template and
   upstream Redis 8.10 configuration changes, especially compact hashes,
   backup, preload, and replication settings.
5. Start on an isolated loopback port and validate application commands,
   persistence, restart recovery, replication, ACLs, and latency.
6. Stop the exact old process or service, wait for its PID and port to close,
   then switch using an explicit configuration and service name.
7. Retain the old binaries and verified backup until rollback is no longer
   required.

Redis persistence compatibility is not a substitute for an application-level
rollback test. New commands, compact-hash encodings, or backup layouts may not
be understood by an older server, and replication across major/minor versions
must be qualified in the actual topology.

## Package qualification contract

The Windows revision tag may be created only after all of these checks agree on
one clean source commit and one never-before-used extraction:

- clean MinGW64 core build and executable version/Git identity;
- full Redis root, Cluster, Sentinel, and Module API source suites;
- deterministic package manifest, SHA-256, ZIP CRC, and duplicate-name audit;
- exact license, notice, configuration, guide, release-note, and BUILDINFO
  payload, including the static Zstandard license and version;
- x86-64 PE type, resource, import, runtime-DLL, debug-section, and ASLR audit;
- extracted PING, ACL, RDB/AOF creation, packaged checker, and restart recovery;
- sustained packaged QFork RDB/AOF cycles under concurrent writes with exact
  child-process ownership;
- complete replication coverage pointed directly at freshly extracted
  executables;
- isolated packaged Windows service start, restart, Event Log, stop, uninstall,
  and cleanup; and
- final process, service, port, scratch-directory, Git, and checksum audit.

The package makes no benchmark or clean-machine compatibility claim unless a
separate recorded protocol supports it. The legacy Visual Studio/MSI, NuGet,
and Chocolatey paths remain quarantined and are not artifacts of this MinGW
release. Automated qualification covers non-interactive CLI use; human
interactive editing in a physical Windows console remains a deployment check,
not a package performance or terminal-compatibility claim.

## Support boundary

This is an unofficial community Windows port, not an official Redis Ltd.
Windows binary distribution. Test backups and recovery before production use.
When reporting a problem, include BUILDINFO.txt, executable `--version`
output, the exact configuration, Windows version, service/console mode, and a
minimal reproduction that does not expose secrets.
