#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd -P)"
# shellcheck source=package-process-identity.sh
source "$script_dir/package-process-identity.sh"

mock_owned_status=1
mock_alive_status=1

pid_is_owned() {
    return "$mock_owned_status"
}

pid_is_alive() {
    return "$mock_alive_status"
}

expect_status() {
    local expected_status=$1
    local description=$2
    shift 2
    local actual_status=0

    "$@" || actual_status=$?
    if (( actual_status != expected_status )); then
        printf 'error: %s: expected status %d, got %d\n' \
            "$description" "$expected_status" "$actual_status" >&2
        exit 1
    fi
}

# Model the observed CI failure: the exact token no longer matches, while an
# unrelated process is alive at the reused numeric PID.
mock_owned_status=1
mock_alive_status=0
expect_status 0 'stale token with reused live PID' \
    wait_for_pid_exit 4242 '/expected/redis-benchmark.exe' 123456789 0

# Without a creation token, a live PID remains ambiguous and must fail closed.
expect_status 2 'tokenless reused live PID' \
    wait_for_pid_exit 4242 '/expected/redis-benchmark.exe' '' 0

# A still-owned exact instance at the deadline remains a timeout.
mock_owned_status=0
mock_alive_status=0
expect_status 1 'owned exact instance at deadline' \
    wait_for_pid_exit 4242 '/expected/redis-benchmark.exe' 123456789 0

# A tokenless PID that is no longer live is an ordinary successful exit.
mock_owned_status=1
mock_alive_status=1
expect_status 0 'tokenless exited PID' \
    wait_for_pid_exit 4242 '/expected/redis-benchmark.exe' '' 0

echo 'PACKAGE_PROCESS_IDENTITY_TEST_OK'
