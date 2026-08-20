#!/usr/bin/env bash
# Run the isolated elevated Windows service test for the MinGW build.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
service_script="$repo_root/tests/windows/service.ps1"
if command -v cygpath >/dev/null 2>&1; then
    script_path="$(cygpath -aw "$service_script")"
elif command -v wslpath >/dev/null 2>&1; then
    script_path="$(wslpath -w "$service_script")"
else
    printf '%s\n' 'cannot convert the service test path: cygpath/wslpath is unavailable' >&2
    exit 127
fi

exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$script_path" "$@"
