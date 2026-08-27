# Redis on Windows with MinGW64

This repository contains the Redis Windows portability layer and its first-class
MSYS2/MinGW64 build and test path. The supported development shell is the
MSYS2 `MINGW64` environment.

## Prerequisites

Install MSYS2 MinGW64 tools, a MinGW64 Tcl interpreter, Git, and the normal
Redis build dependencies. When invoking the wrappers from WSL or another host
shell, select MSYS2 with one of these mechanisms:

```bash
export MSYS_BASH=/path/to/msys2/usr/bin/bash.exe
./build-mingw.sh -j2
```

or:

```bash
./build-mingw.sh --msys-bash /path/to/msys2/usr/bin/bash.exe -j2
```

For repeated local use, put the same `MSYS_BASH=...` assignment in the ignored
`.local/mingw.env` file. The wrappers do not source arbitrary local scripts.
When they are already running inside an MSYS2 `MINGW64` shell, no path is
needed.

## Build and test

```bash
./build-mingw.sh -j2
./runtest-mingw.sh --clients 1 --quiet --timeout 600
./runtest-mingw.sh --cluster
./runtest-mingw.sh --sentinel
./runtest-mingw.sh --moduleapi --clients 1 --quiet --timeout 300
./runtest-mingw-service.sh
```

The service test needs an elevated Windows token. Use private test ports and
do not stop or reconfigure unrelated Redis services.

## Windows port boundaries

The Windows path uses IOCP networking, the Win32 file-descriptor layer, and the
QFork persistence implementation. Client I/O remains single-threaded until a
separate Windows qualification proves otherwise. Core-only package builds keep
bundled Redis modules outside the release boundary.

The first release qualification boundary is x86-64 PE binaries built with the
MSYS2/MinGW64 toolchain on GitHub Actions `windows-2022` runners. Windows
Server 2025 runs are useful forward-compatibility evidence, but do not replace
the pinned release qualification. x86-32, MSVC, and legacy Windows Server
2012 R2 are not release targets yet.

## CI and releases

Pull requests run public-source hygiene checks, a MinGW build, focused Redis
tests, and Windows interop smoke coverage. Pushes to canonical `mingw-*`
branches run the complete Windows qualification, including the source suites,
service integration, extracted-package audits, packaged replication, a
30-minute QFork persistence soak, and a short benchmark.

Public releases are dispatched manually from the default branch with an exact
canonical-branch commit and expected tag. CI performs one clean package build,
strips that candidate before testing, extracts it, and runs the source suites
and packaged checks against those same release bytes. A final hash and archive
checksum gate runs before CI emits test evidence, an SPDX SBOM, and GitHub
artifact attestations. The release workflow publishes that uploaded artifact
without rebuilding it. No release package or tag is created locally. GitHub's
repository-level release immutability setting must be enabled before the first
public release.

See [RELEASE-POLICY.md](RELEASE-POLICY.md) for the maintained-line order,
review policy, benchmark contract, and the small set of GitHub administrator
settings that cannot be represented in source.

## Troubleshooting

- `MSYS2 bash is not configured`: set `MSYS_BASH`, pass `--msys-bash PATH`, or
  create `.local/mingw.env`.
- `MinGW Tcl not found`: install the MinGW64 Tcl package and run the wrapper
  from an MSYS2 `MINGW64` shell.
- stale allocator or dependency state after switching branches: run
  `./build-mingw.sh distclean` before rebuilding.
