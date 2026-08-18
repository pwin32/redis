#!/usr/bin/env bash
#
# Exercise a freshly extracted Windows package under repeated QFork RDB/AOF
# persistence.  The repository launcher is the process-ownership boundary;
# Redis traffic and persistence validation use only binaries from PACKAGE_DIR.

source "$(cd "$(dirname "$0")/../.." && pwd)/scripts/mingw-bootstrap.sh"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

set -euo pipefail

usage() {
    cat <<'EOF'
usage: tests/windows/package-soak.sh PACKAGE_DIR PORT [DURATION_SECONDS [MIN_CYCLES]]

Run the packaged Redis server for at least DURATION_SECONDS (default: 600)
and at least MIN_CYCLES alternating BGSAVE/BGREWRITEAOF cycles (default: 16).
PACKAGE_DIR must be a fresh extraction containing the packaged server, CLI,
benchmark, and RDB/AOF checkers.  PORT must be an unused local TCP port.

Optional environment variables:
  REDIS_PACKAGE_SOAK_SEED_REQUESTS    seed SET requests (default: 100000)
  REDIS_PACKAGE_SOAK_WRITER_REQUESTS  SET requests per cycle (default: 100000)
  REDIS_PACKAGE_SOAK_KEEP             keep scratch data after success when 1
  REDIS_PACKAGE_EXPECTED_VERSION      expected Redis version (default: src/version.h)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
if (( $# < 2 || $# > 4 )); then
    usage >&2
    exit 2
fi

package_arg=$1
port=$2
duration=${3:-600}
min_cycles=${4:-16}
seed_requests=${REDIS_PACKAGE_SOAK_SEED_REQUESTS:-100000}
writer_requests=${REDIS_PACKAGE_SOAK_WRITER_REQUESTS:-100000}
keep_success=${REDIS_PACKAGE_SOAK_KEEP:-0}

# WSL callers commonly pass /mnt/<drive>/... while the re-executed MSYS2
# shell sees the same drive as /<drive>/....
package_arg="$(printf '%s' "$package_arg" | sed 's|^/mnt/\([a-zA-Z]\)/|/\1/|')"

is_uint() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

if ! is_uint "$port" || (( port < 1 || port > 65535 )); then
    echo "error: PORT must be an integer from 1 through 65535" >&2
    exit 2
fi
if ! is_uint "$duration" || (( duration < 1 || duration > 86400 )); then
    echo "error: DURATION_SECONDS must be an integer from 1 through 86400" >&2
    exit 2
fi
if ! is_uint "$min_cycles" || (( min_cycles < 1 || min_cycles > 10000 )); then
    echo "error: MIN_CYCLES must be an integer from 1 through 10000" >&2
    exit 2
fi
for request_count in "$seed_requests" "$writer_requests"; do
    if ! is_uint "$request_count" || (( request_count < 1000 || request_count > 10000000 )); then
        echo "error: benchmark request counts must be integers from 1000 through 10000000" >&2
        exit 2
    fi
done
if [[ "$keep_success" != "0" && "$keep_success" != "1" ]]; then
    echo "error: REDIS_PACKAGE_SOAK_KEEP must be 0 or 1" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
expected_version="${REDIS_PACKAGE_EXPECTED_VERSION:-}"
if [[ -z "$expected_version" ]]; then
    expected_version="$(sed -n 's/^#define REDIS_VERSION "\([^"]*\)"/\1/p' "$repo_root/src/version.h")"
fi
if [[ -z "$expected_version" ]]; then
    echo "error: unable to determine expected Redis package version" >&2
    exit 1
fi
if [[ "$package_arg" != /* ]]; then
    package_arg="$repo_root/$package_arg"
fi
if [[ ! -d "$package_arg" ]]; then
    echo "error: package directory not found: $package_arg" >&2
    exit 1
fi
package_dir="$(cd "$package_arg" && pwd -P)"

server="$package_dir/redis-server.exe"
cli="$package_dir/redis-cli.exe"
benchmark="$package_dir/redis-benchmark.exe"
check_rdb="$package_dir/redis-check-rdb.exe"
check_aof="$package_dir/redis-check-aof.exe"
buildinfo="$package_dir/BUILDINFO.txt"
launcher="$repo_root/build/mingw64/redis-test-launcher.exe"

for executable in "$server" "$cli" "$benchmark" "$check_rdb" "$check_aof" "$launcher"; do
    if [[ ! -f "$executable" ]]; then
        echo "error: required executable not found: $executable" >&2
        exit 1
    fi
done
if [[ ! -f "$buildinfo" ]]; then
    echo "error: required package provenance file not found: $buildinfo" >&2
    exit 1
fi
for tool in grep mktemp taskkill.exe; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required MSYS2/Windows tool not found: $tool" >&2
        exit 1
    fi
done

scratch_dir="$(mktemp -d "$repo_root/build/mingw64/package-soak-${port}-XXXXXXXX")"
config_aof="$scratch_dir/soak-aof.conf"
config_rdb="$scratch_dir/soak-rdb.conf"
redis_log="$scratch_dir/redis.log"
rdb_file="$scratch_dir/soak.rdb"
aof_manifest="$scratch_dir/appendonlydir/soak.aof.manifest"

server_pid=""
writer_pid=""
server_token=""
writer_token=""
qfork_child_token=""
server_label=""
completed=0
cycles=0
cycle_qfork_observations=0
final_qfork_observations=0
expected_dbsize=""
expected_digest=""
qfork_child_pid=""

log() {
    printf '==> %s\n' "$*"
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

show_log_tail() {
    local path=$1
    if [[ -s "$path" ]]; then
        printf '%s\n' "--- $path (tail) ---" >&2
        tail -n 80 "$path" >&2 || true
    fi
}

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
    if pid_is_alive "$pid"; then
        return 2
    fi
    return 0
}

terminate_exact_pid() {
    local pid=$1
    local expected=$2
    local token=${3:-}
    local description=${4:-process}

    if ! is_uint "$pid" || (( pid == 0 )); then
        return 0
    fi
    if [[ -n "$token" ]] && pid_is_owned "$pid" "$expected" "$token"; then
        log "terminating exact $description PID $pid"
        if ! "$launcher" --terminate "$pid" --token "$token" "$expected" >/dev/null 2>&1; then
            warn "atomic terminate rejected for $description PID $pid"
            return 1
        fi
        if ! wait_for_pid_exit "$pid" "$expected" "$token" 10; then
            warn "$description PID $pid remains after exact-PID termination"
            return 1
        fi
    elif pid_is_owned "$pid" "$expected"; then
        log "terminating exact $description PID $pid without creation token"
        MSYS2_ARG_CONV_EXCL='*' taskkill.exe /F /T /PID "$pid" >/dev/null 2>&1 || true
        if ! wait_for_pid_exit "$pid" "$expected" "" 10; then
            warn "$description PID $pid remains after exact-PID termination"
            return 1
        fi
    elif pid_is_alive "$pid"; then
        warn "refusing to terminate live PID $pid: executable ownership does not match $expected"
        return 1
    fi
    return 0
}

cleanup() {
    local status=$?
    local cleanup_failed=0

    trap - EXIT INT TERM
    set +e

    if [[ -n "$writer_pid" ]]; then
        terminate_exact_pid "$writer_pid" "$benchmark" "$writer_token" "benchmark writer" || cleanup_failed=1
        writer_pid=""
        writer_token=""
    fi
    if [[ -n "$qfork_child_pid" ]]; then
        terminate_exact_pid "$qfork_child_pid" "$server" "$qfork_child_token" "QFork child" || cleanup_failed=1
        qfork_child_pid=""
        qfork_child_token=""
    fi
    if [[ -n "$server_pid" ]]; then
        terminate_exact_pid "$server_pid" "$server" "$server_token" "packaged server" || cleanup_failed=1
        server_pid=""
        server_token=""
    fi

    if (( cleanup_failed != 0 && status == 0 )); then
        status=1
    fi

    if (( status == 0 && completed == 1 && keep_success == 0 )); then
        case "$scratch_dir" in
            "$repo_root"/build/mingw64/package-soak-"$port"-*)
                rm -rf -- "$scratch_dir"
                ;;
            *)
                warn "refusing to remove unexpected scratch path: $scratch_dir"
                status=1
                ;;
        esac
    else
        printf 'PACKAGE_SOAK_ARTIFACTS=%s\n' "$scratch_dir" >&2
    fi

    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

write_configuration() {
    local path=$1
    local appendonly=$2

    cat >"$path" <<EOF
bind 127.0.0.1
protected-mode yes
port $port
persistence-available yes
dir "."
dbfilename "soak.rdb"
save ""
appendonly $appendonly
appendfilename "soak.aof"
appenddirname "appendonlydir"
appendfsync everysec
auto-aof-rewrite-percentage 0
aof-use-rdb-preamble yes
enable-debug-command local
logfile "redis.log"
EOF
}

launch_process() {
    local result_var=$1
    local token_var=$2
    local stdout_log=$3
    local stderr_log=$4
    local executable=$5
    shift 5
    local launched
    local launched_pid
    local launched_token

    if ! launched="$("$launcher" --emit-token "$stdout_log" "$stderr_log" -- "$executable" "$@")"; then
        show_log_tail "$stderr_log"
        die "native launcher could not start $executable"
    fi
    launched="${launched//$'\r'/}"
    launched="${launched//$'\n'/}"
    launched_pid="${launched%% *}"
    launched_token="${launched#* }"
    if [[ "$launched_pid" == "$launched_token" ]]; then
        launched_token=""
    fi
    if ! is_uint "$launched_pid" || (( launched_pid == 0 )) || \
            ! is_uint "$launched_token" || (( launched_token == 0 )); then
        die "native launcher returned an invalid PID for $executable: $launched"
    fi
    printf -v "$result_var" '%s' "$launched_pid"
    printf -v "$token_var" '%s' "$launched_token"
}

redis_raw() {
    "$cli" -h 127.0.0.1 -p "$port" --raw "$@"
}

redis_scalar() {
    local value
    if ! value="$(redis_raw "$@")"; then
        return 1
    fi
    value="${value//$'\r'/}"
    printf '%s' "$value"
}

info_value() {
    local section=$1
    local key=$2
    local output line

    if ! output="$(redis_raw INFO "$section")"; then
        return 1
    fi
    while IFS= read -r line; do
        line="${line//$'\r'/}"
        if [[ "$line" == "$key:"* ]]; then
            printf '%s' "${line#*:}"
            return 0
        fi
    done <<<"$output"
    return 1
}

assert_server_endpoint() {
    local actual_pid

    if [[ -z "$server_pid" ]] || ! pid_is_owned "$server_pid" "$server" "$server_token"; then
        die "packaged server PID ${server_pid:-unknown} is not alive with the expected executable"
    fi
    if ! actual_pid="$(info_value server process_id)"; then
        die "unable to read process_id from the packaged server on port $port"
    fi
    if [[ "$actual_pid" != "$server_pid" ]]; then
        die "port $port belongs to Redis PID $actual_pid, expected exact packaged PID $server_pid"
    fi
}

wait_for_server() {
    local deadline=$((SECONDS + 30))
    local pong actual_pid

    while (( SECONDS < deadline )); do
        if pid_is_owned "$server_pid" "$server" "$server_token"; then
            pong="$(redis_scalar PING 2>/dev/null || true)"
            if [[ "$pong" == "PONG" ]]; then
                actual_pid="$(info_value server process_id 2>/dev/null || true)"
                if [[ "$actual_pid" == "$server_pid" ]]; then
                    return 0
                fi
                if [[ -n "$actual_pid" ]]; then
                    die "port $port answered from Redis PID $actual_pid, expected $server_pid"
                fi
            fi
        elif pid_is_alive "$server_pid"; then
            die "server PID $server_pid is alive with an unexpected executable"
        else
            show_log_tail "$scratch_dir/${server_label}.stderr.log"
            show_log_tail "$redis_log"
            die "packaged server PID $server_pid exited during startup"
        fi
        sleep 0.1
    done

    show_log_tail "$scratch_dir/${server_label}.stderr.log"
    show_log_tail "$redis_log"
    die "packaged server PID $server_pid did not accept connections on port $port"
}

start_server() {
    local config=$1
    local config_name=${config##*/}
    local previous_dir=$PWD
    server_label=$2

    cd "$scratch_dir"
    launch_process server_pid server_token \
        "$scratch_dir/${server_label}.stdout.log" \
        "$scratch_dir/${server_label}.stderr.log" \
        "$server" "$config_name"
    cd "$previous_dir"
    wait_for_server
    assert_server_endpoint
    log "$server_label started as exact packaged PID $server_pid"
}

stop_server() {
    local pid=$server_pid
    local shutdown_status=0

    assert_server_endpoint
    redis_raw SHUTDOWN NOSAVE >/dev/null 2>&1 || shutdown_status=$?
    if ! wait_for_pid_exit "$pid" "$server" "$server_token" 30; then
        die "packaged server PID $pid did not exit after SHUTDOWN NOSAVE"
    fi
    server_pid=""
    server_token=""
    if (( shutdown_status != 0 )); then
        warn "redis-cli returned $shutdown_status while the exact packaged server shut down cleanly"
    fi
}

wait_for_benchmark() {
    local pid=$1
    local token=$2
    local stdout_log=$3
    local stderr_log=$4

    if ! wait_for_pid_exit "$pid" "$benchmark" "$token" 180; then
        show_log_tail "$stdout_log"
        show_log_tail "$stderr_log"
        die "packaged benchmark PID $pid did not exit within 180 seconds"
    fi
    if ! grep -aEq 'SET: .*requests per second' "$stdout_log"; then
        show_log_tail "$stdout_log"
        show_log_tail "$stderr_log"
        die "packaged benchmark PID $pid did not report a completed SET test"
    fi
    if grep -aEiq '(error|failed|connection refused|could not connect)' "$stderr_log"; then
        show_log_tail "$stderr_log"
        die "packaged benchmark PID $pid reported an error"
    fi
}

run_seed_benchmark() {
    local pid
    local token
    local stdout_log="$scratch_dir/seed-benchmark.stdout.log"
    local stderr_log="$scratch_dir/seed-benchmark.stderr.log"

    launch_process pid token "$stdout_log" "$stderr_log" "$benchmark" \
        -h 127.0.0.1 -p "$port" -c 50 -n "$seed_requests" \
        -r 200000 -d 512 -P 16 -t set -q
    writer_pid=$pid
    writer_token=$token
    wait_for_benchmark "$pid" "$token" "$stdout_log" "$stderr_log"
    writer_pid=""
    writer_token=""
}

start_cycle_writer() {
    local cycle=$1
    local stdout_log="$scratch_dir/writer-${cycle}.stdout.log"
    local stderr_log="$scratch_dir/writer-${cycle}.stderr.log"

    launch_process writer_pid writer_token "$stdout_log" "$stderr_log" "$benchmark" \
        -h 127.0.0.1 -p "$port" -c 50 -n "$writer_requests" \
        -r 200000 -d 512 -P 16 -t set -q
    if ! pid_is_owned "$writer_pid" "$benchmark" "$writer_token"; then
        show_log_tail "$stdout_log"
        show_log_tail "$stderr_log"
        die "packaged benchmark writer PID $writer_pid exited before cycle $cycle began"
    fi
}

finish_cycle_writer() {
    local cycle=$1
    local pid=$writer_pid

    wait_for_benchmark "$pid" "$writer_token" \
        "$scratch_dir/writer-${cycle}.stdout.log" \
        "$scratch_dir/writer-${cycle}.stderr.log"
    writer_pid=""
    writer_token=""
}

wait_for_qfork_child() {
    local writer_required=$1
    local deadline=$((SECONDS + 15))
    local child
    local token

    qfork_child_pid=""
    qfork_child_token=""
    while (( SECONDS < deadline )); do
        child="$("$launcher" --find-qfork-child "$server_pid" "$server" 2>/dev/null || true)"
        child="${child//$'\r'/}"
        child="${child//$'\n'/}"
        if is_uint "$child" && (( child > 0 )); then
            token="$("$launcher" --creation-token "$child" 2>/dev/null || true)"
            token="${token//$'\r'/}"
            token="${token//$'\n'/}"
            if ! is_uint "$token" || (( token == 0 )); then
                die "unable to capture creation token for QFork child PID $child"
            fi
            if ! pid_is_owned "$child" "$server" "$token"; then
                die "QFork child PID $child is not the expected packaged server image"
            fi
            qfork_child_pid=$child
            qfork_child_token=$token
            if (( writer_required != 0 )) && ! pid_is_owned "$writer_pid" "$benchmark" "$writer_token"; then
                die "benchmark writer PID $writer_pid was not active when QFork PID $child was observed"
            fi
            return 0
        fi
        assert_server_endpoint
        if (( writer_required != 0 )) && ! pid_is_owned "$writer_pid" "$benchmark" "$writer_token"; then
            die "benchmark writer PID $writer_pid exited before a QFork child was observed"
        fi
        sleep 0.025
    done
    return 1
}

wait_for_background_operation() {
    local operation=$1
    local progress_key status_key
    local deadline=$((SECONDS + 180))
    local in_progress status

    if [[ "$operation" == "BGSAVE" ]]; then
        progress_key=rdb_bgsave_in_progress
        status_key=rdb_last_bgsave_status
    else
        progress_key=aof_rewrite_in_progress
        status_key=aof_last_bgrewrite_status
    fi

    while (( SECONDS < deadline )); do
        assert_server_endpoint
        in_progress="$(info_value persistence "$progress_key" || true)"
        if [[ "$in_progress" == "0" ]]; then
            status="$(info_value persistence "$status_key" || true)"
            if [[ "$status" != "ok" ]]; then
                die "$operation completed with $status_key=${status:-missing}"
            fi
            return 0
        fi
        sleep 0.1
    done
    die "$operation did not complete within 180 seconds"
}

run_background_operation() {
    local operation=$1
    local phase=$2
    local writer_required=$3
    local reply child

    assert_server_endpoint
    if ! reply="$(redis_scalar "$operation")"; then
        die "$operation command failed"
    fi
    if [[ "$reply" != Background*started ]]; then
        die "unexpected $operation reply: $reply"
    fi
    if ! wait_for_qfork_child "$writer_required"; then
        die "no exact QFork child was observed for $operation"
    fi
    child=$qfork_child_pid
    if [[ "$phase" == "cycle" ]]; then
        ((cycle_qfork_observations += 1))
    else
        ((final_qfork_observations += 1))
    fi
    log "$operation observed exact QFork PID $child"
    wait_for_background_operation "$operation"
    if ! wait_for_pid_exit "$child" "$server" "$qfork_child_token" 10; then
        die "QFork child PID $child remained after $operation completed"
    fi
    qfork_child_pid=""
    qfork_child_token=""
}

assert_nonempty_file() {
    if [[ ! -s "$1" ]]; then
        die "missing or empty persistence file: $1"
    fi
}

check_persistence_files() {
    local rdb_output="$scratch_dir/redis-check-rdb.log"
    local aof_output="$scratch_dir/redis-check-aof.log"

    assert_nonempty_file "$rdb_file"
    assert_nonempty_file "$aof_manifest"
    if ! "$check_rdb" "$rdb_file" >"$rdb_output" 2>&1; then
        show_log_tail "$rdb_output"
        die "packaged redis-check-rdb rejected $rdb_file"
    fi
    if ! "$check_aof" "$aof_manifest" >"$aof_output" 2>&1; then
        show_log_tail "$aof_output"
        die "packaged redis-check-aof rejected $aof_manifest"
    fi
    log "packaged RDB and multi-part AOF checkers passed"
}

verify_restart() {
    local mode=$1
    local config=$2
    local marker cycle_count dbsize digest

    start_server "$config" "${mode}-restart"
    marker="$(redis_scalar GET soak:marker || true)"
    cycle_count="$(redis_scalar GET soak:cycles || true)"
    dbsize="$(redis_scalar DBSIZE || true)"
    digest="$(redis_scalar DEBUG DIGEST || true)"
    if [[ "$marker" != "final-$cycles" ]]; then
        die "$mode restart marker mismatch: expected final-$cycles, found ${marker:-missing}"
    fi
    if [[ "$cycle_count" != "$cycles" ]]; then
        die "$mode restart cycle counter mismatch: expected $cycles, found ${cycle_count:-missing}"
    fi
    if [[ "$dbsize" != "$expected_dbsize" ]]; then
        die "$mode restart DBSIZE mismatch: expected $expected_dbsize, found ${dbsize:-missing}"
    fi
    if [[ "$digest" != "$expected_digest" ]]; then
        die "$mode restart dataset digest mismatch: expected $expected_digest, found ${digest:-missing}"
    fi
    stop_server
    log "$mode restart recovered marker, cycle counter, exact DBSIZE, and full dataset digest"
}

write_configuration "$config_aof" yes
write_configuration "$config_rdb" no

log "package: $package_dir"
log "scratch: $scratch_dir"
log "gate: at least ${duration}s and at least $min_cycles cycles on port $port"

start_server "$config_aof" initial
redis_version="$(info_value server redis_version || true)"
redis_git_sha1="$(info_value server redis_git_sha1 || true)"
redis_git_dirty="$(info_value server redis_git_dirty || true)"
if [[ "$redis_version" != "$expected_version" ]]; then
    die "packaged server version mismatch: expected $expected_version, found ${redis_version:-missing}"
fi
if [[ ! "$redis_git_sha1" =~ ^[0-9a-f]{8}$ || "$redis_git_dirty" != "0" ]]; then
    die "packaged server Git identity is not a clean release build: sha=${redis_git_sha1:-missing} dirty=${redis_git_dirty:-missing}"
fi
buildinfo_commit="$(sed -n 's/^Source commit: //p' "$buildinfo")"
if [[ ! "$buildinfo_commit" =~ ^[0-9a-f]{40}$ || "${buildinfo_commit:0:8}" != "$redis_git_sha1" ]]; then
    die "BUILDINFO/source identity mismatch: buildinfo=${buildinfo_commit:-missing} binary=$redis_git_sha1"
fi
run_seed_benchmark
if [[ "$(redis_scalar SET soak:marker seed || true)" != "OK" ]]; then
    die "unable to write the soak marker"
fi
if [[ "$(redis_scalar CONFIG SET rdb-key-save-delay 200 || true)" != "OK" ]]; then
    die "unable to enable the bounded RDB key-save delay"
fi

start_seconds=$SECONDS
while (( SECONDS - start_seconds < duration || cycles < min_cycles )); do
    cycle=$((cycles + 1))
    if (( cycles % 2 == 0 )); then
        operation=BGSAVE
    else
        operation=BGREWRITEAOF
    fi

    start_cycle_writer "$cycle"
    run_background_operation "$operation" cycle 1
    finish_cycle_writer "$cycle"
    cycles=$cycle
    if [[ "$(redis_scalar SET soak:marker "cycle-$cycles" || true)" != "OK" ]]; then
        die "unable to update the cycle marker"
    fi
    if ! redis_scalar INCR soak:cycles >/dev/null; then
        die "unable to increment the cycle counter"
    fi
    elapsed=$((SECONDS - start_seconds))
    printf 'SOAK_PROGRESS elapsed=%ss cycles=%s operation=%s qfork_pid_count=%s\n' \
        "$elapsed" "$cycles" "$operation" "$cycle_qfork_observations"
done

if (( cycle_qfork_observations != cycles )); then
    die "observed $cycle_qfork_observations QFork children for $cycles soak cycles"
fi
soak_elapsed=$((SECONDS - start_seconds))
if [[ "$(redis_scalar SET soak:marker "final-$cycles" || true)" != "OK" ]]; then
    die "unable to write the final soak marker"
fi
run_background_operation BGSAVE final 0
run_background_operation BGREWRITEAOF final 0
if (( final_qfork_observations != 2 )); then
    die "observed $final_qfork_observations QFork children for two final persistence operations"
fi
if [[ "$(redis_scalar CONFIG SET rdb-key-save-delay 0 || true)" != "OK" ]]; then
    die "unable to clear the RDB key-save delay"
fi
expected_dbsize="$(redis_scalar DBSIZE || true)"
if ! is_uint "$expected_dbsize"; then
    die "invalid final DBSIZE: ${expected_dbsize:-missing}"
fi
expected_digest="$(redis_scalar DEBUG DIGEST || true)"
if [[ ! "$expected_digest" =~ ^[0-9a-f]{40}$ ]]; then
    die "invalid final dataset digest: ${expected_digest:-missing}"
fi
stop_server

check_persistence_files
verify_restart AOF "$config_aof"
verify_restart RDB "$config_rdb"

for diagnostic_log in "$redis_log" "$scratch_dir"/*.stderr.log "$scratch_dir"/*.stdout.log; do
    if [[ -f "$diagnostic_log" ]] && \
            grep -aEiq '(assert|access violation|stack trace|qfork child failed|fatal error|out of memory|physicalmapmemory.*failed|virtualfree.*failed|unhandled exception)' \
                "$diagnostic_log"; then
        show_log_tail "$diagnostic_log"
        die "fatal diagnostic found in packaged runtime output"
    fi
done

total_elapsed=$((SECONDS - start_seconds))
completed=1
printf 'PACKAGE_SOAK_OK version=%s git=%s requested_duration=%ss soak_elapsed=%ss total_elapsed=%ss cycles=%s qfork_observations=%s final_qfork_observations=%s dbsize=%s digest=%s\n' \
    "$redis_version" "$redis_git_sha1" "$duration" "$soak_elapsed" "$total_elapsed" \
    "$cycles" "$cycle_qfork_observations" "$final_qfork_observations" \
    "$expected_dbsize" "$expected_digest"
