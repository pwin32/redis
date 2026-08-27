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

if [[ -n "${REDIS_TEST_BINARY_DIR:-}" ]]; then
    build_dir=$REDIS_TEST_BINARY_DIR
    if [[ "$build_dir" != /* ]]; then
        build_dir="$repo_root/$build_dir"
    fi
    build_dir="$(cd "$build_dir" && pwd -P)"
    if command -v cygpath >/dev/null 2>&1; then
        build_path="$(cygpath -aw "$build_dir")"
    else
        build_path="$(wslpath -w "$build_dir")"
    fi
    MSYS2_ARG_CONV_EXCL='*' exec powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$script_path" -BuildDir "$build_path" "$@"
fi

MSYS2_ARG_CONV_EXCL='*' exec powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$script_path" "$@"
