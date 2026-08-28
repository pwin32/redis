#!/usr/bin/env bash
# Prove that the binaries exercised from the MinGW build directory are the
# exact bytes staged in the audited release package.

set -euo pipefail
export LC_ALL=C

usage() {
    echo "usage: $0 BUILD_DIR PACKAGE_DIR REPORT_FILE"
}

die() {
    echo "error: $*" >&2
    exit 1
}

if (( $# != 3 )); then
    usage >&2
    exit 2
fi

build_dir=$1
package_dir=$2
report_file=$3

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
for variable in build_dir package_dir; do
    value=${!variable}
    if [[ "$value" != /* ]]; then
        value="$repo_root/$value"
    fi
    [[ -d "$value" ]] || die "directory not found: ${!variable}"
    printf -v "$variable" '%s' "$(cd "$value" && pwd -P)"
done

if [[ "$report_file" != /* ]]; then
    report_file="$repo_root/$report_file"
fi
mkdir -p "$(dirname "$report_file")"
[[ ! -e "$report_file" ]] || die "report file already exists: $report_file"

for tool in cmp sha256sum wc; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done

# The first field is the binary used by the source-tree qualification suites;
# the second is the corresponding file extracted from the release ZIP.
comparisons=(
    'redis-server.exe redis-server.exe'
    'redis-server.exe redis-sentinel.exe'
    'redis-check-aof.exe redis-check-aof.exe'
    'redis-check-rdb.exe redis-check-rdb.exe'
    'redis-cli.exe redis-cli.exe'
    'redis-benchmark.exe redis-benchmark.exe'
    'EventLog.dll EventLog.dll'
)

printf 'build_file\tpackage_file\tsha256\tsize\n' > "$report_file"
for comparison in "${comparisons[@]}"; do
    read -r build_name package_name <<< "$comparison"
    build_file="$build_dir/$build_name"
    package_file="$package_dir/$package_name"
    [[ -f "$build_file" ]] || die "build binary not found: $build_name"
    [[ -f "$package_file" ]] || die "package binary not found: $package_name"
    if ! cmp -s "$build_file" "$package_file"; then
        die "tested build binary differs from packaged release binary: $build_name -> $package_name"
    fi
    file_sha="$(sha256sum "$build_file" | sed 's/[[:space:]].*$//')"
    file_size="$(wc -c < "$build_file" | tr -d '[:space:]')"
    printf '%s\t%s\t%s\t%s\n' \
        "$build_name" "$package_name" "$file_sha" "$file_size" >> "$report_file"
done

printf 'PACKAGE_BINARY_IDENTITY_OK comparisons=%d report=%s\n' \
    "${#comparisons[@]}" "$(basename "$report_file")"
