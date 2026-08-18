# Redis 8.10 Windows Source Audit

Date: August 18, 2026

## Outcome

The Redis 8.10.0 Windows core source was reviewed for portability, LLP64,
Unicode, process, persistence, and build-boundary problems. The canonical
MinGW64 line contains the required Windows adaptations and remains intentionally
limited to the documented Windows feature and packaging boundaries.

This public record summarizes technical findings. Machine-specific evidence,
private worktree information, process identifiers, internal hosting operations,
and unpublished release records are excluded.

## Source boundaries reviewed

- Redis core sources and generated command definitions.
- `src/Win32_Interop/`, including IOCP, file-descriptor emulation, QFork,
  pthread/time/signal compatibility, console, Event Log, and service code.
- The customized jemalloc tree and its QFork page-tracking hooks.
- MinGW64 build rules, dependency selection, and link inputs.
- Tcl test harness process creation and executable ownership checks.
- Core-only package manifests, notices, configuration examples, and aliases.

## Implemented design

### Unicode and Windows APIs

Redis strings remain UTF-8 internally. Windows filesystem, console, process,
service, and Event Log boundaries convert explicitly to UTF-16 and use wide
Windows APIs where required. Conversion failures propagate as errors rather
than silently falling back to a different path or identity.

### Paths and identity

Windows path comparison accounts for separator and case differences where the
operating system treats them as equivalent. Long-path handling, executable
ownership checks, and service identity checks avoid broad name-only process
matching. Generic WSL/MSYS drive conversion is retained as portability logic;
machine-specific installation paths are not part of tracked source.

### LLP64 correctness

Pointer-width, handle, size, and format conversions were reviewed for the
Windows LLP64 data model. Integer truncation and signedness fixes remain local
to the Windows boundary where possible, while core logic follows upstream
types and control flow.

### Networking and processes

The Windows server uses IOCP and the repository file-descriptor abstraction.
Process launch, wait, termination, and test cleanup use exact process identity
and executable ownership boundaries. Windows client I/O remains restricted to
one thread pending separate qualification of upstream threaded-I/O behavior.

### Persistence and QFork

RDB and AOF child behavior uses the QFork implementation and allocator hooks to
track and restore heap pages. Child discovery, collection, error propagation,
module state, and cleanup received focused regression coverage. Branch changes
must continue to treat QFork and allocator-generated state as high-risk areas.

### Build and dependencies

The supported build is MSYS2/MinGW64 through the repository wrappers and
`Makefile.mingw`. The wrappers accept their machine-specific MSYS2 shell path
through CLI, environment, or ignored local configuration. Redis 8.10 links the
supported Zstandard dependency statically for core functionality; runtime
replication compression remains disabled on Windows.

### Package scope

The intended Windows package contains Redis core executables, configuration,
licenses, notices, and provenance. Bundled Redis modules, legacy MSI/NuGet/
Chocolatey publication, TLS, Unix-domain sockets, multiple client I/O threads,
and runtime replication compression are outside the qualified core scope.

## Validation model

The validation path includes:

- clean MinGW64 builds;
- focused interop and executable-identity tests;
- Redis root, Cluster, Sentinel, and Module API suites;
- QFork RDB/AOF persistence and package soak coverage;
- service and Event Log testing with an elevated Windows token;
- package manifest, notice, PE, import, and checksum audits.

The public validation workflow added during repository sanitization performs
tracked-content hygiene, a MinGW64 build, interop coverage, and focused Redis
tests only. It cannot publish packages or create tags. Full release validation
and publication are deferred to a separately authorized CI workflow.

## Cross-branch rule

Security and portability fixes must be evaluated independently on the 6.2,
7.2, 7.4, 8.8, and 8.10 Windows lines. The older 6.2 line uses its legacy
allocator tree; newer lines use the customized jemalloc tree. Do not reuse
generated allocator state across branch switches.

## Remaining caveats

- Real-console Unicode editing and grapheme behavior require physical Windows
  console coverage.
- Service tests require elevation and cannot be represented by an ordinary
  unprivileged hosted-runner smoke job.
- Clean-machine deployment, antivirus/filter drivers, extreme memory pressure,
  and application-specific upgrade or rollback remain deployment concerns.
- Legacy Visual Studio packaging is best-effort and is not part of the public
  MinGW64 release path.

## Publication boundary

This audit does not authorize a public package or release tag. Public releases
will use independent versioned tags and artifacts created only after the full
CI release matrix succeeds.
