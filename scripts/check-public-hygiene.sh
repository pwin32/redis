#!/usr/bin/env bash
# Check tracked Windows-port content for machine-specific or secret material.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

scan_history=0
case "${1:-}" in
    "") ;;
    --history) scan_history=1 ;;
    -h|--help)
        echo "usage: $0 [--history]"
        exit 0
        ;;
    *)
        echo "error: unknown option: $1" >&2
        exit 2
        ;;
esac

failures=0

report_matches() {
    local label=$1
    local pattern=$2
    shift 2
    local matches

    matches="$(git grep -n -I -E -- "$pattern" -- "$@" 2>/dev/null || true)"
    if [[ -n "$matches" ]]; then
        echo "error: $label" >&2
        printf '%s\n' "$matches" >&2
        failures=$((failures + 1))
    fi
}

if git ls-files --error-unmatch .local >/dev/null 2>&1 || [[ -n "$(git ls-files '.local/**')" ]]; then
    echo "error: .local content is tracked" >&2
    failures=$((failures + 1))
fi

port_paths=(
    AGENTS.md
    README.md
    WINDOWS-MINGW-README.md
    WINDOWS-PORTING-HISTORY.md
    'WINDOWS-*-CHANGES.md'
    WINDOWS-8.10-SOURCE-AUDIT.md
    build-mingw.sh
    package-mingw.sh
    runtest-mingw.sh
    runtest-moduleapi
    scripts
    tests/windows
)

report_matches \
    "hard-coded MSYS2 path" \
    '(MSYS_BASH|msys_bash)[^#\n]*(/mnt/[[:alpha:]]/|[[:alpha:]]:[\\/])' \
    "${port_paths[@]}"

report_matches \
    "concrete WSL path in public-facing documentation" \
    '/mnt/[[:alpha:]]/[[:alnum:]_.@%+,:;=-]+' \
    AGENTS.md README.md WINDOWS-MINGW-README.md WINDOWS-PORTING-HISTORY.md \
    'WINDOWS-*-CHANGES.md' WINDOWS-8.10-SOURCE-AUDIT.md

report_matches \
    "user-home path in public-facing content" \
    '/(home|Users)/[[:alnum:]_.-]+/' \
    "${port_paths[@]}"

report_matches \
    "internal hosting or Git endpoint in Windows-port guidance" \
    '(private forge|git@[[:alnum:]_.-]+:|ssh://[[:alnum:]_.@-]+)' \
    AGENTS.md README.md WINDOWS-MINGW-README.md WINDOWS-PORTING-HISTORY.md \
    'WINDOWS-*-CHANGES.md' WINDOWS-8.10-SOURCE-AUDIT.md \
    build-mingw.sh package-mingw.sh runtest-mingw.sh runtest-moduleapi \
    tests/windows

report_matches \
    "operational PID in public-facing documentation" \
    '\bPID[[:space:]]+[[:digit:]]+' \
    AGENTS.md WINDOWS-MINGW-README.md WINDOWS-PORTING-HISTORY.md \
    'WINDOWS-*-CHANGES.md' \
    WINDOWS-8.10-SOURCE-AUDIT.md

report_matches \
    "private-key or service-token signature" \
    '-----BEGIN ([A-Z0-9 ]+ )?PRIVATE KEY-----|AKIA[0-9A-Z]{16}|ASIA[0-9A-Z]{16}|github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|glpat-[A-Za-z0-9_-]{20,}|xox[baprs]-[A-Za-z0-9-]{10,}' \
    .

if [[ -n "${PUBLIC_PRIVATE_VALUES_FILE:-}" ]]; then
    if [[ ! -f "$PUBLIC_PRIVATE_VALUES_FILE" ]]; then
        echo "error: PUBLIC_PRIVATE_VALUES_FILE does not exist" >&2
        failures=$((failures + 1))
    else
        while IFS= read -r private_value; do
            [[ -n "$private_value" && "$private_value" != \#* ]] || continue
            if git grep -n -I -F -- "$private_value" -- . >/dev/null 2>&1; then
                echo "error: private value remains in tracked content" >&2
                failures=$((failures + 1))
            fi
            if (( scan_history )) && git log --all --format='%an%n%ae%n%cn%n%ce%n%B' | grep -F -- "$private_value" >/dev/null 2>&1; then
                echo "error: private value remains in commit metadata or messages" >&2
                failures=$((failures + 1))
            fi
        done <"$PUBLIC_PRIVATE_VALUES_FILE"
    fi
fi

if (( scan_history )); then
    missing="$(GIT_NO_LAZY_FETCH=1 git rev-list --objects --all --missing=print | sed -n '/^?/p')"
    if [[ -n "$missing" ]]; then
        echo "error: reachable Git objects are missing" >&2
        failures=$((failures + 1))
    fi

    history_paths="$(git log --all --format='%H%x09%B' | grep -E '(/mnt/[[:alpha:]]/|(^|[^[:alnum:]])[[:alpha:]]:[\\/](Users|home|tmp)[\\/])' || true)"
    if [[ -n "$history_paths" ]]; then
        echo "error: concrete local path remains in commit messages" >&2
        printf '%s\n' "$history_paths" >&2
        failures=$((failures + 1))
    fi
fi

if (( failures != 0 )); then
    echo "public hygiene check failed with $failures finding group(s)" >&2
    exit 1
fi

echo "public hygiene check passed"
