#!/usr/bin/env bash
# Compare two packaged Redis servers with the candidate package's benchmark
# client.  This wrapper re-execs under MSYS2 MinGW64 so native process launch,
# path conversion, and toolchain DLL lookup match the release test environment.

source "$(cd "$(dirname "$0")/../.." && pwd)/scripts/mingw-bootstrap.sh"
caller_abs="$(pwd -P)"
caller_msys="$(printf '%s' "$caller_abs" | sed 's|^/mnt/\([a-zA-Z]\)/|/\1/|')"
export REDIS_PACKAGE_BENCHMARK_CALLER_PWD="$caller_msys"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

set -euo pipefail
export LC_ALL=C

usage() {
    printf '%s\n' \
        "usage: $0 --baseline-dir DIR --candidate-dir DIR --port PORT --output-dir DIR" \
        "" \
        "Runs a 10,000-request warmup per test, then three 50,000-request rounds." \
        "Measured order alternates baseline-candidate / candidate-baseline." \
        "The candidate package's redis-benchmark.exe and redis-cli.exe are" \
        "used against both servers. Existing output directories must be empty."
}

die() {
    echo "error: $*" >&2
    exit 1
}

require_option_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        die "missing value for $1"
    fi
}

baseline_arg=
candidate_arg=
port=
output_arg=

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baseline-dir)
            require_option_value "$@"
            baseline_arg=$2
            shift 2
            ;;
        --candidate-dir)
            require_option_value "$@"
            candidate_arg=$2
            shift 2
            ;;
        --port)
            require_option_value "$@"
            port=$2
            shift 2
            ;;
        --output-dir)
            require_option_value "$@"
            output_arg=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown argument: $1"
            ;;
    esac
done

