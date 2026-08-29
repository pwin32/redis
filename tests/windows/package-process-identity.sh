#!/usr/bin/env bash
# Shared process-instance helpers for packaged Windows qualification.
# The caller must set launcher to redis-test-launcher.exe.

pid_is_alive() {
    "$launcher" --is-alive "$1" >/dev/null 2>&1
}

pid_is_owned() {
    local pid=$1
    local expected=$2
    local token=${3:-}

    if [[ -n "$token" ]]; then
        "$launcher" --is-owned "$pid" --token "$token" "$expected" >/dev/null 2>&1
    else
        "$launcher" --is-owned "$pid" "$expected" >/dev/null 2>&1
    fi
}

wait_for_pid_exit() {
    local pid=$1
    local expected=$2
    local token=${3:-}
    local timeout_seconds=${4:-10}
    local deadline=$((SECONDS + timeout_seconds))

    while pid_is_owned "$pid" "$expected" "$token"; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done

    # A creation token identifies the exact process instance. If that instance
    # no longer owns the PID, it has exited even when Windows has already reused
    # the numeric PID for an unrelated live process.
    if [[ -n "$token" ]]; then
        return 0
    fi
    if pid_is_alive "$pid"; then
        return 2
    fi
    return 0
}
