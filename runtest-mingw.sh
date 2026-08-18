#!/usr/bin/env bash
#
# Run the Redis TCL test suite against the MSYS2 MinGW64 build.
#
# The normal ./runtest script prefers /usr/bin/tclsh8.6 in MSYS2, which is the
# MSYS runtime Tcl and reports tcl_platform(platform)=unix. Use the MinGW Tcl so
# the Windows branches in the Redis test harness are active.

source "$(cd "$(dirname "$0")" && pwd)/scripts/mingw-bootstrap.sh"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
cd "$repo_root"

suite=root
case "${1:-}" in
    --cluster)
        suite=cluster
        shift
        ;;
    --sentinel)
        suite=sentinel
        shift
        ;;
    --moduleapi)
        suite=moduleapi
        shift
        ;;
esac

TCLSH=/mingw64/bin/tclsh
BUILD_DIR="$repo_root/build/mingw64"

if [[ ! -x "$TCLSH" ]]; then
    echo "error: MinGW Tcl not found at $TCLSH" >&2
    echo "install it with pacman -S mingw-w64-x86_64-tcl" >&2
    exit 1
fi

for exe in redis-server.exe redis-cli.exe redis-benchmark.exe redis-check-aof.exe redis-check-rdb.exe redis-test-launcher.exe; do
    if [[ ! -x "$BUILD_DIR/$exe" ]]; then
        echo "error: missing $BUILD_DIR/$exe" >&2
        echo "run ./build-mingw.sh first" >&2
        exit 1
    fi
done

export REDIS_SERVER="$BUILD_DIR/redis-server.exe"
export REDIS_CLI="$BUILD_DIR/redis-cli.exe"
export REDIS_BENCHMARK="$BUILD_DIR/redis-benchmark.exe"
export REDIS_CHECK_AOF="$BUILD_DIR/redis-check-aof.exe"
export REDIS_CHECK_RDB="$BUILD_DIR/redis-check-rdb.exe"
export REDIS_TEST_LAUNCHER="$BUILD_DIR/redis-test-launcher.exe"

case "$suite" in
    root)
        exec "$TCLSH" tests/test_helper.tcl "$@"
        ;;
    cluster)
        exec "$TCLSH" tests/cluster/run.tcl "$@"
        ;;
    sentinel)
        exec "$TCLSH" tests/sentinel/run.tcl "$@"
        ;;
    moduleapi)
        # Keep the authoritative 7.2 module-unit inventory in runtest-moduleapi.
        # Calling it here also ensures the MinGW-built modules are selected via
        # REDIS_TEST_MODULE_DIR rather than the source-tree POSIX objects.
        exec "$repo_root/runtest-moduleapi" "$@"
        ;;
esac