[[ -n "$baseline_arg" ]] || die "--baseline-dir is required"
[[ -n "$candidate_arg" ]] || die "--candidate-dir is required"
[[ -n "$port" ]] || die "--port is required"
[[ -n "$output_arg" ]] || die "--output-dir is required"
[[ "$port" =~ ^[0-9]+$ ]] || die "--port must be an integer"
(( ${#port} <= 5 )) || die "--port must be between 1 and 65535"
port=$((10#$port))
(( port >= 1 && port <= 65535 )) || die "--port must be between 1 and 65535"

for value in "$baseline_arg" "$candidate_arg" "$output_arg"; do
    if [[ "$value" == *$'\n'* || "$value" == *$'\r'* || "$value" == *$'\t'* ]]; then
        die "path arguments must not contain tabs or newlines"
    fi
done

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
invocation_dir=${REDIS_PACKAGE_BENCHMARK_CALLER_PWD:-$PWD}

to_msys_path() {
    local path=$1

    if [[ "$path" =~ ^/mnt/([a-zA-Z])(/.*)?$ ]]; then
        path="/${BASH_REMATCH[1]}${BASH_REMATCH[2]:-}"
    elif [[ "$path" =~ ^[a-zA-Z]:[\\/].*$ ]]; then
        command -v cygpath >/dev/null 2>&1 || die "cygpath is required for Windows path arguments"
        path="$(cygpath -u "$path")"
    fi
    if [[ "$path" != /* ]]; then
        path="$invocation_dir/$path"
    fi
    printf '%s\n' "$path"
}

canonical_existing_dir() {
    local path
    path="$(to_msys_path "$1")"
    [[ -d "$path" ]] || die "directory not found: $1"
    (cd "$path" && pwd -P)
}

baseline_dir="$(canonical_existing_dir "$baseline_arg")"
candidate_dir="$(canonical_existing_dir "$candidate_arg")"
[[ "$baseline_dir" != "$candidate_dir" ]] || die "baseline and candidate directories must differ"

output_dir="$(to_msys_path "$output_arg")"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd -P)"
if find "$output_dir" -mindepth 1 -print -quit | grep -q .; then
    die "output directory must be empty: $output_dir"
fi

for tool in awk cp date find grep sed sha256sum sleep sort taskkill.exe tr; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done

launcher="$repo_root/build/mingw64/redis-test-launcher.exe"
baseline_server="$baseline_dir/redis-server.exe"
candidate_server="$candidate_dir/redis-server.exe"
benchmark="$candidate_dir/redis-benchmark.exe"
cli="$candidate_dir/redis-cli.exe"

for executable in "$launcher" "$baseline_server" "$candidate_server" "$benchmark" "$cli"; do
    [[ -x "$executable" ]] || die "required executable not found: $executable"
done

mkdir -p "$output_dir/versions" "$output_dir/runs"

metadata="$output_dir/metadata.tsv"
matrix_file="$output_dir/matrix.tsv"
schedule_file="$output_dir/schedule.tsv"
warmup_results="$output_dir/warmup.tsv"
measurements="$output_dir/measurements.tsv"
summary="$output_dir/summary.tsv"
assessment="$output_dir/assessment.tsv"
config_template="$output_dir/server.conf"

printf 'key\tvalue\n' > "$metadata"
metadata_row() {
    local key=$1
    local value=$2
    value=${value//$'\r'/ }
    value=${value//$'\n'/ }
    value=${value//$'\t'/ }
    printf '%s\t%s\n' "$key" "$value" >> "$metadata"
}

metadata_row format_version 1
metadata_row started_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
metadata_row msystem "${MSYSTEM:-unknown}"
metadata_row windows_processor_identifier "${PROCESSOR_IDENTIFIER:-unknown}"
metadata_row windows_logical_processors "${NUMBER_OF_PROCESSORS:-unknown}"
if command -v cmd.exe >/dev/null 2>&1; then
    metadata_row windows_version "$(MSYS2_ARG_CONV_EXCL='*' cmd.exe /d /c ver 2>/dev/null || printf unknown)"
fi
if command -v powercfg.exe >/dev/null 2>&1; then
    metadata_row windows_power_scheme "$(MSYS2_ARG_CONV_EXCL='*' powercfg.exe /GETACTIVESCHEME 2>/dev/null || printf unknown)"
fi
if command -v git >/dev/null 2>&1; then
    metadata_row repository_head "$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || printf unknown)"
fi
metadata_row port "$port"
metadata_row host_address 127.0.0.1
metadata_row seed "redis-benchmark default (Redis 6.2 has no --seed option)"
metadata_row keepalive 1
metadata_row warmup_passes_per_variant 1
metadata_row warmup_requests_per_test 10000
metadata_row measured_requests_per_test 50000
metadata_row measured_rounds 3
metadata_row benchmark_client candidate
metadata_row baseline_server_sha256 "$(sha256sum "$baseline_server" | awk '{print $1}')"
metadata_row candidate_server_sha256 "$(sha256sum "$candidate_server" | awk '{print $1}')"
metadata_row candidate_benchmark_sha256 "$(sha256sum "$benchmark" | awk '{print $1}')"
metadata_row candidate_cli_sha256 "$(sha256sum "$cli" | awk '{print $1}')"
metadata_row launcher_sha256 "$(sha256sum "$launcher" | awk '{print $1}')"

printf '%s\n' \
    'bind 127.0.0.1' \
    "port $port" \
    'protected-mode yes' \
    'daemonize no' \
    'supervised no' \
    'save ""' \
    'appendonly no' \
    'dir .' \
    'dbfilename dump.rdb' \
    'logfile ""' \
    'loglevel notice' > "$config_template"
metadata_row server_config_sha256 "$(sha256sum "$config_template" | awk '{print $1}')"

printf '%s\n' \
    $'matrix_id\tclients\tpipeline\twarmup_requests\tmeasured_requests\tkeyspace\tdatasize_bytes\tthreads\tselected_tests\texpected_csv_rows' \
    $'ping-c50-p1\t50\t1\t10000\t50000\t-\tdefault\t0\tping\tPING_INLINE,PING_MBULK' \
    $'set-get-c50-p1-r100k-d512\t50\t1\t10000\t50000\t100000\t512\t0\tset,get\tSET,GET' \
    > "$matrix_file"

printf 'sequence\tphase\tround\torder_in_round\tvariant\n' > "$schedule_file"
schedule_sequence=0
append_schedule() {
    schedule_sequence=$((schedule_sequence + 1))
    printf '%d\t%s\t%d\t%d\t%s\n' \
        "$schedule_sequence" "$1" "$2" "$3" "$4" >> "$schedule_file"
}
append_schedule warmup 0 1 baseline
append_schedule warmup 0 2 candidate
for round in 1 2 3; do
    if (( round % 2 == 1 )); then
        append_schedule measured "$round" 1 baseline
        append_schedule measured "$round" 2 candidate
    else
        append_schedule measured "$round" 1 candidate
        append_schedule measured "$round" 2 baseline
    fi
done

results_header=$'phase\tround\tsequence\torder_in_round\tvariant\tmatrix_id\ttest\trps\tavg_latency_ms\tmin_latency_ms\tp50_latency_ms\tp95_latency_ms\tp99_latency_ms\tmax_latency_ms\traw_stdout'
printf '%s\n' "$results_header" > "$warmup_results"
printf '%s\n' "$results_header" > "$measurements"

record_command() {
    local destination=$1
    shift

    printf 'command=' > "$destination"
    printf '%q ' "$@" >> "$destination"
    printf '\n' >> "$destination"
}

record_version() {
    local name=$1
    shift
    record_command "$output_dir/versions/$name.command.txt" "$@"
    if ! "$@" > "$output_dir/versions/$name.stdout.txt" \
            2> "$output_dir/versions/$name.stderr.txt"; then
        die "version command failed: $name"
    fi
}

record_version baseline-server "$baseline_server" --version
record_version candidate-server "$candidate_server" --version
record_version candidate-benchmark "$benchmark" --version
record_version candidate-cli "$cli" --version

active_pid=
active_token=
active_server=
active_run_dir=

server_is_owned() {
    local pid=$1
    local server=$2
    local token=${3:-}
    if [[ -n "$token" ]]; then
        "$launcher" --is-owned "$pid" --token "$token" "$server" >/dev/null 2>&1
    else
        "$launcher" --is-owned "$pid" "$server" >/dev/null 2>&1
    fi
}

stop_active_server() {
    local cleanup_log
    local attempt

    [[ -n "$active_pid" ]] || return 0
    cleanup_log="$active_run_dir/cleanup.log"
    printf 'pid=%s\ntoken=%s\nexpected_image=%s\n' "$active_pid" "${active_token:-}" "$active_server" >> "$cleanup_log"

    if server_is_owned "$active_pid" "$active_server" "$active_token"; then
        printf 'action=shutdown-nosave\n' >> "$cleanup_log"
        "$cli" --raw -h 127.0.0.1 -p "$port" SHUTDOWN NOSAVE \
            >> "$cleanup_log" 2>&1 || true
        for attempt in {1..100}; do
            if ! server_is_owned "$active_pid" "$active_server" "$active_token"; then
                break
            fi
            sleep 0.1
        done
    fi

    if server_is_owned "$active_pid" "$active_server" "$active_token"; then
        printf 'action=terminate-exact-instance\n' >> "$cleanup_log"
        if [[ -n "$active_token" ]]; then
            if ! "$launcher" --terminate "$active_pid" --token "$active_token" "$active_server" \
                    >> "$cleanup_log" 2>&1; then
                printf 'error=terminate-failed\n' >> "$cleanup_log"
            fi
        else
            if ! MSYS2_ARG_CONV_EXCL='*' taskkill.exe /F /T /PID "$active_pid" \
                    >> "$cleanup_log" 2>&1; then
                printf 'error=taskkill-failed\n' >> "$cleanup_log"
            fi
        fi
        for attempt in {1..100}; do
            if ! server_is_owned "$active_pid" "$active_server" "$active_token"; then
                break
            fi
            sleep 0.1
        done
    fi

    if server_is_owned "$active_pid" "$active_server" "$active_token"; then
        printf 'error=owned-process-remains\n' >> "$cleanup_log"
        return 1
    fi

    active_pid=
    active_token=
    active_server=
    active_run_dir=
    return 0
}

cleanup_on_exit() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "$active_pid" ]] && ! stop_active_server; then
        echo "error: exact owned Redis PID $active_pid could not be stopped" >&2
        status=1
    fi
    exit "$status"
}
trap cleanup_on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

start_server() {
    local server=$1
    local run_dir=$2
    local pid_output
    local attempt
    local pong

    cp "$config_template" "$run_dir/server.conf"
    [[ "$(sha256sum "$run_dir/server.conf" | awk '{print $1}')" == \
       "$(sha256sum "$config_template" | awk '{print $1}')" ]] || \
        die "server configuration changed while staging $run_dir"

    record_command "$run_dir/server.command.txt" \
        "$launcher" --emit-token server.stdout.log server.stderr.log -- "$server" server.conf
    if ! (
        cd "$run_dir"
        "$launcher" --emit-token server.stdout.log server.stderr.log -- "$server" server.conf \
            > launcher.stdout.txt 2> launcher.stderr.log
    ); then
        die "native launcher failed for $server; see $run_dir"
    fi
    pid_output="$(< "$run_dir/launcher.stdout.txt")"
    pid_output=${pid_output//$'\r'/}
    pid_output=${pid_output//$'\n'/}
    local token_output
    token_output="${pid_output#* }"
    pid_output="${pid_output%% *}"
    if [[ "$pid_output" == "$token_output" ]]; then
        token_output=""
    fi
    [[ "$pid_output" =~ ^[1-9][0-9]*$ && "$token_output" =~ ^[1-9][0-9]*$ ]] || \
        die "native launcher returned an invalid process identity: $pid_output $token_output"

    active_pid=$pid_output
    active_token=$token_output
    active_server=$server
    active_run_dir=$run_dir
    printf '%s\n' "$active_pid" > "$run_dir/server.pid"
    printf '%s\n' "$active_token" > "$run_dir/server.token"

    if ! server_is_owned "$active_pid" "$active_server" "$active_token"; then
        die "launched PID $active_pid is not the expected server image"
    fi

    for attempt in {1..1200}; do
        if ! server_is_owned "$active_pid" "$active_server" "$active_token"; then
            die "server PID $active_pid exited before becoming ready; see $run_dir"
        fi
        if grep -Fq 'Ready to accept connections' "$run_dir/server.stdout.log"; then
            if "$cli" --raw -h 127.0.0.1 -p "$port" PING \
                    > "$run_dir/readiness.stdout.txt" \
                    2> "$run_dir/readiness.stderr.txt"; then
                pong="$(tr -d '\r\n' < "$run_dir/readiness.stdout.txt")"
                if [[ "$pong" == PONG ]]; then
                    return 0
                fi
            fi
        fi
        sleep 0.1
    done
    die "server PID $active_pid did not become ready within 120 seconds; see $run_dir"
}

parse_benchmark_csv() {
    local source=$1
    local destination=$2
    local phase=$3
    local round=$4
    local sequence=$5
    local order=$6
    local variant=$7
    local matrix_id=$8
    local expected_first=$9
    local expected_second=${10}
    local raw_relative=${11}

    awk -F, \
        -v phase="$phase" \
        -v round="$round" \
        -v sequence="$sequence" \
        -v order="$order" \
        -v variant="$variant" \
        -v matrix_id="$matrix_id" \
        -v expected_first="$expected_first" \
        -v expected_second="$expected_second" \
        -v raw_relative="$raw_relative" '
        function fail(message) {
            print "invalid benchmark CSV " FILENAME ": " message > "/dev/stderr"
            failed = 1
            exit 1
        }
        function unquote(value, column) {
            sub(/\r$/, "", value)
            if (value !~ /^"[^"]*"$/) fail("column " column " is not quoted")
            return substr(value, 2, length(value) - 2)
        }
        function numeric(value) {
            return value ~ /^[0-9]+([.][0-9]+)?$/
        }
        NR == 1 {
            if (NF != 8) fail("header has " NF " columns, expected 8")
            expected[1] = "test"
            expected[2] = "rps"
            expected[3] = "avg_latency_ms"
            expected[4] = "min_latency_ms"
            expected[5] = "p50_latency_ms"
            expected[6] = "p95_latency_ms"
            expected[7] = "p99_latency_ms"
            expected[8] = "max_latency_ms"
            for (column = 1; column <= 8; column++) {
                if (unquote($column, column) != expected[column])
                    fail("unexpected header column " column)
            }
            next
        }
        /^[[:space:]]*$/ { next }
        {
            if (NF != 8) fail("data row has " NF " columns, expected 8")
            rows++
            title = unquote($1, 1)
            expected_title = rows == 1 ? expected_first : expected_second
            if (rows > 2 || title != expected_title)
                fail("unexpected test row " rows ": " title)
            for (column = 2; column <= 8; column++) {
                values[column] = unquote($column, column)
                if (!numeric(values[column]))
                    fail("non-numeric value in column " column ": " values[column])
            }
            if (values[2] + 0 <= 0) fail("rps must be greater than zero")
            printf "%s\t%d\t%d\t%d\t%s\t%s\t%s", \
                phase, round, sequence, order, variant, matrix_id, title
            for (column = 2; column <= 8; column++) printf "\t%s", values[column]
            printf "\t%s\n", raw_relative
        }
        END {
            if (!failed && rows != 2) {
                print "invalid benchmark CSV " FILENAME \
                    ": expected 2 data rows, found " rows > "/dev/stderr"
                exit 1
            }
        }
    ' "$source" > "$destination"
}

matrix_ids=(
    ping-c50-p1
    set-get-c50-p1-r100k-d512
)

run_matrix_case() {
    local phase=$1
    local round=$2
    local sequence=$3
    local order=$4
    local variant=$5
    local matrix_id=$6
    local run_dir=$7
    local case_index=$8
    local expected_first
    local expected_second
    local raw_relative
    local stdout
    local stderr
    local parsed
    local target
    local reset_log
    local -a args
    local -a command
    local parsed_row
    local request_count=50000

    if [[ "$phase" == warmup ]]; then
        request_count=10000
    fi

    case "$matrix_id" in
        ping-c50-p1)
            args=(-c 50 -P 1 -n "$request_count" -t ping)
            expected_first=PING_INLINE
            expected_second=PING_MBULK
            ;;
        set-get-c50-p1-r100k-d512)
            args=(-c 50 -P 1 -n "$request_count" -r 100000 -d 512 -t set,get)
            expected_first=SET
            expected_second=GET
            ;;
        *)
            die "unknown benchmark matrix entry: $matrix_id"
            ;;
    esac

    printf -v case_index '%02d' "$case_index"
    stdout="$run_dir/benchmarks/$case_index-$matrix_id.stdout.csv"
    stderr="$run_dir/benchmarks/$case_index-$matrix_id.stderr.log"
    parsed="$run_dir/benchmarks/$case_index-$matrix_id.parsed.tsv"
    reset_log="$run_dir/benchmarks/$case_index-$matrix_id.reset.log"
    raw_relative=${stdout#"$output_dir/"}
    command=("$benchmark" -h 127.0.0.1 -p "$port" -k 1 --csv "${args[@]}")

    record_command "$run_dir/benchmarks/$case_index-$matrix_id.flush.command.txt" \
        "$cli" --raw -h 127.0.0.1 -p "$port" FLUSHALL SYNC
    if ! "$cli" --raw -h 127.0.0.1 -p "$port" FLUSHALL SYNC \
            > "$reset_log" 2>&1; then
        die "FLUSHALL SYNC failed before $variant $matrix_id; see $reset_log"
    fi
    record_command "$run_dir/benchmarks/$case_index-$matrix_id.resetstat.command.txt" \
        "$cli" --raw -h 127.0.0.1 -p "$port" CONFIG RESETSTAT
    if ! "$cli" --raw -h 127.0.0.1 -p "$port" CONFIG RESETSTAT \
            >> "$reset_log" 2>&1; then
        die "CONFIG RESETSTAT failed before $variant $matrix_id; see $reset_log"
    fi

    record_command "$run_dir/benchmarks/$case_index-$matrix_id.command.txt" "${command[@]}"
    if ! "${command[@]}" > "$stdout" 2> "$stderr"; then
        die "benchmark failed for $variant $matrix_id; see $stderr"
    fi
    parse_benchmark_csv "$stdout" "$parsed" "$phase" "$round" "$sequence" \
        "$order" "$variant" "$matrix_id" "$expected_first" "$expected_second" \
        "$raw_relative"

    if [[ "$phase" == warmup ]]; then
        target=$warmup_results
    else
        target=$measurements
    fi
    while IFS= read -r parsed_row; do
        printf '%s\n' "$parsed_row" >> "$target"
    done < "$parsed"
}

run_sequence=0
run_variant() {
    local phase=$1
    local round=$2
    local order=$3
    local variant=$4
    local server
    local run_name
    local run_dir
    local matrix_index=0
    local matrix_id

    run_sequence=$((run_sequence + 1))
    case "$variant" in
        baseline) server=$baseline_server ;;
        candidate) server=$candidate_server ;;
        *) die "unknown server variant: $variant" ;;
    esac

    printf -v run_name '%03d-%s-r%02d-o%d-%s' \
        "$run_sequence" "$phase" "$round" "$order" "$variant"
    run_dir="$output_dir/runs/$run_name"
    mkdir -p "$run_dir/benchmarks"
    printf '%s\n' "$variant" > "$run_dir/variant.txt"
    printf '%s\n' "$phase" > "$run_dir/phase.txt"
    printf '%s\n' "$round" > "$run_dir/round.txt"
    printf '%s\n' "$order" > "$run_dir/order-in-round.txt"
    printf '%s\n' "$server" > "$run_dir/server-image.txt"

    echo "==> $phase round=$round order=$order variant=$variant"
    start_server "$server" "$run_dir"
    if ! "$cli" --raw -h 127.0.0.1 -p "$port" INFO server \
            > "$run_dir/info-server.stdout.txt" \
            2> "$run_dir/info-server.stderr.txt"; then
        die "INFO server failed for $variant; see $run_dir"
    fi

    for matrix_id in "${matrix_ids[@]}"; do
        matrix_index=$((matrix_index + 1))
        echo "    $matrix_id"
        run_matrix_case "$phase" "$round" "$run_sequence" "$order" \
            "$variant" "$matrix_id" "$run_dir" "$matrix_index"
    done

    if ! stop_active_server; then
        die "failed to stop exact Redis PID after $variant run; see $run_dir/cleanup.log"
    fi
}

run_variant warmup 0 1 baseline
run_variant warmup 0 2 candidate
for round in 1 2 3; do
    if (( round % 2 == 1 )); then
        run_variant measured "$round" 1 baseline
        run_variant measured "$round" 2 candidate
    else
        run_variant measured "$round" 1 candidate
        run_variant measured "$round" 2 baseline
    fi
done

summary_body="$output_dir/.summary-body.tsv"
awk -F '\t' '
    function append(values, value) {
        return values == "" ? value : values "," value
    }
    function median(values, local_values, count, i, j, swap) {
        count = split(values, local_values, ",")
        for (i = 1; i <= count; i++) {
            for (j = i + 1; j <= count; j++) {
                if (local_values[j] + 0 < local_values[i] + 0) {
                    swap = local_values[i]
                    local_values[i] = local_values[j]
                    local_values[j] = swap
                }
            }
        }
        return local_values[int((count + 1) / 2)] + 0
    }
    function coefficient_of_variation(values, local_values, count, i, mean, sum, squares) {
        count = split(values, local_values, ",")
        if (count < 2) return 0
        for (i = 1; i <= count; i++) sum += local_values[i] + 0
        mean = sum / count
        if (mean == 0) return 0
        for (i = 1; i <= count; i++)
            squares += ((local_values[i] + 0) - mean) ^ 2
        return sqrt(squares / (count - 1)) / mean
    }
    NR == 1 { next }
    {
        key = $6 SUBSEP $7
        seen[key] = 1
        if ($5 == "baseline") {
            baseline_count[key]++
            baseline_rps[key] = append(baseline_rps[key], $8)
            baseline_p95[key] = append(baseline_p95[key], $12)
            baseline_p99[key] = append(baseline_p99[key], $13)
        } else if ($5 == "candidate") {
            candidate_count[key]++
            candidate_rps[key] = append(candidate_rps[key], $8)
            candidate_p95[key] = append(candidate_p95[key], $12)
            candidate_p99[key] = append(candidate_p99[key], $13)
        } else {
            print "unexpected variant in measurements: " $5 > "/dev/stderr"
            invalid = 1
        }
    }
    END {
        for (key in seen) {
            if (baseline_count[key] != 3 || candidate_count[key] != 3) {
                print "expected three samples per variant for " key > "/dev/stderr"
                invalid = 1
                continue
            }
            split(key, key_parts, SUBSEP)
            baseline_rps_median = median(baseline_rps[key])
            candidate_rps_median = median(candidate_rps[key])
            baseline_p95_median = median(baseline_p95[key])
            candidate_p95_median = median(candidate_p95[key])
            baseline_p99_median = median(baseline_p99[key])
            candidate_p99_median = median(candidate_p99[key])
            rps_ratio = candidate_rps_median / baseline_rps_median
            baseline_cv = coefficient_of_variation(baseline_rps[key])
            candidate_cv = coefficient_of_variation(candidate_rps[key])
            p95_ratio = baseline_p95_median == 0 ? 0 : candidate_p95_median / baseline_p95_median
            p99_ratio = baseline_p99_median == 0 ? 0 : candidate_p99_median / baseline_p99_median
            p95_allowed = baseline_p95_median * 1.15
            p99_allowed_ratio = baseline_p99_median * 1.50
            p99_allowed_delta = baseline_p99_median + 0.50
            p99_allowed = p99_allowed_ratio > p99_allowed_delta ? p99_allowed_ratio : p99_allowed_delta
            printf "%s\t%s\t3\t3\t%.2f\t%.2f\t%.6f\t%s\t%.6f\t%.6f\t%s", \
                key_parts[1], key_parts[2], baseline_rps_median, candidate_rps_median, \
                rps_ratio, (rps_ratio >= 0.85 ? "pass" : "warn"), \
                baseline_cv, candidate_cv, \
                (baseline_cv <= 0.05 && candidate_cv <= 0.05 ? "pass" : "repeat")
            printf "\t%.3f\t%.3f\t%.6f\t%+.3f\t%.3f\t%s", \
                baseline_p95_median, candidate_p95_median, p95_ratio, \
                candidate_p95_median - baseline_p95_median, p95_allowed, \
                (candidate_p95_median <= p95_allowed ? "pass" : "warn")
            printf "\t%.3f\t%.3f\t%.6f\t%+.3f\t%.3f\t%s\n", \
                baseline_p99_median, candidate_p99_median, p99_ratio, \
                candidate_p99_median - baseline_p99_median, p99_allowed, \
                (candidate_p99_median <= p99_allowed ? "pass" : "fail")
        }
        if (invalid) exit 1
    }
' "$measurements" | LC_ALL=C sort -t $'\t' -k1,1 -k2,2 > "$summary_body"

printf '%s\n' $'matrix_id\ttest\tbaseline_samples\tcandidate_samples\tbaseline_median_rps\tcandidate_median_rps\trps_ratio\trps_warning_below_0.85\tbaseline_rps_cv\tcandidate_rps_cv\tvariation_note\tbaseline_median_p95_ms\tcandidate_median_p95_ms\tp95_ratio\tp95_delta_ms\tp95_allowed_ms\tp95_warning_above_1.15\tbaseline_median_p99_ms\tcandidate_median_p99_ms\tp99_ratio\tp99_delta_ms\tp99_allowed_ms\tp99_note' > "$summary"
while IFS= read -r summary_row; do
    printf '%s\n' "$summary_row" >> "$summary"
done < "$summary_body"
rm -f "$summary_body"

awk -F '\t' '
    BEGIN {
        print "metric\tvalue\tcriterion\tpass"
        individual_rps = 1
        p95 = 1
        p99 = 1
        variation = 1
    }
    NR == 1 { next }
    {
        ratios++
        log_sum += log($7)
        if ($8 != "pass") individual_rps = 0
        if ($11 != "pass") variation = 0
        if ($17 != "pass") p95 = 0
        if ($23 != "pass") p99 = 0
    }
    END {
        if (ratios == 0) {
            print "no summary rows" > "/dev/stderr"
            exit 1
        }
        geometric = exp(log_sum / ratios)
        geometric_pass = geometric >= 0.85
        overall = individual_rps && p95
        printf "geometric_mean_rps_ratio\t%.6f\t>=0.85 advisory\t%s\n", geometric, geometric_pass ? "pass" : "warn"
        printf "all_individual_rps\t%d\t>=0.85 each advisory\t%s\n", individual_rps, individual_rps ? "pass" : "warn"
        printf "all_p95_latency\t%d\t<=1.15x advisory\t%s\n", p95, p95 ? "pass" : "warn"
        printf "all_p99_latency\t%d\trecorded, not gated\t%s\n", p99, "info"
        printf "rps_variation\t%d\tsample CV <=0.05\t%s\n", variation, variation ? "pass" : "repeat"
        printf "regression_advisory\t%d\tRPS and p95 within 15%%\t%s\n", overall, overall ? "pass" : "warn"
    }
' "$summary" > "$assessment"

metadata_row completed_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
metadata_row status complete

echo "==> measurements: $measurements"
echo "==> summary:      $summary"
echo "==> assessment:   $assessment"
