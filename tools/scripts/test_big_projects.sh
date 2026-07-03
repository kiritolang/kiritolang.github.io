#!/usr/bin/env bash
# Run every big_projects/ project's own test suite against a `ki` interpreter. Each project supplies
# its own test entry point in one of three conventions:
#
#   1. Golden text output — `<proj>/test_<proj>.ki` prints an output that must byte-match
#      `<proj>/test_<proj>.expected`. (cronki, feedreader, kirdown, ledger, snip)
#   2. Self-asserting Kirito test — `<proj>/test_*.ki` runs `assert`s and prints a
#      "N passed, 0 failed" / "ALL TESTS PASSED" line. Exit 0 iff every assertion held.
#      (imaging: test_imaging.ki + test_video.ki; kgrad: test_kgrad.ki + test_extra.ki;
#       selfhost: run_tests.ki — a Kirito-in-Kirito interpreter run over the whole
#       tools/tests/scripts/*.ki golden suite; genuinely slow AND lags recent language
#       features, so it's opt-in via --selfhost.)
#   3. Python harness — a `test_client.py` (functional + adversarial) and, where present, a
#      `test_concurrent.py`. Both accept `--ki PATH` and launch the server themselves.
#      (sqldb, sqldb_kwargs, webserver, webserver_kwargs)
#
# Usage:
#   tools/scripts/test_big_projects.sh                          # uses `ki` on PATH
#   tools/scripts/test_big_projects.sh --ki dist/ki-linux-x64   # or a specific binary
#   tools/scripts/test_big_projects.sh --selfhost               # also run the slow self-host pass
#
# The self-host suite is skipped by default: it re-interprets Kirito in Kirito over hundreds of
# golden scripts and lags behind current language features, so a stock run against a modern release
# is expected to be slow and occasionally red. Pass --selfhost to include it explicitly.
#
# Exit status is the number of failed suites (0 iff all passed).

set -u
cd "$(dirname "$0")/../.."

KI="ki"
SELFHOST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --ki)       KI="$2"; shift 2 ;;
        --selfhost) SELFHOST=1; shift ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
command -v "$KI" >/dev/null 2>&1 || { echo "ki not found: $KI" >&2; exit 2; }
# absolute path so the python harnesses (which cd into their own dir) find it.
KI="$(command -v "$KI")"
command -v python3 >/dev/null 2>&1 || { echo "python3 required for sqldb / webserver harnesses" >&2; exit 2; }

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
        tail -8 "$tmp/err" 2>/dev/null | sed 's/^/          /'
        fail=$((fail + 1))
    fi
}

# Byte-compare stdout to a matching .expected. CRLF is normalized so a Windows-built ki (running
# under wine) matches the LF fixture.
run_golden() {
    local ki_file="$1"; shift
    local name; name="$(realpath --relative-to="$PWD" "$ki_file")"
    local exp="${ki_file%.ki}.expected"
    if [ ! -f "$exp" ]; then
        printf "  SKIP  %s (no .expected)\n" "$name"; skip=$((skip + 1)); return
    fi
    if ! "$KI" "$@" "$ki_file" >"$tmp/out" 2>"$tmp/err"; then
        printf "  FAIL  %s (exit %d)\n" "$name" $?
        tail -8 "$tmp/err" 2>/dev/null | sed 's/^/          /'
        fail=$((fail + 1)); return
    fi
    if diff -q <(tr -d '\r' < "$tmp/out") <(tr -d '\r' < "$exp") >/dev/null; then
        printf "  PASS  %s\n" "$name"
        pass=$((pass + 1))
    else
        printf "  FAIL  %s (stdout != .expected)\n" "$name"
        diff <(tr -d '\r' < "$exp") <(tr -d '\r' < "$tmp/out") | head -12 | sed 's/^/          /'
        fail=$((fail + 1))
    fi
}

# --- golden-output projects -----------------------------------------------------------------
for proj in cronki feedreader kirdown ledger snip; do
    d="examples/big_projects/$proj"
    run_golden "$d/test_$proj.ki"
done

# --- self-asserting Kirito tests ------------------------------------------------------------
run examples/big_projects/imaging/test_imaging.ki \
    "$KI" examples/big_projects/imaging/test_imaging.ki
run examples/big_projects/imaging/test_video.ki \
    "$KI" examples/big_projects/imaging/test_video.ki

run examples/big_projects/kgrad/test_kgrad.ki \
    "$KI" --lib examples/big_projects/kgrad/lib \
    examples/big_projects/kgrad/test_kgrad.ki
run examples/big_projects/kgrad/test_extra.ki \
    "$KI" --lib examples/big_projects/kgrad/lib \
    examples/big_projects/kgrad/test_extra.ki

if [ "$SELFHOST" -eq 1 ]; then
    # selfhost/run_tests.ki drives the whole tools/tests/scripts/*.ki golden suite through a
    # Kirito-in-Kirito interpreter, so it is very slow. Cap it at an hour.
    run examples/big_projects/selfhost/run_tests.ki \
        timeout --preserve-status 3600 \
        "$KI" --lib examples/big_projects/selfhost/lib \
        examples/big_projects/selfhost/run_tests.ki
else
    printf "  SKIP  examples/big_projects/selfhost/run_tests.ki (opt-in via --selfhost)\n"
    skip=$((skip + 1))
fi

# --- Python harnesses (sqldb, webserver, and their _kwargs twins) ---------------------------
for proj in sqldb sqldb_kwargs webserver webserver_kwargs; do
    d="examples/big_projects/$proj"
    for harness in test_client.py test_concurrent.py; do
        if [ -f "$d/$harness" ]; then
            run "$d/$harness" \
                bash -c "cd '$d' && python3 '$harness' --ki '$KI'"
        fi
    done
done

echo
echo "$pass passed, $fail failed, $skip skipped"
exit "$fail"
