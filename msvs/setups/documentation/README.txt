Redis for Windows - legacy Visual Studio/MSI documentation staging
=================================================================

This directory belongs to the historical Visual Studio/MSI packaging path. It
is retained as best-effort compatibility material and is not the supported or
validated Redis 7.2.15 release package.

The current release path is the MinGW64 flat ZIP produced by
package-mingw.sh. Read the repository root README.md,
WINDOWS-7.2-CHANGES.md, Windows Service Documentation.md, and
RELEASENOTES.txt before using the files in this directory.

The redis.windows.conf and redis.windows-service.conf files remain the source
templates staged into the MinGW portable archive. Their active directives are
validated through the MinGW package and runtime gates; comments for less common
options are compatibility reference material rather than a complete Redis 7.2
configuration manual.
