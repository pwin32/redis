#!/usr/bin/env bash
#
# Build the Windows Redis port with the MSYS2 MinGW64 toolchain.
#
source "$(cd "$(dirname "$0")" && pwd)/scripts/mingw-bootstrap.sh"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
cd "$repo_root"

echo "==> msystem:   ${MSYSTEM:-unknown}"
echo "==> toolchain: $(gcc --version | sed -n '1p')"
echo "==> makefile:  Makefile.mingw"

exec make -f Makefile.mingw "$@"
