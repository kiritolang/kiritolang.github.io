#!/usr/bin/env bash
# Kirito post-work verification routine.
#
# Run this AFTER finishing a change and BEFORE calling the work done. It rebuilds each build variant
# from scratch and runs the WHOLE CTest suite for each — SEQUENTIALLY, in a fixed order, because the
# order encodes the workflow gate (see below).
#
# Forward-compatible by design: it NEVER enumerates individual test files. Tests are auto-discovered
# by CTest (unit executables and the globbed scripts/ and errors/ directories register themselves in
# tests/CMakeLists.txt), so adding or removing a test needs no change here.
#
# Variants (each in its OWN build dir; the three the project ships — `strict` was folded into `debug`):
#   debug   — g++ -O2 with the HARDENED warning set: -Wall -Wextra -Wformat=2 -Wconversion
#             -Wpointer-arith -Wpedantic -Werror -fstack-protector-all -Wreorder -Wunused -Wshadow.
#             The strictest compile gate. (binaryDir: build-debug)
#   release — g++ -O2, the looser warnings-as-errors set (no -Wconversion/-Wshadow); the build to
#             benchmark and ship. (binaryDir: build-release)
#   asan    — AddressSanitizer + UBSan (-fno-sanitize-recover=all) with the hardened warning set; the
#             memory/UB-safety gate, and a slow one. (binaryDir: build-asan)
#   tsan    — ThreadSanitizer with the hardened warning set; data-race + lock-order-inversion gate for
#             the multiprocessing dispatcher (the only concurrent code). (binaryDir: build-tsan)
#
# THE WORKFLOW GATE (run sequentially, in THIS order):
#   1. build + test `debug`.
#   2. build + test `release`.
#   3. If BOTH debug and release are green -> COMMIT AND PUSH. This is the point at which the work
#      becomes durable; do it BEFORE the long sanitizer runs so a crash/preemption/rollback can't lose it.
#   4. build + test `asan`, then `tsan`; fix any error either surfaces, then re-run (and push the fix).
#
# This script runs the variants in that order and reports each. It does NOT git-commit for you (the
# commit message/branch is a decision for the author), but after debug+release pass it prints a clear
# READY-TO-PUSH marker, then continues into asan.
#
# DISK HYGIENE: the build dirs are large and ALL FOUR together (~1.3 GB debug + 1.3 GB release + ~12 GB
# asan + ~9 GB tsan ≈ 24 GB) can fill a small disk mid-run (a `No space left on device` build abort).
# So each variant's build dir is REMOVED as soon as that variant's tests PASS — peak on-disk footprint
# is then a single variant at a time. A variant that FAILS keeps its dir (so you can re-run
# `ctest --test-dir build-<v> --rerun-failed --output-on-failure` or attach a debugger); the logs under
# /tmp/pw_<v>.*.log are always kept regardless. Pass --keep-builds to retain every build dir.
#
# Usage:  scripts/post_work_check.sh [--no-asan] [--keep-builds]
# TLS: the debug and release presets build with -DKIRITO_ENABLE_TLS on (they find_package(OpenSSL
# REQUIRED)), so the whole variant matrix compiles AND tests the TLS-on paths — a TLS-only build error
# or a failing HTTPS/deep-TLS test surfaces here, not only in the nightly. This makes OpenSSL
# (libssl-dev) a prerequisite for the debug/release presets. The asan/tsan presets stay TLS-off (a
# TLS-linked OpenSSL trips the leak detector with its own still-reachable allocations). The full
# non-system / Windows-OpenSSL path is covered by tools/scripts/build_all.sh's mingw cross-build.
#
#   --no-asan       run debug + release only (the commit gate); skip the slow asan + tsan passes.
#   --keep-builds   do NOT delete a variant's build dir after it passes (keep all artifacts).
#
# Exit status is non-zero if ANY variant fails to build or has a failing test.

set -u
cd "$(dirname "$0")/../.."

NO_ASAN=0
KEEP_BUILDS=0
for arg in "$@"; do
    case "$arg" in
        --no-asan)     NO_ASAN=1 ;;
        --keep-builds) KEEP_BUILDS=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

JOBS="$( { nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4; } )"
[ "$JOBS" -lt 2 ] 2>/dev/null && JOBS=2

