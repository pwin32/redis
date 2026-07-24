# Historical MSOpenTech Redis on Windows design note

This filename is retained so old links do not disappear, but the former
contents described the MSOpenTech Redis 2.8-era port. Its Visual Studio-first
build instructions, `--maxheap` and fixed pagefile formulas, NuGet/Chocolatey
release status, service-document references, performance claims, and xcopy
upgrade advice do not describe the Redis 7.2.15 MinGW release.

Use the current documentation instead:

- [Redis for Windows](README.md) for the supported build and quick start.
- [Redis 7.2 behavior in the Windows port](WINDOWS-7.2-CHANGES.md) for explicit
  differences from original Redis on Linux/POSIX and for upgrade constraints.
- [Running Redis 7.2 as a Windows service](Windows%20Service%20Documentation.md)
  for the current built-in SCM service interface.
- [Windows release notes](RELEASENOTES.txt) for current Windows releases.

The historical text remains available in Git history.
