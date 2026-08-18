#!/usr/bin/env bash
#
# Build the Windows Redis port with the MSYS2 MinGW64 toolchain.
#
source "$(cd "$(dirname "$0")" && pwd)/scripts/mingw-bootstrap.sh"
mingw_bootstrap "$@"
set -- "${MINGW_BOOTSTRAP_ARGS[@]}"

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
cd "$repo_root"

# Nested jemalloc makefiles include absolute source paths in dependency files.
# A branch switch can otherwise combine one maintenance line's sources with
# another line's generated headers. Discard only that generated subtree when
# its committed allocator source tree changes.
case " $* " in
    *" clean "*|*" distclean "*) ;;
    *)
        jemalloc_src="$(sed -n 's/^JEMALLOC_SRC[[:space:]]*:=[[:space:]]*//p' Makefile.mingw | sed -n '1p')"
        if [[ -n "$jemalloc_src" ]] && command -v git >/dev/null 2>&1; then
            jemalloc_source_id="$(git rev-parse "HEAD:$jemalloc_src" 2>/dev/null || true)"
            if [[ -n "$jemalloc_source_id" ]]; then
                jemalloc_build="$repo_root/build/mingw64/jemalloc"
                jemalloc_marker="$repo_root/build/mingw64/.jemalloc-source-id"
                previous_source_id=""
                if [[ -f "$jemalloc_marker" ]]; then
                    previous_source_id="$(sed -n '1p' "$jemalloc_marker")"
                fi
                current_source_id="$jemalloc_src:$jemalloc_source_id"
                if [[ -d "$jemalloc_build" ]] && [[ "$previous_source_id" != "$current_source_id" ]]; then
                    echo "==> allocator source changed; removing stale MinGW jemalloc state"
                    rm -rf "$jemalloc_build"
                fi
                mkdir -p "$(dirname "$jemalloc_marker")"
                printf '%s\n' "$current_source_id" >"$jemalloc_marker"
            fi
        fi
        ;;
esac

echo "==> msystem:   ${MSYSTEM:-unknown}"
echo "==> toolchain: $(gcc --version | sed -n '1p')"
echo "==> makefile:  Makefile.mingw"

exec make -f Makefile.mingw "$@"
