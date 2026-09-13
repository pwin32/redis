#!/usr/bin/env bash
# Focused validation of an explicit BUILD_TLS=no build, including archive reuse.
set -euo pipefail
build_dir=${1:-build/mingw64}
grep -Fx 'BUILD_TLS=no' "$build_dir/.tls-build" >/dev/null
if ar t "$build_dir/lib/libhiredis.a" | grep -Fx ssl.o; then
    echo 'stale hiredis SSL object in a no-TLS build' >&2
    exit 1
fi
for option in '--tls-port 6399' '--tls-replication yes' '--tls-cluster yes'; do
    read -r -a args <<< "$option"
    if output="$("$build_dir/redis-server.exe" --port 0 --save '' "${args[@]}" 2>&1)"; then
        echo 'no-TLS build accepted TLS configuration' >&2
        exit 1
    fi
    [[ "$output" == *'rebuild with BUILD_TLS=yes'* ]]
done
if "$build_dir/redis-cli.exe" --help | grep -q -- '--tls'; then
    echo 'no-TLS CLI unexpectedly advertises TLS' >&2
    exit 1
fi
printf 'TLS_DISABLED_OK\n'
