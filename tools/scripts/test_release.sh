#!/usr/bin/env bash
# Run the end-to-end .ki test suites against the RELEASE executables in dist/ — i.e. exercise the
# actual `ki` interpreter binary, not the in-tree C++ unit tests. Two suites are run per binary:
#
#   tools/tests/scripts/*.ki  -> stdout must exactly match the matching .expected (a .in file, if present,
#                          is fed on stdin).
#   tools/tests/errors/*.ki   -> the program must exit non-zero and its stderr must contain every line of
#                          the matching .experr (each line is a required substring).
#
# Linux binaries run natively; Windows .exe binaries run under Wine (sudo apt-get install -y wine64).
#
# Usage:
#   scripts/test_release.sh                 # test every binary found in dist/
#   scripts/test_release.sh ./build/ki      # test one specific interpreter
#   scripts/test_release.sh path/ki.exe wine64   # test one binary through a runner (e.g. wine)

set -u
cd "$(dirname "$0")/../.."

# run_suite <label> <runner argv...>   (the .ki path is appended to the runner)
# Carriage returns are stripped from both sides before comparing: a Windows binary's stdout/stderr is
# CRLF (the CRT translates \n), while the golden fixtures are LF — the comparison is of logical
# output, so the line-ending difference is normalized away.
run_suite() {
    local label="$1"; shift
    local pass=0 fail=0 s exp in actual expected err rc ok n argf tok
    local args
    for s in tools/tests/scripts/*.ki; do
        exp="${s%.ki}.expected"; [ -f "$exp" ] || continue
        in="${s%.ki}.in"
        # Optional `<name>.args` sidecar (one argv token per line) supplies the script's command-line
        # arguments, so an arglist/argmain test runs with identical argv in every runner.
        args=(); argf="${s%.ki}.args"
        if [ -f "$argf" ]; then while IFS= read -r tok || [ -n "$tok" ]; do args+=("$tok"); done < "$argf"; fi
        if [ -f "$in" ]; then actual="$("$@" "$s" "${args[@]}" < "$in" 2>/dev/null | tr -d '\r')"
        else                  actual="$("$@" "$s" "${args[@]}" < /dev/null 2>/dev/null | tr -d '\r')"; fi
        expected="$(tr -d '\r' < "$exp")"
        if [ "$actual" = "$expected" ]; then pass=$((pass + 1))
        else echo "  FAIL       $s"; fail=$((fail + 1)); fi
    done
    for s in tools/tests/errors/*.ki; do
        exp="${s%.ki}.experr"; [ -f "$exp" ] || continue
        err="$("$@" "$s" < /dev/null 2>&1 >/dev/null)"; rc=$?
        err="$(printf '%s' "$err" | tr -d '\r')"
        ok=1
        [ "$rc" -eq 0 ] && ok=0   # must FAIL
        while IFS= read -r n; do
            n="${n%$'\r'}"; [ -z "$n" ] && continue
            case "$err" in *"$n"*) ;; *) ok=0 ;; esac
        done < "$exp"
        if [ "$ok" -eq 1 ]; then pass=$((pass + 1))
        else echo "  FAIL(err)  $s"; fail=$((fail + 1)); fi
    done
    echo "[$label] $pass passed, $fail failed"
    [ "$fail" -eq 0 ]
}

# Pick a Wine runner for .exe binaries, if available.
wine_runner() { command -v wine64 || command -v wine || true; }

rc=0

# explicit binary given on the command line
if [ "$#" -ge 1 ]; then
    bin="$1"; shift
    run_suite "$(basename "$bin")" "$@" "$bin" || rc=1
    exit "$rc"
fi

# otherwise: every binary in dist/
shopt -s nullglob
found=0
for bin in dist/ki-linux-* ; do
    found=1; echo "=== $(basename "$bin") ==="
    run_suite "$(basename "$bin")" "$bin" || rc=1
done
for bin in dist/ki-windows-*.exe ; do
    found=1; echo "=== $(basename "$bin") ==="
    w="$(wine_runner)"
    if [ -n "$w" ]; then run_suite "$(basename "$bin")" "$w" "$bin" || rc=1
    else echo "  SKIP: install wine to run the .exe (sudo apt-get install -y wine64)"; fi
done
# Sanitizer builds from build_all.sh — run the same suites with the sanitizer runtime active so a
# third party gets a vetted, working interpreter. A sanitizer finding aborts the run loudly; leak
# detection is left off here (the interpreter's exit-time roots are noisy and off-topic for a smoke
# test — a third party can re-enable it via ASAN_OPTIONS).
for bin in dist/debug-asan dist/debug-tsan ; do
    [ -e "$bin" ] || continue
    found=1; echo "=== $(basename "$bin") ==="
    case "$bin" in
        *asan) ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0" \
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
               run_suite "$(basename "$bin")" "$bin" || rc=1 ;;
        *tsan) TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
               run_suite "$(basename "$bin")" "$bin" || rc=1 ;;
    esac
done
[ "$found" -eq 0 ] && { echo "no dist/ki-* binaries found — run scripts/build_all.sh first"; exit 1; }
exit "$rc"