# --- pre-flight: the retired 1.11 C++ value API must not resurface ------------------------------
# The 1.11 value API removed the free helpers `val(vm,x)` / `none(vm)` / `makeList(vm,…)` and the
# `List/Dict/Set(vm).add(…)/.set(…).build()` builder shims (use `Value(vm,x)` / `Value::None(vm)` /
# `List(vm,{…})` / `.push()` / init-list ctors instead). A clean build below catches their return —
# but this static check fails in under a second with the exact offending lines, so a stale
# incremental build during development can't hide the regression until the full multi-minute run.
# Scoped to C++ sources; comment lines, backtick doc mentions, and the legit `vm.none()` are excluded.
RETIRED_API_RE='(\.build\(\))|((^|[^.A-Za-z0-9_])(val|makeList)\(\s*[A-Za-z_]*vm)|([^.]none\(\s*[A-Za-z_]*vm)'
preflight_retired_api() {
    local hits
    hits=$(grep -rnE "$RETIRED_API_RE" src tools/tests examples \
                --include=*.hpp --include=*.cpp 2>/dev/null \
           | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*)' | grep -vE 'vm\.none|`')
    if [ -n "$hits" ]; then
        echo "==================== PRE-FLIGHT: RETIRED API ===================="
        echo "Retired 1.11 C++ value API resurfaced (val/none/makeList free helpers or .add()/.build() shims):"
        echo "$hits" | head -30
        echo "Fix: Value(vm,x) / Value::None(vm) / List(vm,{...}) / .push() / init-list ctors; no .build()."
        return 1
    fi
    return 0
}
if ! preflight_retired_api; then
    echo "DO NOT PUSH: retired API present — fix before building." ; exit 1
fi

declare -A DIR=( [debug]=build-debug [release]=build-release [asan]=build-asan [tsan]=build-tsan )
FAILED=0
GREEN_GATE=1   # cleared if debug or release fails

# Build+test one variant from scratch. Returns 0 on success. asan gets a generous stack + sanitizer
# options, scoped to its own invocation (the recursion guard's frames are larger under ASan).
run_variant() {
    local name="$1" dir="${DIR[$1]}"
    echo "==================== VARIANT: $name ===================="
    rm -rf "$dir"
    if ! cmake --preset "$name" >"/tmp/pw_$name.cfg.log" 2>&1; then
        echo "[$name] CONFIG FAILED"; tail -8 "/tmp/pw_$name.cfg.log"; FAILED=1; return 1
    fi
    # ASan/TSan compiles of the big headers use ~2-2.5 GB EACH, so peak build RAM is jobs x ~2.5 GB —
    # it scales with CORE COUNT, not total RAM. On a many-core / memory-capped box (e.g. a 24-core WSL2
    # whose default cap is ~50% of host) a full -j build is OOM-killed mid-compile (a bare "Terminated";
    # dmesg shows `oom-kill ... cc1plus`). Cap the sanitizer build jobs by available memory (~3 GB/job)
    # AND by core count, so it fits everywhere. Override with PW_SANITIZER_JOBS=N.
    local bjobs="$JOBS"
    case "$name" in
        asan|tsan)
            local memgb=$(( $(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null || echo 8000000) / 1048576 ))
            local rammax=$(( memgb / 3 )); [ "$rammax" -lt 2 ] && rammax=2
            bjobs="${PW_SANITIZER_JOBS:-$(( JOBS < rammax ? JOBS : rammax ))}"
            echo "[$name] building with -j$bjobs (cores=$JOBS, RAM=${memgb} GB capped at ~3 GB/job)"
            ;;
    esac
    if ! cmake --build "$dir" -j"$bjobs" -- -k 0 >"/tmp/pw_$name.build.log" 2>&1; then
        echo "[$name] BUILD FAILED ($(grep -cE 'error:' "/tmp/pw_$name.build.log") errors):"
        grep -E 'error:' "/tmp/pw_$name.build.log" | head -20
        FAILED=1; return 1
    fi
    local pre=""
    if [ "$name" = asan ]; then
        ulimit -s 262144 2>/dev/null || true
        pre="ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
    elif [ "$name" = tsan ]; then
        # ThreadSanitizer: data races AND lock-order inversions (potential deadlocks) in the dispatcher.
        pre="TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1"
    fi
    if env $pre ctest --test-dir "$dir" -j"$JOBS" >"/tmp/pw_$name.test.log" 2>&1; then
        echo "[$name] $(grep -E 'tests passed' "/tmp/pw_$name.test.log" | tail -1)"
        # Tests passed: drop this variant's build dir so the four don't pile up and fill the disk
        # (the next variant builds from scratch anyway). Logs under /tmp/pw_$name.* are kept.
        if [ "$KEEP_BUILDS" -eq 0 ]; then rm -rf "$dir" && echo "[$name] cleaned $dir (passed)"; fi
        return 0
    fi
    # Failed: keep the build dir for investigation (re-run failed tests / attach a debugger).
    echo "[$name] TESTS FAILED:"; grep -A8 'The following tests FAILED' "/tmp/pw_$name.test.log" | tail -10
    echo "[$name] build dir kept for debugging: $dir"
    FAILED=1; return 1
}

run_variant debug   || GREEN_GATE=0
run_variant release || GREEN_GATE=0

echo "==================== COMMIT GATE ===================="
if [ "$GREEN_GATE" -eq 1 ]; then
    echo "READY TO PUSH: debug + release (both TLS-on) are GREEN — commit and push now, before asan."
else
    echo "DO NOT PUSH: debug or release failed — fix before committing."
fi

[ "$NO_ASAN" -eq 0 ] && run_variant asan
[ "$NO_ASAN" -eq 0 ] && run_variant tsan

echo "==================== SUMMARY ===================="
for v in debug release asan tsan; do
    { [ "$v" = "asan" ] || [ "$v" = "tsan" ]; } && [ "$NO_ASAN" -eq 1 ] && { echo "$v: <skipped>"; continue; }
    line=$(grep -hE 'tests passed|TESTS FAILED|BUILD FAILED|CONFIG FAILED' \
                 "/tmp/pw_$v.test.log" "/tmp/pw_$v.build.log" "/tmp/pw_$v.cfg.log" 2>/dev/null | tail -1)
    echo "$v: ${line:-<not run>}"
done
[ "$FAILED" -eq 0 ] && echo "ALL GREEN" || echo "FAILURES PRESENT"
exit "$FAILED"
