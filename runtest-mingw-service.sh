#!/usr/bin/env bash
# Run the isolated elevated Windows service test for the MinGW build.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
script_path="$(wslpath -w "$repo_root/tests/windows/service.ps1")"

exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$script_path" "$@"
