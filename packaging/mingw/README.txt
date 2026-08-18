Redis for Windows - MinGW64 portable package
=============================================

This is an unofficial Windows x64 Redis 7.4.11 revision 1 qualification build. It is built
with the MSYS2/MinGW64 toolchain and is configured not to require Visual Studio
or external MinGW runtime DLLs on the target system.

Package contents:

- redis-server.exe, redis-sentinel.exe, redis-cli.exe, redis-benchmark.exe,
  redis-check-aof.exe, and redis-check-rdb.exe
- EventLog.dll - Windows Event Log message resources
- redis.windows.conf, redis.windows-service.conf, and sentinel.conf - example
  configuration files for console, Windows service, and Sentinel operation
- LICENSE.txt - Redis source license terms
- REDISCONTRIBUTIONS.txt - Redis contribution licensing terms
- WINDOWS-NOTICES.txt - Windows port BSD-3-Clause notice
- THIRD-PARTY-NOTICES.txt - bundled dependency attribution and license notices
- CC0-1.0.txt, GPL-3.0.txt, GCC-RUNTIME-LIBRARY-EXCEPTION.txt,
  GCC-RUNTIME-README.txt, and MINGW-W64-RUNTIME.txt - complete companion
  license texts for statically linked code
- RELEASENOTES.txt - Windows port release notes and validation scope
- 00-RELEASENOTES - upstream Redis release notes
- WINDOWS-7.4-CHANGES.md - current Redis 7.4 Windows constraints and upgrade guide

This package reports Redis 7.4.11 and is produced only by the CI qualification
workflow as `Redis-x64-7.4.11-mingw-r1.zip`. The 7.4 line is qualification-only
under the current publication policy, so no public 7.4 tag or release is
created. Verify the adjacent SHA-256 file, BUILDINFO, and selected source
revision in CI. Read RELEASENOTES.txt before deployment.

The included server configurations are Windows-oriented compatibility templates,
not complete Redis 7.4 manuals. Their active directives have reviewed defaults,
but some explanatory comments remain reference text inherited from upstream
templates. Read WINDOWS-7.4-CHANGES.md first, then review ACLs,
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

    redis-server.exe --service-install --service-name Redis7411 redis.windows-service.conf
    redis-server.exe --service-start --service-name Redis7411
    redis-server.exe --service-stop --service-name Redis7411
    redis-server.exe --service-uninstall --service-name Redis7411

Installation does not start the service, and uninstallation does not stop it.
The example service configuration writes `server_log.txt` and also uses the
Windows Application Event Log. Console mode uses stdout or `logfile`; set
`syslog-enabled yes` to additionally enable Application Event Log output.
The Event Log source registration is shared globally: uninstalling any named
Redis service removes that registration and can affect message rendering for
other side-by-side Redis services until one is installed again.

For source code and issue tracking, visit:
https://github.com/pwin32/redis
