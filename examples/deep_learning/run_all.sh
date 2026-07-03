#!/usr/bin/env bash
# Run every deep-learning project and check each finishes with its "OK" line.
# Usage:  ./run_all.sh [path-to-ki]   (default: ../../build-tls/ki — a TLS build, for the downloads)
set -u
cd "$(dirname "$0")"
KI="${1:-../../build-tls/ki}"
if [ ! -x "$KI" ]; then
    echo "ki not found at '$KI' — build a TLS-enabled ki (-DKIRITO_ENABLE_TLS=ON) or pass its path"
    exit 1
fi

fail=0
for f in [0-9][0-9]_*.ki; do
    printf '%-34s ' "$f"
    out="$("$KI" --lib lib "$f" 2>&1)"
    if printf '%s\n' "$out" | grep -q '^OK '; then
        echo "PASS  ($(printf '%s\n' "$out" | tail -1))"
    else
        echo "FAIL"
        printf '%s\n' "$out" | tail -3 | sed 's/^/    /'
        fail=1
    fi
done
[ "$fail" -eq 0 ] && echo "ALL PROJECTS OK" || echo "SOME PROJECTS FAILED"
exit "$fail"
