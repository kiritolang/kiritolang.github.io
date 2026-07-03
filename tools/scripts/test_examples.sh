#!/usr/bin/env bash
# Run every runnable program in examples/ against a `ki` interpreter. This is a smoke test — each
# example is expected to exit 0; there are no per-example golden outputs at this level (the
# `tools/tests/scripts/*.ki` suite does that job). Network-only demos are skipped.
#
# Usage:
#   tools/scripts/test_examples.sh                 # uses `ki` from PATH
#   tools/scripts/test_examples.sh --ki PATH       # or a specific binary (e.g. dist/ki-linux-x64)
#
# Exit status is the number of failed examples (0 iff all passed).

set -u
cd "$(dirname "$0")/../.."

KI="ki"
while [ $# -gt 0 ]; do
    case "$1" in
        --ki) KI="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

command -v "$KI" >/dev/null 2>&1 || { echo "ki not found: $KI" >&2; exit 2; }

pass=0; fail=0; skip=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

run() {
    local name="$1"; shift
    if "$@" >"$tmp/out" 2>"$tmp/err"; then
        printf "  PASS  %s\n" "$name"
        pass=$((pass + 1))
    else
        local rc=$?
        printf "  FAIL  %s (exit %d)\n" "$name" "$rc"
        tail -6 "$tmp/err" 2>/dev/null | sed 's/^/          /'
        fail=$((fail + 1))
    fi
}

skip_note() {
    printf "  SKIP  %s (%s)\n" "$1" "$2"
    skip=$((skip + 1))
}

# --- standalone examples ---------------------------------------------------------------------
run examples/complex_linsolve.ki    "$KI" --lib examples/lib examples/complex_linsolve.ki
run examples/domain_suffix_bench.ki "$KI" examples/domain_suffix_bench.ki
run examples/rpn_calculator.ki      "$KI" examples/rpn_calculator.ki
run examples/stats.ki               "$KI" examples/stats.ki
run examples/tabular_iris.ki        "$KI" examples/tabular_iris.ki
run examples/tabular_sales.ki       "$KI" examples/tabular_sales.ki
run examples/tabular_survey.ki      "$KI" examples/tabular_survey.ki
run examples/todo.ki                "$KI" examples/todo.ki
run examples/trie.ki                "$KI" examples/trie.ki
run examples/wordcount.ki           "$KI" examples/wordcount.ki

# --- two-step pair: gen_systems writes /tmp/system_*.txt, solve_systems reads them ----------
run examples/gen_systems.ki         "$KI" examples/gen_systems.ki
run examples/solve_systems.ki       "$KI" --lib examples/lib examples/solve_systems.ki

# --- deliberately skipped ------------------------------------------------------------------
skip_note examples/rule34_download.ki "network I/O (calls rule34.xxx API)"

echo
echo "$pass passed, $fail failed, $skip skipped"
exit "$fail"
