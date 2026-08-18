#!/usr/bin/env bash
# Build the Redis 7.4.10 Windows MinGW64 revision 1 package.

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/scripts/mingw-bootstrap.sh"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

repo_root="$(cd "$(dirname "$0")" && pwd)"
cd "$repo_root"
export REDIS_REPO_ROOT="$repo_root"
mingw_resolve_bash
msys_bash="$MINGW_RESOLVED_BASH"

for tool in git install sed sha256sum strip zip; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required packaging tool not found: $tool" >&2
        exit 1
    fi
done

required_manifest_files=(
    src/version.h
    packaging/mingw/README.txt
    msvs/setups/documentation/redis.windows.conf
    msvs/setups/documentation/redis.windows-service.conf
    packaging/mingw/sentinel.conf
    LICENSE.txt
    REDISCONTRIBUTIONS.txt
    WINDOWS-NOTICES.txt
    THIRD-PARTY-NOTICES.txt
    packaging/licenses/CC0-1.0.txt
    packaging/licenses/GCC-RUNTIME-LIBRARY-EXCEPTION.txt
    packaging/licenses/GCC-RUNTIME-README.txt
    packaging/licenses/GPL-3.0.txt
    packaging/licenses/MINGW-W64-RUNTIME.txt
    RELEASENOTES.txt
    00-RELEASENOTES
    WINDOWS-7.4-CHANGES.md
)
for manifest_file in "${required_manifest_files[@]}"; do
    if [[ ! -f "$manifest_file" ]]; then
        echo "error: required package manifest file not found: $manifest_file" >&2
        exit 1
    fi
done

version="$(sed -n 's/^#define REDIS_VERSION "\([^"]*\)"/\1/p' src/version.h)"
if [[ -z "$version" ]]; then
    echo "error: unable to read Redis version from src/version.h" >&2
    exit 1
fi
if [[ "$version" != "7.4.10" ]]; then
    echo "error: package-mingw.sh is restricted to Redis 7.4.10; found $version" >&2
    exit 1
fi

windows_revision="${WINDOWS_PACKAGE_REVISION:-1}"
if [[ "$windows_revision" != "1" ]]; then
    echo "error: Redis 7.4.10 packaging requires WINDOWS_PACKAGE_REVISION=1" >&2
    exit 1
fi

if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    echo "error: package source worktree must be clean" >&2
    git status --short >&2
    exit 1
fi

source_commit="$(git rev-parse HEAD)"
expected_source_commit="${WINDOWS_PACKAGE_SOURCE_COMMIT:-${GITHUB_SHA:-}}"

if [[ "${CI:-}" != "true" && "${CI:-}" != "1" ]]; then
    echo "error: public release packages must be built by CI, not locally" >&2
    exit 1
fi
if [[ "${PUBLIC_RELEASE_CI:-}" != "1" ]]; then
    echo "error: CI release packaging requires PUBLIC_RELEASE_CI=1" >&2
    exit 1
fi
if [[ ! "$expected_source_commit" =~ ^[0-9a-f]{40}$ ]]; then
    echo "error: set WINDOWS_PACKAGE_SOURCE_COMMIT to the selected 40-character CI source commit" >&2
    exit 1
fi
if [[ "$source_commit" != "$expected_source_commit" ]]; then
    echo "error: source checkout does not match WINDOWS_PACKAGE_SOURCE_COMMIT" >&2
    exit 1
fi

package_name="Redis-x64-${version}-mingw-r${windows_revision}"
build_dir="$repo_root/build/mingw64"
stage_dir="$build_dir/package/$package_name"
release_dir="$build_dir/releases"
archive="$release_dir/$package_name.zip"
checksum="$archive.sha256"

jobs=${JOBS:-2}
./build-mingw.sh -j"$jobs"

rm -rf "$stage_dir"
mkdir -p "$stage_dir" "$release_dir"

for executable in \
    redis-benchmark.exe \
    redis-cli.exe \
    redis-server.exe
do
    install -m 0755 "$build_dir/$executable" "$stage_dir/$executable"
done

install -m 0644 "$build_dir/EventLog.dll" "$stage_dir/EventLog.dll"
install -m 0644 packaging/mingw/README.txt "$stage_dir/README.txt"
install -m 0644 msvs/setups/documentation/redis.windows.conf "$stage_dir/redis.windows.conf"
install -m 0644 msvs/setups/documentation/redis.windows-service.conf "$stage_dir/redis.windows-service.conf"
install -m 0644 packaging/mingw/sentinel.conf "$stage_dir/sentinel.conf"
install -m 0644 LICENSE.txt "$stage_dir/LICENSE.txt"
install -m 0644 REDISCONTRIBUTIONS.txt "$stage_dir/REDISCONTRIBUTIONS.txt"
install -m 0644 WINDOWS-NOTICES.txt "$stage_dir/WINDOWS-NOTICES.txt"
install -m 0644 THIRD-PARTY-NOTICES.txt "$stage_dir/THIRD-PARTY-NOTICES.txt"
install -m 0644 packaging/licenses/CC0-1.0.txt "$stage_dir/CC0-1.0.txt"
install -m 0644 packaging/licenses/GCC-RUNTIME-LIBRARY-EXCEPTION.txt "$stage_dir/GCC-RUNTIME-LIBRARY-EXCEPTION.txt"
install -m 0644 packaging/licenses/GCC-RUNTIME-README.txt "$stage_dir/GCC-RUNTIME-README.txt"
install -m 0644 packaging/licenses/GPL-3.0.txt "$stage_dir/GPL-3.0.txt"
install -m 0644 packaging/licenses/MINGW-W64-RUNTIME.txt "$stage_dir/MINGW-W64-RUNTIME.txt"
install -m 0644 RELEASENOTES.txt "$stage_dir/RELEASENOTES.txt"
install -m 0644 00-RELEASENOTES "$stage_dir/00-RELEASENOTES"
install -m 0644 WINDOWS-7.4-CHANGES.md "$stage_dir/WINDOWS-7.4-CHANGES.md"

strip \
    "$stage_dir/redis-benchmark.exe" \
    "$stage_dir/redis-cli.exe" \
    "$stage_dir/redis-server.exe"

# These programs select their mode from argv[0]. Copy the already stripped
# server so all four aliases are byte-identical and packaging is deterministic.
for alias in redis-check-aof.exe redis-check-rdb.exe redis-sentinel.exe; do
    install -m 0755 "$stage_dir/redis-server.exe" "$stage_dir/$alias"
done

rm -f "$archive" "$checksum"
zip -X -9 -j -q "$archive" "$stage_dir"/*
(
    cd "$release_dir"
    sha256sum "$package_name.zip" > "$package_name.zip.sha256"
)

echo "==> package:  $archive"
echo "==> checksum: $checksum"
