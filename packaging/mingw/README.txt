Redis for Windows - MinGW64 portable package
=============================================

This is an unofficial Windows x64 port of Redis. It is built with the
MSYS2/MinGW64 toolchain and does not require Visual Studio or external MinGW
runtime DLLs on the target system.

Package contents:

- redis-server.exe, redis-sentinel.exe, redis-cli.exe, redis-benchmark.exe,
  redis-check-aof.exe, and redis-check-rdb.exe
- EventLog.dll - Windows Event Log message resources
- redis.windows.conf, redis.windows-service.conf, and sentinel.conf - example
  configuration files for console, Windows service, and Sentinel operation
- LICENSE.txt - Redis and Windows-port license information
- BUILDINFO.txt - exact source, tree, Windows revision, toolchain, allocator,
  and feature-scope identity
- RELEASENOTES.txt - Windows port release notes
- 00-RELEASENOTES - upstream Redis release notes
- WINDOWS-6.2-CHANGES.md - command, compatibility, configuration, persistence,
  service, module, packaging, and Windows-constraint guide

This package reports Redis 6.2.23 and is distributed as Windows revision 1:

    Redis-x64-6.2.23-mingw-r1.zip
    v6.2.23-windows.1

The tag, BUILDINFO source commit/tree, archive, and adjacent SHA-256 checksum
must agree. Read WINDOWS-6.2-CHANGES.md before upgrading.

The included configuration files are compact Windows examples, not exhaustive
Redis 6 option references. Review ACLs, bind/protected-mode settings,
persistence, replication credentials, and directory permissions before use.

Quick start from a command prompt:

    redis-server.exe redis.windows.conf
    redis-cli.exe PING

For source code and issue tracking, visit:
https://github.com/pwin32/redis
