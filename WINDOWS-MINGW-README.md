# Redis on Windows with MinGW64

This repository contains the Redis Windows portability layer and its first-class
MSYS2/MinGW64 build and test path. The supported development shell is the
MSYS2 `MINGW64` environment.

## Prerequisites

Install MSYS2 MinGW64 tools, a MinGW64 Tcl interpreter, Git, and the normal
Redis build dependencies, including OpenSSL 3 static development libraries:

```bash
pacman -S --needed make git curl tar diffutils mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-tcl mingw-w64-x86_64-openssl \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-zstd
```

When invoking the wrappers from WSL or another host
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

For focused CPU-affinity and executable-hardening checks in MINGW64 after a
build:

    ./build/mingw64/redis-affinity-test.exe
    ./runtest-mingw.sh --single windows/config --single windows/cpuaffinity --clients 1 --quiet --timeout 300
    bash tests/windows/pe-hardening.sh build/mingw64

These checks cover synthetic processor groups, native thread-mask readback,
startup configuration validation, QFork persistence affinity, and PE flags and
relocations. Physical multi-group qualification requires a host with multiple
processor groups. Focused checks do not replace the complete CI qualification.

## Performance investigations

The public validation workflow has an optional `performance_analysis` dispatch
input. Supply exact `baseline_sha` and `candidate_sha` commits to rebuild both
with one MinGW64 toolchain. It compares the baseline with TLS-enabled and
TLS-disabled candidate builds over plaintext TCP, using one baseline benchmark
client, separate client/server CPU affinity, two complete rotations of the
variants, and 250,000 requests per test. An optional `reference_sha` includes
the original TLS-enabled build in the same run when evaluating a fix.
Raw samples, binary identities, command counts,
and user/kernel CPU time are retained in the performance artifact.

Separate builds use `-pg` for gprof call graphs and sampling. Their timings are
kept separate from the uninstrumented A/B results. The profiled server keeps
the existing fixed image base, which gprof needs to resolve Windows samples.
Profiles do not attribute time inside Windows DLLs; use the recorded kernel
CPU measurements alongside the call graphs. This workflow builds diagnostic
binaries without creating release packages or release qualification evidence.

## Native TLS

The standard build includes built-in TLS and statically links OpenSSL 3. It
requires no OpenSSL DLLs. `./build-mingw.sh BUILD_TLS=no -j2` builds an explicit
plaintext-only variant; switching `BUILD_TLS=yes|no` rebuilds the affected
objects and hiredis archive. Other values, including `module`, are rejected.

TLS remains disabled until configured. Client connections, CLI, benchmark,
`rediss://` URLs, replication, Cluster links and Sentinel use the native TLS
transport. Diskless synchronization, including a dedicated RDB channel, keeps
encryption in the parent process while QFork writes the snapshot to a pipe.

For a TLS-only listener, set `port 0`, `tls-port 6379`, `tls-cert-file`,
`tls-key-file`, and `tls-ca-cert-file`. Client certificates are required by
default. Use `tls-replication yes` on replicas and Sentinel, and `tls-cluster yes`
for Cluster bus connections. Configure `tls-client-cert-file` and
`tls-client-key-file` when the outbound certificate differs from the server
certificate. `tls-expected-peer-name` verifies configured peer identities in
addition to CA trust. The packaged configuration files contain commented examples.

Use quoted certificate paths with forward slashes, for example
`"C:/Redis/certificates/server.crt"`; UTF-8 paths with spaces and Unicode are
supported. The service account needs read access to its private key and
certificates. To renew certificates, install the new files and issue one
`CONFIG SET tls-cert-file ... tls-key-file ... tls-ca-cert-file ...`. Redis
validates the new context before replacing it; existing connections retain
their context until they reconnect. Persist the changed paths in the
configuration file or with `CONFIG REWRITE`.

Focused native checks:

```bash
make -f Makefile.mingw interop-test
./build/mingw64/interop-test.exe --legacy
./build/mingw64/interop-test.exe --modern
./runtest-mingw.sh --tls --single unit/tls --single windows/tls --clients 1 --quiet --timeout 600
./runtest-mingw.sh --tls --single unit/cluster/cluster-response-tls --single unit/cluster/tls-peer-impersonation --clients 1 --quiet --timeout 600
./runtest-mingw.sh --sentinel --tls --single 00-base
```

`--tls` prepares TclTLS 1.7.22 from its checksum-pinned official release in
ignored `.local/test-deps/` and generates ignored test certificates. Only the
test harness receives its Tcl package path. Tests and keys are excluded from
packages. The suite selector (`--cluster`, `--sentinel`, or `--moduleapi`) must
precede `--tls`. Full suites run in CI; service tests accept `-TLS` after the
TLS fixtures have been prepared.

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
tests in TLS and plaintext modes, explicit no-TLS builds, and Windows interop
coverage. Pushes to canonical `mingw-*` branches run the complete Windows
qualification, including the source suites, service integration,
extracted-package audits, packaged replication, a 30-minute QFork persistence
soak for each transport, and a short benchmark.
Plaintext and TLS qualification run in parallel on separate Windows runners.
Each runner runs the complete Root, Cluster, Sentinel and Module API suites,
service tests, packaged replication and persistence soak in sequence against
the same stripped package from a single build. Checksums and binary identity
are verified before and after testing; release evidence requires both modes
to pass. Native test helpers are transferred separately from the core package.

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
