# Redis Windows Porting History

This document records the public technical milestones of the MinGW64 Windows
port. Internal session logs, machine paths, process identifiers, and hosting
operations are intentionally excluded.

## Redis 5.0 baseline

The original Windows fork was restored to a reproducible MinGW64 build and a
Redis 5.0.14 behavior baseline. The work established the repository-owned
build/test wrappers, IOCP and file-descriptor compatibility fixes, QFork
persistence coverage, Tcl harness integration, and service/Event Log testing.

## Redis 6.2

The port advanced incrementally to Redis 6.2.23. This line retained the legacy
`deps/jemalloc-5.2.1` allocator tree and received focused QFork, replication,
module, service, process-ownership, and package hardening.

## Redis 7.2

Redis 7.2.15 established the customized `deps/jemalloc` 5.3-based Windows
allocator path. The port updated command generation, Module API coverage,
cluster behavior, persistence, networking, and the manually maintained Visual
Studio source lists.

## Redis 7.4

Redis 7.4.10 was integrated as a Windows source and security baseline. It is a
canonical maintenance branch but was not promoted as a qualified public
Windows package release.

## Redis 8.8

The Redis 8 port advanced through the upstream 8.0.6, 8.2.8, 8.4.5, 8.6.5,
and 8.8.1 checkpoints. The first Redis 8 deliverable remained core-only,
retained single-threaded Windows client I/O, and kept bundled Redis modules out
of package scope.

## Redis 8.10

Redis 8.10.0 is the current Windows core line. It includes additional source,
LLP64, Unicode, process, console, QFork, packaging, and dependency hardening.
Zstandard is linked for the supported core build, while replication
compression remains disabled on Windows pending separate threaded-I/O
qualification.

## Public release policy

Canonical public branches are maintained separately from private integration
records. Validation CI may build and test branch updates, but it does not
publish artifacts or create tags. A public release requires a separately
enabled CI workflow and the complete Windows qualification matrix.
