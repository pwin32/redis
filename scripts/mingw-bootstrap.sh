#!/usr/bin/env bash
# Shared MSYS2/MinGW64 bootstrap for the Windows wrappers.
#
# This file deliberately does not contain a machine-specific installation
# path.  A caller may provide --msys-bash, MSYS_BASH, or an ignored
# .local/mingw.env assignment.  When already running inside a MINGW64 shell,
# no second shell is started.

mingw_resolve_bash() {
    local local_config
    local local_bash=""
    local requested_bash="${1:-}"
    local resolved_bash=""
    local config_line

    if [[ "${MSYSTEM:-}" == "MINGW64" && -z "$requested_bash" ]]; then
        MINGW_RESOLVED_BASH="bash"
        return 0
    fi

    local_config="$(cd "${REDIS_REPO_ROOT:-.}" && pwd)/.local/mingw.env"
    if [[ -f "$local_config" ]]; then
        config_line="$(sed -n 's/^[[:space:]]*\(export[[:space:]]\+\)\?MSYS_BASH[[:space:]]*=[[:space:]]*//p' "$local_config" | sed -n '1p')"
        case "$config_line" in
            \"*\") local_bash=${config_line#\"}; local_bash=${local_bash%\"} ;;
            \'*\') local_bash=${config_line#\'}; local_bash=${local_bash%\'} ;;
            *) local_bash=$config_line ;;
        esac
    fi

    resolved_bash=${requested_bash:-${MSYS_BASH:-$local_bash}}
    if [[ -z "$resolved_bash" ]]; then
        echo "error: MSYS2 bash is not configured" >&2
        echo "set MSYS_BASH, use --msys-bash PATH, or create .local/mingw.env" >&2
        return 1
    fi
    if [[ "$resolved_bash" != "bash" && ! -x "$resolved_bash" ]]; then
        echo "error: MSYS2 bash not found at $resolved_bash" >&2
        return 1
    fi

    MINGW_RESOLVED_BASH=$resolved_bash
}

mingw_bootstrap() {
    local wrapper_path="${BASH_SOURCE[1]}"
    local cli_bash=""
    local self_abs
    local self_msys
    local -a forwarded=()

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --msys-bash)
                if [[ $# -lt 2 || -z "$2" ]]; then
                    echo "error: --msys-bash requires a path" >&2
                    return 2
                fi
                cli_bash=$2
                shift 2
                ;;
            --)
                shift
                forwarded+=("$@")
                break
                ;;
            *)
                forwarded+=("$1")
                shift
                ;;
        esac
    done

    MINGW_BOOTSTRAP_ARGS=("${forwarded[@]}")

    if [[ "${MSYSTEM:-}" == "MINGW64" || "${REDIS_MINGW_BOOTSTRAPPED:-}" == "1" ]]; then
        return 0
    fi

    REDIS_REPO_ROOT="$(cd "$(dirname "$wrapper_path")" && pwd)"
    export REDIS_REPO_ROOT
    mingw_resolve_bash "$cli_bash" || return

    self_abs="$REDIS_REPO_ROOT/$(basename "$wrapper_path")"
    self_msys="$(printf '%s' "$self_abs" | sed 's|^/mnt/\([a-zA-Z]\)/|/\1/|')"

    exec "$MINGW_RESOLVED_BASH" -l -c \
        'export MSYSTEM=MINGW64 REDIS_MINGW_BOOTSTRAPPED=1; source /etc/profile >/dev/null 2>&1; exec bash "$1" "${@:2}"' \
        _ "$self_msys" "${forwarded[@]}"
}
