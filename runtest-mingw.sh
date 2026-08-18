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

for exe in redis-server.exe redis-cli.exe redis-benchmark.exe redis-check-aof.exe redis-check-rdb.exe; do
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
        make -f Makefile.mingw test-modules
        export REDIS_TEST_MODULE_DIR="$BUILD_DIR/tests/modules"
        exec "$TCLSH" tests/test_helper.tcl \
            --single unit/moduleapi/commandfilter \
            --single unit/moduleapi/basics \
            --single unit/moduleapi/fork \
            --single unit/moduleapi/testrdb \
            --single unit/moduleapi/infotest \
            --single unit/moduleapi/propagate \
            --single unit/moduleapi/hooks \
            --single unit/moduleapi/misc \
            --single unit/moduleapi/blockonkeys \
            --single unit/moduleapi/blockonbackground \
            --single unit/moduleapi/scan \
            --single unit/moduleapi/datatype \
            --single unit/moduleapi/auth \
            --single unit/moduleapi/keyspace_events \
            --single unit/moduleapi/blockedclient \
            --single unit/moduleapi/getkeys \
            --single unit/moduleapi/test_lazyfree \
            --single unit/moduleapi/defrag \
            --single unit/moduleapi/hash \
            --single unit/moduleapi/zset \
            --single unit/moduleapi/stream \
            --single unit/moduleapi/timer \
            --single unit/moduleapi/cluster "$@"
        ;;
esac
