Redis for Windows - MinGW64 portable package
=============================================

This is an unofficial Windows x64 Redis 6.2.24 revision 1 core build. It is
built with the MSYS2/MinGW64 toolchain and does not require Visual Studio or
external MinGW runtime DLLs on the target system.

This is intentionally a Redis core package. It does not contain prebuilt
third-party Redis modules and must not be described as a modules bundle.

Package contents:

- redis-server.exe, redis-sentinel.exe, redis-cli.exe, redis-benchmark.exe,
  redis-check-aof.exe, and redis-check-rdb.exe
- EventLog.dll - Windows Event Log message resources
- redis.windows.conf, redis.windows-service.conf, and sentinel.conf - example
  configuration files for console, Windows service, and Sentinel operation
- COPYING - upstream Redis BSD-3-Clause license
- LICENSE.txt - Redis and Windows-port BSD-3-Clause notice
- BUILDINFO.txt and PACKAGE-MANIFEST.txt - exact source, tree, Windows revision,
  toolchain, allocator, file sizes, hashes, and feature-scope identity
- RELEASENOTES.txt - Windows port release notes and validation scope
- 00-RELEASENOTES - upstream Redis release notes
- WINDOWS-6.2-CHANGES.md - command, compatibility, configuration, persistence,
  service, module, packaging, and Windows-constraint guide

This package reports Redis 6.2.24. A public CI release uses
`Redis-x64-6.2.24-mingw-r1.zip` and tag `v6.2.24-windows.1`; no tag or archive
is created locally. The tag, BUILDINFO source commit/tree, archive, package
manifest, release test report, and adjacent SHA-256 checksum must agree. Read
WINDOWS-6.2-CHANGES.md before upgrading.

The included configuration files are compact Windows examples, not exhaustive
Redis 6 option references. Review ACLs, bind/protected-mode settings,
persistence, replication credentials, and directory permissions before use.

Quick start from a command prompt:

    redis-server.exe redis.windows.conf
    redis-cli.exe PING

Sentinel quick start:

    redis-sentinel.exe sentinel.conf
    redis-cli.exe -p 26379 PING

The packaged Sentinel example is copied from the upstream 6.2 template with
its working directory made relative to the extracted package. Review its bind,
quorum, authentication, announce, and writable-config requirements before use.

From an elevated command prompt, use an isolated service name and keep the
service action as the first argument:

    redis-server.exe --service-install --service-name Redis6224 redis.windows-service.conf
    redis-server.exe --service-start --service-name Redis6224
    redis-server.exe --service-stop --service-name Redis6224
    redis-server.exe --service-uninstall --service-name Redis6224

Installation does not start the service, and uninstallation does not stop it.
The example service configuration writes its logfile and also uses the Windows
Application Event Log. Event Log source registration is shared globally across
side-by-side Redis services.

For source code and issue tracking, visit:
https://github.com/pwin32/redis
