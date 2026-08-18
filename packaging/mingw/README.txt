Redis for Windows - MinGW64 portable package
=============================================

This is an unofficial Windows x64 Redis 7.2.16 revision 1 core build. It is built
with the MSYS2/MinGW64 toolchain and is configured not to require Visual Studio
or external MinGW runtime DLLs on the target system.

This is intentionally a Redis core package. It does not contain prebuilt
third-party Redis modules and must not be described as a modules bundle.

Package contents:

- redis-server.exe, redis-sentinel.exe, redis-cli.exe, redis-benchmark.exe,
  redis-check-aof.exe, and redis-check-rdb.exe
- EventLog.dll - Windows Event Log message resources
- BUILDINFO.txt and PACKAGE-MANIFEST.txt - exact source, toolchain, allocator,
  package identity, file sizes, and hashes
- redis.windows.conf, redis.windows-service.conf, and sentinel.conf - example
  configuration files for console, Windows service, and Sentinel operation
- COPYING - upstream Redis BSD-3-Clause license
- LICENSE.txt - Windows port BSD-3-Clause notice
- RELEASENOTES.txt - Windows port release notes and validation scope
- 00-RELEASENOTES - upstream Redis release notes
- WINDOWS-7.2-CHANGES.md - current Redis 7.2 Windows constraints and upgrade guide

This package reports Redis 7.2.16. A public CI release uses
`Redis-x64-7.2.16-mingw-r1.zip` and tag `v7.2.16-windows.1`; no tag or archive
is created locally. Verify the adjacent SHA-256 file, BUILDINFO.txt,
PACKAGE-MANIFEST.txt, release test report, and selected source revision before
deployment. Read RELEASENOTES.txt before upgrading.

The included server configurations are Windows-oriented compatibility templates,
not complete Redis 7.2 manuals. Their active directives have reviewed defaults,
but some explanatory comments remain reference text inherited from upstream
templates. Read WINDOWS-7.2-CHANGES.md first, then review ACLs,
bind/protected-mode settings, the intentional disk-backed replication default,
persistence, replication credentials, and directory permissions before use.

Console server quick start from a command prompt:

    redis-server.exe redis.windows.conf
    redis-cli.exe PING

Sentinel quick start:

    redis-sentinel.exe sentinel.conf
    redis-cli.exe -p 26379 PING

The packaged Sentinel example binds only to 127.0.0.1, enables protected mode,
and monitors a local Redis server named `mymaster` on port 6379. Edit its
address, quorum, authentication, and announce settings for the deployment.
Sentinel rewrites its configuration, so start it with a writable copy.
One Sentinel can monitor and report state, but this example's quorum of 2
requires additional independent Sentinel instances before failover can be
authorized.

From an elevated command prompt, use an isolated service name and keep the
service action as the first argument:

    redis-server.exe --service-install --service-name Redis7216 redis.windows-service.conf
    redis-server.exe --service-start --service-name Redis7216
    redis-server.exe --service-stop --service-name Redis7216
    redis-server.exe --service-uninstall --service-name Redis7216

Installation does not start the service, and uninstallation does not stop it.
The example service configuration writes `server_log.txt` and also uses the
Windows Application Event Log. Console mode uses stdout or `logfile`; set
`syslog-enabled yes` to additionally enable Application Event Log output.
The Event Log source registration is shared globally: uninstalling any named
Redis service removes that registration and can affect message rendering for
other side-by-side Redis services until one is installed again.

For source code and issue tracking, visit:
https://github.com/pwin32/redis
