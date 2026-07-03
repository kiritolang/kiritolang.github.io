#!/usr/bin/env bash
# Bytecode / semantic stability gate. Runs every golden `.ki` in tools/tests/scripts/ through a
# chosen `ki` binary and diffs stdout against the recorded `.expected`. Any drift is a stability
# breach — either the compiler changed something (intentional, requires updating fixtures) or the
# VM changed behaviour (unintentional, is the bug).
#
# The full CTest suite does the same thing plus much more, but this script is the "quick pre-commit
# check" — no configure, no build, no CTest setup: point it at a binary and go.
#
# Usage:
#   tools/scripts/bytecode_stability.sh                     # uses `ki` on PATH
#   tools/scripts/bytecode_stability.sh --ki dist/ki-linux-x64
#
# Exit status: number of scripts whose stdout diverged (0 iff every fixture round-trips).

set -u
cd "$(dirname "$0")/../.."

KI="ki"
while [ $# -gt 0 ]; do
    case "$1" in
        --ki) KI="$2"; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
command -v "$KI" >/dev/null 2>&1 || { echo "ki not found: $KI" >&2; exit 2; }
KI="$(command -v "$KI")"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

drift=0
total=0
for ki_file in tools/tests/scripts/*.ki; do
    exp="${ki_file%.ki}.expected"
    [ -f "$exp" ] || continue
    total=$((total + 1))
    name="$(basename "${ki_file%.ki}")"
    args_file="${ki_file%.ki}.args"
    in_file="${ki_file%.ki}.in"
    args=()
    if [ -f "$args_file" ]; then
        while IFS= read -r tok || [ -n "$tok" ]; do args+=("$tok"); done < "$args_file"
    fi
    if [ -f "$in_file" ]; then
        actual="$("$KI" "$ki_file" "${args[@]}" < "$in_file" 2>/dev/null | tr -d '\r')"
    else
        actual="$("$KI" "$ki_file" "${args[@]}" < /dev/null 2>/dev/null | tr -d '\r')"
    fi
    expected="$(tr -d '\r' < "$exp")"
    if [ "$actual" != "$expected" ]; then
        drift=$((drift + 1))
        printf "  DRIFT  %s\n" "$name"
        diff <(printf '%s' "$expected") <(printf '%s' "$actual") | head -6 | sed 's/^/         /'
    fi
done
echo
echo "$total fixtures, $drift diverged"
exit "$drift"
