# Running Redis 7.2 as a Windows service

This document describes the built-in service interface for the Redis 7.2.14
MinGW portable package. The historical Visual Studio/MSI projects under
`msvs/` are retained as best-effort compatibility files, but MSI installation
is not the current package path and has not been validated for this release.

Read [Redis 7.2 behavior in the Windows port](WINDOWS-7.2-CHANGES.md) before
deployment. Service management requires an elevated Windows token.

## Install, start, stop, and uninstall

The service action must be the first argument. If a custom name is used,
`--service-name` must follow the action and precede the configuration and Redis
options. Run these commands from an elevated Command Prompt or PowerShell:

```bat
redis-server.exe --service-install --service-name Redis7214 redis.windows-service.conf
redis-server.exe --service-start --service-name Redis7214
redis-server.exe --service-stop --service-name Redis7214
redis-server.exe --service-uninstall --service-name Redis7214
```

Installation creates an automatic-start service running as
`NT AUTHORITY\NetworkService`, but it does not start the service.
Uninstallation requests SCM deletion, but it does not stop a running service.
A running service remains marked for deletion until it stops and open SCM
handles are closed. Keep the install, start, stop, uninstall order.

SCM can report the service as `Running` before the Redis worker has finished
initializing its listener. After every `--service-start` or restart, wait for a
successful authenticated `PING` (and any required readiness command) before
accepting traffic or declaring the instance ready.

Use a unique service name for every instance. The default name is `Redis`, so
release tests and side-by-side deployments should always pass an explicit name
that cannot target an unrelated installed service.

## Paths and permissions

Use absolute executable and configuration paths when the command is not run
from the extracted package directory. At runtime the service changes its
working directory to the directory that contains `redis-server.exe`, so relative
configuration, logfile, RDB, AOF, and module paths resolve from there.

During installation, the bootstrap parser discovers the configuration file,
included configuration directories, and Redis `dir`, and grants the default
NetworkService account access to those paths. Relative paths are resolved from
the elevated install command's current directory during that discovery. Install
from the executable directory or use absolute paths so install-time ACLs and
runtime path resolution name the same locations.

Review and grant permissions separately for external ACL files, module DLLs,
log directories outside the package, certificates in a custom TLS build, and
other paths not discovered by the installer. `CONFIG REWRITE` also requires
write access to the configuration file's directory.

## Shutdown and restart behavior

`--service-stop` sends an SCM stop request. `STOP` and `PRESHUTDOWN` are routed
to Redis' graceful shutdown path on the main worker thread. The service reports
the stopped state only after the Redis worker has actually exited.

Automation should still verify all three conditions before restart, cleanup,
or binary replacement:

1. SCM reports the named service as stopped.
2. The exact Redis process ID for that service has exited.
3. The configured TCP port is closed.

Do not identify a service process only by the executable name when multiple
instances exist. Match the SCM process ID, executable path, `--service-run`, and
the exact `--service-name` command-line value.

## Logging and Event Log registration

Service mode always enables the Windows Application Event Log while retaining
the configured `logfile` sink. The packaged `redis.windows-service.conf` uses
`server_log.txt` and `syslog-enabled yes`, so it writes to both the file and the
Application Event Log. The Event Log provider/source is named `redis`; the
configured `syslog-ident` is included in the message text.

All named Redis services share that one Event Log source registration. The
first install creates the message-resource path, later named installs do not
refresh it, and uninstalling any one named Redis service removes the shared
registration. Removing one instance can therefore make events from surviving
instances render incorrectly until a Redis service is installed again.

For side-by-side services, coordinate uninstallation, keep the registered
message-resource executable available, and recreate the source from the
intended surviving Redis executable after a removal or path change.

## Multiple instances

Each instance needs its own service name, TCP port, writable data directory,
logfile, and Cluster configuration file if Cluster is enabled. For example:

```bat
redis-server.exe --service-install --service-name Redis7214A redis-a.conf --port 10001
redis-server.exe --service-start --service-name Redis7214A

redis-server.exe --service-install --service-name Redis7214B redis-b.conf --port 10002
redis-server.exe --service-start --service-name Redis7214B
```

Do not reuse a writable configuration file between Sentinel or Cluster
instances. Back up and test each instance's complete RDB or multi-part AOF
state before upgrading the service binary.
