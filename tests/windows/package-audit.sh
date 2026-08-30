#!/usr/bin/env bash
# Audit a freshly built Redis Windows ZIP without relying on the source tree's
# binaries. The extracted directory is retained for later package tests.

source "$(cd "$(dirname "$0")/../.." && pwd)/scripts/mingw-bootstrap.sh"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

set -euo pipefail
export LC_ALL=C

usage() {
    echo "usage: $0 ARCHIVE OUTPUT_DIR EXPECTED_VERSION WINDOWS_REVISION"
}

die() {
    echo "error: $*" >&2
    exit 1
}

if (( $# != 4 )); then
    usage >&2
    exit 2
fi

archive=$1
output_dir=$2
expected_version=$3
windows_revision=$4

[[ "$expected_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "invalid expected version"
[[ "$windows_revision" =~ ^[1-9][0-9]*$ ]] || die "invalid Windows revision"

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
if [[ "$archive" != /* ]]; then
    archive="$repo_root/$archive"
fi
[[ -f "$archive" ]] || die "archive not found: $archive"
archive="$(cd "$(dirname "$archive")" && pwd -P)/$(basename "$archive")"

if [[ "$output_dir" != /* ]]; then
    output_dir="$repo_root/$output_dir"
fi
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd -P)"
if find "$output_dir" -mindepth 1 -print -quit | grep -q .; then
    die "output directory must be empty: $output_dir"
fi

for tool in awk comm diff find git grep objdump sed sha256sum sort tr uniq unzip wc; do
    command -v "$tool" >/dev/null 2>&1 || die "required audit tool not found: $tool"
done

checksum="$archive.sha256"
[[ -f "$checksum" ]] || die "adjacent checksum not found: $checksum"
(
    cd "$(dirname "$archive")"
    sha256sum -c "$(basename "$checksum")"
) > "$output_dir/checksum-verification.txt"

unzip -t "$archive" > "$output_dir/zip-test.txt"
zip_entries="$output_dir/zip-entries.txt"
unzip -Z1 "$archive" > "$zip_entries"
while IFS= read -r entry; do
    [[ -n "$entry" ]] || die "package ZIP contains an empty entry name"
    [[ "$entry" != */* && "$entry" != *'\\'* && "$entry" != .* ]] ||
        die "package ZIP contains an unsafe or non-flat entry: $entry"
    [[ "$entry" != *$'\r'* && "$entry" != *$'\t'* ]] ||
        die "package ZIP contains a control character in an entry name"
done < "$zip_entries"
LC_ALL=C sort "$zip_entries" | uniq -d > "$output_dir/duplicate-zip-entries.txt"
if [[ -s "$output_dir/duplicate-zip-entries.txt" ]]; then
    sed -n '1,120p' "$output_dir/duplicate-zip-entries.txt" >&2
    die "package ZIP contains duplicate entry names"
fi
package_dir="$output_dir/package"
mkdir -p "$package_dir"
unzip -q "$archive" -d "$package_dir"

release_line="${expected_version%.*}"
changes_doc="WINDOWS-${release_line}-CHANGES.md"
expected_files="$output_dir/expected-files.txt"
actual_files="$output_dir/actual-files.txt"
cat > "$expected_files" <<EOF
00-RELEASENOTES
BUILDINFO.txt
CC0-1.0.txt
EventLog.dll
GCC-RUNTIME-LIBRARY-EXCEPTION.txt
GCC-RUNTIME-README.txt
GPL-3.0.txt
LICENSE.txt
MINGW-W64-RUNTIME.txt
PACKAGE-MANIFEST.txt
README.txt
REDISCONTRIBUTIONS.txt
RELEASENOTES.txt
THIRD-PARTY-NOTICES.txt
WINDOWS-NOTICES.txt
$changes_doc
redis-benchmark.exe
redis-check-aof.exe
redis-check-rdb.exe
redis-cli.exe
redis-sentinel.exe
redis-server.exe
redis.windows-service.conf
redis.windows.conf
sentinel.conf
EOF
LC_ALL=C sort -o "$expected_files" "$expected_files"
if [[ -f "$package_dir/ZSTD-LICENSE.txt" ]]; then
    printf '%s\n' ZSTD-LICENSE.txt >> "$expected_files"
    LC_ALL=C sort -o "$expected_files" "$expected_files"
fi
find "$package_dir" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' |
    LC_ALL=C sort > "$actual_files"
if ! comm -3 "$expected_files" "$actual_files" > "$output_dir/file-set-diff.txt"; then
    die "unable to compare package file set"
fi
if [[ -s "$output_dir/file-set-diff.txt" ]]; then
    sed -n '1,120p' "$output_dir/file-set-diff.txt" >&2
    die "package contains missing or unexpected files"
fi

for required_text in \
    LICENSE.txt REDISCONTRIBUTIONS.txt WINDOWS-NOTICES.txt \
    THIRD-PARTY-NOTICES.txt CC0-1.0.txt GPL-3.0.txt \
    GCC-RUNTIME-LIBRARY-EXCEPTION.txt GCC-RUNTIME-README.txt \
    MINGW-W64-RUNTIME.txt README.txt \
    RELEASENOTES.txt 00-RELEASENOTES "$changes_doc" BUILDINFO.txt
do
    [[ -s "$package_dir/$required_text" ]] || die "required package text is empty: $required_text"
done
if [[ -f "$package_dir/ZSTD-LICENSE.txt" ]]; then
    [[ -s "$package_dir/ZSTD-LICENSE.txt" ]] || die 'ZSTD-LICENSE.txt is empty'
fi

grep -Fx "Redis version: $expected_version" "$package_dir/BUILDINFO.txt" >/dev/null ||
    die "BUILDINFO Redis version mismatch"
grep -Fx "Windows package revision: $windows_revision" "$package_dir/BUILDINFO.txt" >/dev/null ||
    die "BUILDINFO Windows revision mismatch"
grep -Fx "Release tag: v${expected_version}-windows.${windows_revision}" \
    "$package_dir/BUILDINFO.txt" >/dev/null || die "BUILDINFO release tag mismatch"
grep -Fx "Package scope: Redis core only; bundled Redis modules excluded" \
    "$package_dir/BUILDINFO.txt" >/dev/null || die "BUILDINFO package scope mismatch"
grep -Eq '^Source commit: [0-9a-f]{40}$' "$package_dir/BUILDINFO.txt" ||
    die "BUILDINFO source commit is missing or invalid"
grep -Eq '^Source tree: [0-9a-f]{40}$' "$package_dir/BUILDINFO.txt" ||
    die "BUILDINFO source tree is missing or invalid"
buildinfo_commit="$(sed -n 's/^Source commit: //p' "$package_dir/BUILDINFO.txt")"
buildinfo_tree="$(sed -n 's/^Source tree: //p' "$package_dir/BUILDINFO.txt")"
[[ "$buildinfo_commit" == "$(git -C "$repo_root" rev-parse HEAD)" ]] ||
    die "BUILDINFO source commit does not match the audited checkout"
[[ "$buildinfo_tree" == "$(git -C "$repo_root" rev-parse 'HEAD^{tree}')" ]] ||
    die "BUILDINFO source tree does not match the audited checkout"
if [[ -n "${SOURCE_SHA:-}" && "$buildinfo_commit" != "$SOURCE_SHA" ]]; then
    die "BUILDINFO source commit does not match the requested qualification source"
fi

manifest="$package_dir/PACKAGE-MANIFEST.txt"
[[ "$(sed -n '1p' "$manifest")" == "SHA256  SIZE  FILE" ]] || die "invalid package manifest header"
manifest_files="$output_dir/manifest-files.txt"
expected_manifest_files="$output_dir/expected-manifest-files.txt"
: > "$manifest_files"
while read -r manifest_sha manifest_size manifest_name manifest_extra; do
    [[ -n "${manifest_name:-}" ]] || continue
    [[ -z "${manifest_extra:-}" ]] || die "manifest entry contains unexpected fields: $manifest_name"
    [[ "$manifest_sha" =~ ^[0-9a-f]{64}$ ]] || die "manifest contains an invalid SHA256: $manifest_name"
    [[ "$manifest_size" =~ ^[0-9]+$ ]] || die "manifest contains an invalid size: $manifest_name"
    [[ "$manifest_name" != */* && "$manifest_name" != *'\'* && "$manifest_name" != .* ]] ||
        die "manifest contains an unsafe file name: $manifest_name"
    [[ "$manifest_name" != "PACKAGE-MANIFEST.txt" ]] || die "manifest must not self-reference"
    [[ -f "$package_dir/$manifest_name" ]] || die "manifest entry is missing: $manifest_name"
    actual_sha="$(sha256sum "$package_dir/$manifest_name" | sed 's/[[:space:]].*$//')"
    actual_size="$(wc -c < "$package_dir/$manifest_name" | tr -d '[:space:]')"
    [[ "$actual_sha" == "$manifest_sha" ]] || die "manifest hash mismatch: $manifest_name"
    [[ "$actual_size" == "$manifest_size" ]] || die "manifest size mismatch: $manifest_name"
    printf '%s\n' "$manifest_name" >> "$manifest_files"
done < <(sed '1d' "$manifest")
LC_ALL=C sort -o "$manifest_files" "$manifest_files"
if [[ -n "$(uniq -d "$manifest_files")" ]]; then
    uniq -d "$manifest_files" >&2
    die "package manifest contains duplicate file entries"
fi
grep -Fvx 'PACKAGE-MANIFEST.txt' "$expected_files" > "$expected_manifest_files"
if ! diff -u "$expected_manifest_files" "$manifest_files" > "$output_dir/manifest-file-set-diff.txt"; then
    sed -n '1,120p' "$output_dir/manifest-file-set-diff.txt" >&2
    die "package manifest does not cover the exact package file set"
fi

server_hash="$(sha256sum "$package_dir/redis-server.exe" | sed 's/[[:space:]].*$//')"
for alias in redis-check-aof.exe redis-check-rdb.exe redis-sentinel.exe; do
    alias_hash="$(sha256sum "$package_dir/$alias" | sed 's/[[:space:]].*$//')"
    [[ "$alias_hash" == "$server_hash" ]] || die "$alias is not byte-identical to redis-server.exe"
done

imports_report="$output_dir/pe-imports.txt"
layout_report="$output_dir/pe-layout.txt"
: > "$imports_report"
: > "$layout_report"
for pe_file in "$package_dir"/*.exe "$package_dir"/EventLog.dll; do
    pe_name="$(basename "$pe_file")"
    objdump -f "$pe_file" | grep -F 'file format pei-x86-64' >/dev/null ||
        die "$pe_name is not a 64-bit PE image"
    {
        printf '== %s: private headers ==\n' "$pe_name"
        objdump -p "$pe_file"
        printf '== %s: sections ==\n' "$pe_name"
        objdump -h "$pe_file"
    } >> "$layout_report"
    if objdump -h "$pe_file" |
            grep -Eiq '[[:space:]][.](z?debug[^[:space:]]*|gnu_debuglink)[[:space:]]'; then
        die "$pe_name contains a debug section after release stripping"
    fi
    case "$pe_name" in
        *.exe)
            dll_characteristics="$({ objdump -p "$pe_file" || true; } |
                sed -n 's/^[[:space:]]*DllCharacteristics[[:space:]]*\([0-9A-Fa-f][0-9A-Fa-f]*\).*/\1/p' |
                sed -n '1p')"
            [[ "$dll_characteristics" =~ ^[0-9A-Fa-f]+$ ]] ||
                die "$pe_name has no readable PE DllCharacteristics field"
            dll_flags=$((16#$dll_characteristics))
            (( (dll_flags & 0x0100) != 0 )) ||
                die "$pe_name does not retain NX compatibility"
            (( (dll_flags & 0x0040) == 0 )) ||
                die "$pe_name unexpectedly enables dynamic-base ASLR incompatible with QFork"
            (( (dll_flags & 0x0020) == 0 )) ||
                die "$pe_name unexpectedly enables high-entropy ASLR incompatible with QFork"
            printf '%s\tDllCharacteristics=0x%08x\tNX_COMPAT=1\tDYNAMIC_BASE=0\tHIGH_ENTROPY_VA=0\n' \
                "$pe_name" "$dll_flags" >> "$layout_report"
            ;;
    esac
    {
        printf '== %s ==\n' "$pe_name"
        objdump -p "$pe_file" | sed -n '/The Import Tables/,/The Function Table/p'
    } >> "$imports_report"
done
objdump -h "$package_dir/EventLog.dll" | grep -Eq '[[:space:]][.]rsrc[[:space:]]' ||
    die "EventLog.dll does not contain a PE resource section"
if grep -Eiq 'DLL Name: (libgcc_s|libstdc\+\+|libwinpthread|libzstd|msys-|cygwin)' "$imports_report"; then
    grep -Ei 'DLL Name: (libgcc_s|libstdc\+\+|libwinpthread|libzstd|msys-|cygwin)' "$imports_report" >&2
    die "package imports a forbidden non-system runtime DLL"
fi

if find "$package_dir" -maxdepth 1 -type f -iname '*.dll' ! -iname 'EventLog.dll' -print -quit | grep -q .; then
    die "package contains an unexpected DLL"
fi

versions_report="$output_dir/binary-versions.txt"
: > "$versions_report"
for program in redis-server.exe redis-sentinel.exe redis-cli.exe redis-benchmark.exe; do
    printf '== %s ==\n' "$program" >> "$versions_report"
    "$package_dir/$program" --version >> "$versions_report" 2>&1 ||
        die "$program --version failed"
done
grep -F "v=$expected_version" "$versions_report" >/dev/null ||
    die "packaged binary version output does not contain $expected_version"

printf 'PACKAGE_AUDIT_OK version=%s revision=%s archive=%s extracted=%s\n' \
    "$expected_version" "$windows_revision" "$(basename "$archive")" "$package_dir" |
    tee "$output_dir/audit-summary.txt"
