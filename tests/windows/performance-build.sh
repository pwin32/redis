#!/usr/bin/env bash
# Build all comparisons with one resolved toolchain. No release packages.
set -euo pipefail

[[ "${BASELINE_SHA:-}" =~ ^[0-9a-f]{40}$ ]] || { echo 'BASELINE_SHA must be an exact commit' >&2; exit 2; }
[[ "${CANDIDATE_SHA:-}" =~ ^[0-9a-f]{40}$ ]] || { echo 'CANDIDATE_SHA must be an exact commit' >&2; exit 2; }
root=$(pwd -P)
output="$root/build/performance"
mkdir -p "$output/sources"
git config --local core.autocrlf false
git config --local core.eol lf
git config --local core.filemode false
git reset --hard HEAD
[[ -z "$(git status --porcelain --untracked-files=normal)" ]]
for sha in "$BASELINE_SHA" "$CANDIDATE_SHA"; do git cat-file -e "$sha^{commit}"; done
git worktree add --detach "$output/sources/baseline" "$BASELINE_SHA"
git worktree add --detach "$output/sources/candidate" "$CANDIDATE_SHA"
if [[ -n "${REFERENCE_SHA:-}" ]]; then
    [[ "$REFERENCE_SHA" =~ ^[0-9a-f]{40}$ ]] || { echo 'REFERENCE_SHA must be an exact commit' >&2; exit 2; }
    git cat-file -e "$REFERENCE_SHA^{commit}"
    git worktree add --detach "$output/sources/reference" "$REFERENCE_SHA"
fi

{
    gcc --version
    gprof --version
    pacman -Q mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils \
        mingw-w64-x86_64-openssl mingw-w64-x86_64-zstd mingw-w64-x86_64-python
} > "$output/toolchain.txt"
: > "$output/variants.ndjson"

build_variant() {
    local name=$1 source=$2 tls=$3 profile=$4
    local source_dir="$output/sources/$source"
    local dest="$output/binaries/$name"
    local opt='-O2 -g'
    local link='-no-pthread -static-libgcc -static-libstdc++'
    local -a targets=("$dest/redis-server.exe")
    if [[ "$profile" == true ]]; then
        opt+=' -pg -fno-omit-frame-pointer'
        link+=' -pg'
    else
        targets+=("$dest/redis-cli.exe" "$dest/redis-benchmark.exe")
    fi
    (
        cd "$source_dir"
        make -f Makefile.mingw -j2 BUILD="$dest" BUILD_TLS="$tls" \
            OPT="$opt" LDFLAGS_COMMON="$link" "${targets[@]}"
        [[ -z "$(git status --porcelain --untracked-files=no)" ]]
    ) 2>&1 | tee "$output/$name-build.log"
    jq -n --arg name "$name" --arg source "$(git -C "$source_dir" rev-parse HEAD)" \
        --arg tree "$(git -C "$source_dir" rev-parse 'HEAD^{tree}')" \
        --arg directory "$(cygpath -am "$dest")" --arg tls "$tls" \
        --arg opt "$opt" --arg link "$link" --argjson profile "$profile" \
        '{name:$name,source_commit:$source,source_tree:$tree,directory:$directory,
          build_tls:$tls,profile:$profile,opt:$opt,link:$link}' >> "$output/variants.ndjson"
}

build_variant baseline baseline no false
build_variant candidate-notls candidate no false
build_variant candidate-tls candidate yes false
if [[ -n "${REFERENCE_SHA:-}" ]]; then build_variant reference-tls reference yes false; fi
build_variant baseline-gprof baseline no true
build_variant candidate-notls-gprof candidate no true
build_variant candidate-tls-gprof candidate yes true
if [[ -n "${REFERENCE_SHA:-}" ]]; then build_variant reference-tls-gprof reference yes true; fi
jq -s --arg harness "$(git rev-parse HEAD)" \
    --arg output "$(cygpath -am "$output/results")" \
    '{harness_commit:$harness,output:$output,variants:.}' \
    "$output/variants.ndjson" > "$output/manifest.json"
