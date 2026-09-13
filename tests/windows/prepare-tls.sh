#!/usr/bin/env bash
# Build the pinned native Tcl TLS test dependency; never stage it in Redis packages.
set -euo pipefail

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
[[ "${MSYSTEM:-}" == MINGW64 ]] || die 'run this helper in an MSYS2 MINGW64 shell'
for tool in curl sha256sum tar gcc make pkg-config pacman cmp; do
    command -v "$tool" >/dev/null 2>&1 || die "required TLS test build tool not found: $tool"
done
[[ -x /mingw64/bin/tclsh ]] || die 'install mingw-w64-x86_64-tcl first'
pkg-config --atleast-version=3.0 openssl || die 'OpenSSL 3 development files are required'

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
version=1.7.22
checksum=e84e2b7a275ec82c4aaa9d1b1f9786dbe4358c815e917539ffe7f667ff4bc3b4
dependency_dir="$repo_root/.local/test-deps/tcltls-$version"
archive="$dependency_dir/tcltls-$version.tar.gz"
source_dir="$dependency_dir/source"
prefix="$dependency_dir/install"
mkdir -p "$dependency_dir"

recipe_hash="$(sha256sum "$0")"
recipe_hash=${recipe_hash%% *}
{
    printf 'TclTLS %s %s\n' "$version" "$checksum"
    printf 'Recipe %s\n' "$recipe_hash"
    printf 'GCC %s\n' "$(gcc -dumpfullversion)"
    pacman -Q mingw-w64-x86_64-tcl mingw-w64-x86_64-openssl
} > "$dependency_dir/inputs.next"

cat > "$dependency_dir/verify.tcl" <<'TCL'
if {$tcl_platform(platform) ne "windows"} { error "native Windows Tcl is required" }
lappend auto_path [file join [lindex $argv 0] lib tcltls1.7.22]
package require -exact tls 1.7.22
puts "Native TclTLS [package present tls], [tls::version]"
TCL

if [[ -f "$dependency_dir/inputs" ]] &&
   cmp -s "$dependency_dir/inputs" "$dependency_dir/inputs.next" &&
   /mingw64/bin/tclsh "$dependency_dir/verify.tcl" "$prefix"; then
    rm -f "$dependency_dir/inputs.next"
    exit 0
fi

if [[ ! -f "$archive" ]]; then
    curl --fail --location --silent --show-error --connect-timeout 20 \
        --max-time 180 --retry 3 \
        "https://core.tcl-lang.org/tcltls/uv/tcltls-$version.tar.gz" \
        -o "$archive.download"
    printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --check --status ||
        die 'TclTLS release checksum mismatch'
    mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status ||
    die 'cached TclTLS release checksum mismatch'

printf 'Building native TclTLS %s with static OpenSSL for tests\n' "$version"
rm -rf "$source_dir" "$prefix"
mkdir -p "$source_dir"
tar -xzf "$archive" -C "$source_dir" --strip-components=1
if ! (
    cd "$source_dir"
    ./configure --with-tcl=/mingw64/lib \
        --with-openssl-pkgconfig=/mingw64/lib/pkgconfig \
        --enable-static-ssl --enable-deterministic --prefix="$prefix" &&
    make -j2 &&
    make install
) > "$dependency_dir/build-recipe.log" 2>&1; then
    tail -n 80 "$dependency_dir/build-recipe.log" >&2
    die 'native TclTLS build failed'
fi
/mingw64/bin/tclsh "$dependency_dir/verify.tcl" "$prefix"
mv "$dependency_dir/inputs.next" "$dependency_dir/inputs"
