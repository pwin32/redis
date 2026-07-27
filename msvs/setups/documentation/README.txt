Redis for Windows - legacy Visual Studio/MSI documentation staging
=================================================================

This directory belongs to the historical Visual Studio/MSI packaging path. It
is retained as best-effort compatibility material and is not the supported
Redis 7.4.10 MinGW release package.

MSI, NuGet, and Chocolatey creation/publication is quarantined; see
..\..\LEGACY-PACKAGING.md. These configuration templates are staged into the
MinGW ZIP independently of that legacy publication path.

The current release path is the MinGW64 flat ZIP produced by
package-mingw.sh. Read the repository root README.md,
WINDOWS-7.4-CHANGES.md, Windows Service Documentation.md, and
RELEASENOTES.txt before using the files in this directory.

The redis.windows.conf and redis.windows-service.conf files remain the source
templates staged into the MinGW portable archive. Their active directives are
validated through the MinGW package and runtime gates; comments for less common
options are compatibility reference material rather than a complete Redis 7.4
configuration manual.
