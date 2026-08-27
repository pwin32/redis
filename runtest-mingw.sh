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
build_dir="${REDIS_TEST_BINARY_DIR:-$repo_root/build/mingw64}"
if [[ "$build_dir" != /* ]]; then
    build_dir="$repo_root/$build_dir"
fi
build_dir="$(cd "$build_dir" && pwd -P)"
launcher="${REDIS_TEST_LAUNCHER:-$repo_root/build/mingw64/redis-test-launcher.exe}"
module_dir="${REDIS_TEST_MODULE_DIR:-$repo_root/build/mingw64/tests/modules}"

if [[ ! -x "$TCLSH" ]]; then
    echo "error: MinGW Tcl not found at $TCLSH" >&2
    echo "install it with pacman -S mingw-w64-x86_64-tcl" >&2
    exit 1
fi

for exe in redis-server.exe redis-cli.exe redis-benchmark.exe redis-check-aof.exe redis-check-rdb.exe; do
    if [[ ! -x "$build_dir/$exe" ]]; then
        echo "error: missing $build_dir/$exe" >&2
        echo "run ./build-mingw.sh first" >&2
        exit 1
    fi
done
if [[ ! -x "$launcher" ]]; then
    echo "error: missing Redis test launcher: $launcher" >&2
    exit 1
fi

export REDIS_SERVER="$build_dir/redis-server.exe"
export REDIS_CLI="$build_dir/redis-cli.exe"
export REDIS_BENCHMARK="$build_dir/redis-benchmark.exe"
export REDIS_CHECK_AOF="$build_dir/redis-check-aof.exe"
export REDIS_CHECK_RDB="$build_dir/redis-check-rdb.exe"
export REDIS_TEST_LAUNCHER="$launcher"
export REDIS_TEST_MODULE_DIR="$module_dir"

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
        # Keep the authoritative current module-unit inventory in runtest-moduleapi.
        # Calling it here also ensures the MinGW-built modules are selected via
        # REDIS_TEST_MODULE_DIR rather than the source-tree POSIX objects.
        exec "$repo_root/runtest-moduleapi" "$@"
        ;;
esac
