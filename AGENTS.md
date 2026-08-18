# Repository Guidance

## Repository purpose

This branch maintains the Redis 8.8.1 Windows/MinGW64 port on `mingw-8.8`.
Related canonical Windows lines cover Redis 6.2.23, 7.2.15, 7.4.10, and
8.10.0.

The important source areas are:

- `src/` — Redis core plus guarded Windows adaptations.
- `src/Win32_Interop/` — IOCP, file-descriptor, QFork, pthread/time/signal,
  Event Log, console, and service support.
- `deps/` — bundled dependencies, including the branch-specific customized
  jemalloc trees used by QFork.
- `msvs/` — legacy Visual Studio projects; preserve them where practical.

## Build and test

The supported Windows build path is MSYS2/MinGW64. See
`WINDOWS-MINGW-README.md` for prerequisites and local configuration.

```bash
./build-mingw.sh -j2
./runtest-mingw.sh --clients 1 --quiet --timeout 600
./runtest-mingw.sh --cluster
./runtest-mingw.sh --sentinel
./runtest-mingw.sh --moduleapi --clients 1 --quiet --timeout 300
./runtest-mingw-service.sh
```

When invoked outside an MSYS2 `MINGW64` shell, provide the shell location with
`--msys-bash PATH`, `MSYS_BASH`, or the ignored `.local/mingw.env` file. Never
commit machine-specific paths or local configuration.

The service test requires an elevated Windows token. QFork, service, Event Log,
console, and Windows process behavior require real Windows coverage.

## Porting rules

- Prefer upstream Redis code shape for core logic.
- Keep Windows behavior behind the existing `_WIN32`, `IF_WIN32`, or
  `WIN32_ONLY` patterns, or inside `src/Win32_Interop/`.
- Update manual Visual Studio project item lists when source files change.
- Treat QFork persistence, jemalloc hooks, IOCP networking, and file-descriptor
  emulation as high-risk compatibility boundaries.
- Do not replace bundled allocator or compression dependencies casually.
- Keep Windows client I/O single-threaded until separately qualified.
- Keep core-only package scope explicit; bundled Redis modules are not implied.

## Public-source hygiene

- Keep private notes, machine configuration, scan maps, and local tools under
  ignored `.local/`.
- Do not commit credentials, private endpoints, usernames, machine-specific
  absolute paths, worktree locations, PIDs, or internal release operations.
- Generic path examples and portability patterns are allowed when they are
  required by code, documentation, or tests.
- Run `scripts/check-public-hygiene.sh --history` before publication.
- Do not create release tags or upload packages locally. Public releases are
  created only by an explicitly enabled CI release workflow after full Windows
  qualification.

## Editing practices

Use `rg` for navigation and keep patches local to the touched subsystem.
Preserve unrelated working-tree changes. Verify important edits with an
independent read before relying on build results. Do not commit generated
binaries, release headers, build directories, database files, or test output.
