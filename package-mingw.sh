#!/usr/bin/env bash
# Build a Redis Windows core MinGW64 package in an explicitly authorized CI run.

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/scripts/mingw-bootstrap.sh"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

repo_root="$(cd "$(dirname "$0")" && pwd)"
cd "$repo_root"
export REDIS_REPO_ROOT="$repo_root"
mingw_resolve_bash
msys_bash="$MINGW_RESOLVED_BASH"

for tool in git install sed sha256sum strip touch zip; do
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
    packaging/licenses/ZSTD-LICENSE.txt
    RELEASENOTES.txt
    00-RELEASENOTES
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
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: invalid Redis release version in src/version.h: $version" >&2
    exit 1
fi
release_line="${version%.*}"
windows_changes="WINDOWS-${release_line}-CHANGES.md"
if [[ ! -f "$windows_changes" ]]; then
    echo "error: release-line guide not found: $windows_changes" >&2
    exit 1
fi

windows_revision="${WINDOWS_PACKAGE_REVISION:-1}"
if [[ ! "$windows_revision" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: WINDOWS_PACKAGE_REVISION must be a positive integer" >&2
    exit 1
fi

gcc_version="$(
    "$msys_bash" -l -c \
        'export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1; gcc -dumpfullversion'
)"
gcc_version="${gcc_version//$'\r'/}"
if [[ ! "$gcc_version" =~ ^[0-9]+([.][0-9]+)+$ ]]; then
    echo "error: unable to resolve the MinGW64 GCC version: $gcc_version" >&2
    exit 1
fi

gcc_package="$(
    "$msys_bash" -l -c \
        'export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1; pacman -Q mingw-w64-x86_64-gcc'
)"
gcc_package="${gcc_package//$'\r'/}"
if [[ "$gcc_package" != mingw-w64-x86_64-gcc\ * ]]; then
    echo "error: unable to resolve the MinGW64 GCC package: $gcc_package" >&2
    exit 1
fi

zstd_package="$(
    "$msys_bash" -l -c \
        'export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1; pacman -Q mingw-w64-x86_64-zstd'
)"
zstd_package="${zstd_package//$'\r'/}"
if [[ "$zstd_package" != mingw-w64-x86_64-zstd\ * ]]; then
    echo "error: unable to resolve the MinGW64 Zstandard package: $zstd_package" >&2
    exit 1
fi

if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    echo "error: package source worktree must be clean" >&2
    git status --short >&2
    exit 1
fi

source_commit="$(git rev-parse HEAD)"
source_tree="$(git rev-parse HEAD^{tree})"
source_epoch="$(git show -s --format=%ct HEAD)"
release_tag="v${version}-windows.${windows_revision}"
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
# A release archive must never inherit objects or nested jemalloc dependency
# files from another maintenance line or an earlier source checkout.
BUILD_BUNDLED_MODULES=no SKIP_VEC_SETS=yes BUILD_COMPRESSION=yes \
    ./build-mingw.sh distclean
BUILD_BUNDLED_MODULES=no SKIP_VEC_SETS=yes BUILD_COMPRESSION=yes \
    ./build-mingw.sh -j"$jobs"

if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    echo "error: release build modified the tracked source worktree" >&2
    git status --short >&2
    exit 1
fi

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
install -m 0644 packaging/licenses/ZSTD-LICENSE.txt "$stage_dir/ZSTD-LICENSE.txt"
install -m 0644 RELEASENOTES.txt "$stage_dir/RELEASENOTES.txt"
install -m 0644 00-RELEASENOTES "$stage_dir/00-RELEASENOTES"
install -m 0644 "$windows_changes" "$stage_dir/$windows_changes"

{
    printf 'Redis version: %s\n' "$version"
    printf 'Windows package revision: %s\n' "$windows_revision"
    printf 'Package scope: Redis core only; bundled Redis modules excluded\n'
    printf 'Source commit: %s\n' "$source_commit"
    printf 'Source tree: %s\n' "$source_tree"
    printf 'Release tag: %s\n' "$release_tag"
    printf 'Toolchain: GCC %s MSYS2/MinGW64\n' "$gcc_version"
    printf 'Toolchain package: %s\n' "$gcc_package"
    printf 'Allocator: jemalloc-5.3.0-redis\n'
    printf 'Compression package: %s (statically linked)\n' "$zstd_package"
    printf 'Replication compression: compiled in; keep disabled because Windows client I/O is restricted to one thread\n'
} >"$stage_dir/BUILDINFO.txt"

strip \
    "$stage_dir/redis-benchmark.exe" \
    "$stage_dir/redis-cli.exe" \
    "$stage_dir/redis-server.exe"

# These programs select their mode from argv[0]. Copy the already stripped
# server so all four aliases are byte-identical and packaging is deterministic.
for alias in redis-check-aof.exe redis-check-rdb.exe redis-sentinel.exe; do
    install -m 0755 "$stage_dir/redis-server.exe" "$stage_dir/$alias"
done

(
    cd "$stage_dir"
    {
        printf 'SHA256  SIZE  FILE\n'
        for package_file in *; do
            [[ "$package_file" != "PACKAGE-MANIFEST.txt" ]] || continue
            package_sha="$(sha256sum "$package_file" | sed 's/[[:space:]].*$//')"
            package_size="$(wc -c < "$package_file" | tr -d '[:space:]')"
            printf '%s  %s  %s\n' "$package_sha" "$package_size" "$package_file"
        done
    } > PACKAGE-MANIFEST.txt
)

# Normalize staged timestamps to the source commit for reproducible ZIP
# metadata when the same source and toolchain are used.
touch -d "@$source_epoch" "$stage_dir"/*

manifest="$release_dir/$package_name.manifest.txt"
rm -f "$archive" "$checksum" "$manifest"
zip -X -9 -j -q "$archive" "$stage_dir"/*
install -m 0644 "$stage_dir/PACKAGE-MANIFEST.txt" "$manifest"
(
    cd "$release_dir"
    sha256sum "$package_name.zip" > "$package_name.zip.sha256"
)

echo "==> package:  $archive"
echo "==> checksum: $checksum"
echo "==> manifest: $manifest"
