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
# MEMORY HYGIENE — why this script refuses to run beside another build:
# Every test TU includes the whole header-only interpreter, so ONE compile peaks at ~1.7 GB RSS at -O2
# and ~3.2 GB under -fsanitize=address (measured, not estimated). Peak build RAM is therefore
# jobs x that, which scales with CORE COUNT and not with how much RAM the box has — so a reflexive
# `-j$(nproc)` is an OOM waiting to happen on a many-core machine (24 cores => ~41 GB release, ~78 GB
# asan). Two builds at once doubles it (~82 GB), and on WSL2 — which boots with `panic=-1` — an
# out-of-memory kernel reboots the whole distro instantly: you lose the VM, not just the build.
# So this script: (1) refuses to start if another instance holds the lock, (2) refuses to start if a
# build is already running outside it (stray cc1plus/ninja), and (3) sizes -j from MemAvailable rather
# than from the core count. NEVER run two variants side by side to "save time" — it costs the machine.
# PW_MAX_JOBS=N caps jobs (default 16); PW_SANITIZER_JOBS=N overrides just the asan/tsan build.
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

# --- single-instance lock ------------------------------------------------------------------------
# Two of these side by side is what takes the machine down (see MEMORY HYGIENE in the header): each run
# is internally -j<jobs>, and neither run's budget can see the other to compensate. Serialize instead of
# letting two runs bid for the same RAM.
LOCK=/tmp/kirito_post_work.lock
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "REFUSING TO RUN: another post_work_check.sh holds $LOCK."
    echo "Two concurrent builds OOM this machine. Wait for that run, or kill it — never run both."
    exit 3
fi

# The lock above only sees other runs of THIS script. The crash that motivated it came from builds
# started OUTSIDE it — a bare `cmake --build -j24` in a second shell, or ninja orphaned by a run that
# was SIGKILLed — and no lock can observe those. Compilers already running means the memory budget
# below is a fiction (it can only divide RAM among jobs it knows about), so refuse rather than stack a
# second build on top of them.
if pgrep -x cc1plus >/dev/null 2>&1 || pgrep -x ninja >/dev/null 2>&1; then
    echo "REFUSING TO RUN: a build is already in progress —"
    pgrep -a -x ninja 2>/dev/null | head -3
    pgrep -a -x cc1plus 2>/dev/null | head -2
    echo "Two builds at once OOM this machine (~1.7 GB per -O2 TU, ~3.2 GB per asan TU). Wait for it"
    echo "to finish or kill it, then re-run. Never build two variants side by side."
    exit 3
fi

# --- parallelism, bounded by MEMORY and not just by cores -----------------------------------------
# See MEMORY HYGIENE in the header for the per-TU cost this budgets against. Two details that matter:
# budget from MemAvailable and not MemTotal (on WSL2 MemTotal is the ceiling the host balloons toward,
# not memory we actually have), and keep a flat MAX_JOBS ceiling on top so a 64-core box with plenty of
# RAM still can't spawn a compiler per core and page the machine to death.
CORES="$( { nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4; } )"
MAX_JOBS="${PW_MAX_JOBS:-16}"

avail_gb() { awk '/MemAvailable/{printf "%d", $2/1048576}' /proc/meminfo 2>/dev/null || echo 8; }
# jobs_for <gb-per-job>: how many such jobs fit in available RAM, clamped to cores and MAX_JOBS.
jobs_for() {
    local per="$1" j
    j=$(( $(avail_gb) / per ))
    [ "$j" -gt "$CORES" ] && j="$CORES"
    [ "$j" -gt "$MAX_JOBS" ] && j="$MAX_JOBS"
    [ "$j" -lt 2 ] && j=2
    echo "$j"
}
JOBS="$(jobs_for 2)"

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
    hits=$(grep -rnE "$RETIRED_API_RE" src tests examples \
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

# --- pre-flight: host build prerequisites -------------------------------------------------------
# Fail early with an ACTIONABLE message instead of the cryptic, truncated CMake error a missing
# toolchain produces ("CMAKE_CXX_COMPILER not set, after EnableLanguage" for a missing compiler;
# "Could NOT find OpenSSL (OPENSSL_INCLUDE_DIR)" for a missing libssl-dev). The debug/release presets
# build TLS-on, so OpenSSL's dev headers are a hard prerequisite here (asan/tsan are TLS-off).
# `ls a b` exits non-zero when ANY operand is missing, so probing both paths in one `ls` claimed
# libssl-dev was absent on a box that has it: /usr/include/openssl/ssl.h existed, the multiarch glob
# did not expand, `ls` failed on the unexpanded operand, and pkg-config (not installed by default)
# wasn't there to save the `||`. Test each candidate on its own.
have_openssl() {
    pkg-config --exists openssl 2>/dev/null && return 0
    local h
    for h in /usr/include/openssl/ssl.h /usr/include/*/openssl/ssl.h; do
        [ -f "$h" ] && return 0
    done
    return 1
}

preflight_prereqs() {
    local missing=()
    command -v cmake >/dev/null 2>&1 || missing+=("cmake")
    command -v ninja >/dev/null 2>&1 || missing+=("ninja-build (the presets use the Ninja generator)")
    { command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1; } \
        || missing+=("build-essential (no g++/clang++ on PATH → 'CMAKE_CXX_COMPILER not set')")
    have_openssl \
        || missing+=("libssl-dev (debug/release build TLS-on → find_package(OpenSSL REQUIRED))")
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "==================== PRE-FLIGHT: MISSING PREREQUISITES ===================="
        printf '  - %s\n' "${missing[@]}"
        echo "Install (Debian/Ubuntu/WSL):"
        echo "  sudo apt-get update && sudo apt-get install -y build-essential libssl-dev cmake ninja-build"
        echo "(asan/tsan are TLS-off and don't need OpenSSL; see this script's header comment.)"
        return 1
    fi
    return 0
}
if ! preflight_prereqs; then
    echo "DO NOT PUSH: host build prerequisites missing — install them and re-run." ; exit 1
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
    # Budget ~2 GB per -O0/-O2 TU and ~4 GB per sanitizer TU (measured peak RSS 1.7 / 3.2 GB, plus
    # headroom for the linker and the page cache). The cap this replaces divided MemTotal by 3 and
    # applied only to asan/tsan — it both over-committed (3.2 GB actual against a 3 GB assumption, and
    # MemTotal is not free memory) and left debug/release running flat out at -j<cores>, ~41 GB.
    # Override the sanitizer figure with PW_SANITIZER_JOBS=N.
    local bjobs
    case "$name" in
        asan|tsan) bjobs="${PW_SANITIZER_JOBS:-$(jobs_for 4)}" ;;
        *)         bjobs="$(jobs_for 2)" ;;
    esac
    echo "[$name] building with -j$bjobs (cores=$CORES, MemAvailable=$(avail_gb) GB)"
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
    # ThreadSanitizer maps its shadow at FIXED addresses, so a PIE mapping landing in that range kills
    # the process on startup: "FATAL: ThreadSanitizer: unexpected memory mapping". Modern kernels make
    # that near-certain — Ubuntu 24.04 ships vm.mmap_rnd_bits=32 — and it hits EVERY tsan binary,
    # including single-threaded ones, so the variant reports hundreds of instant "failures" that read
    # like real breakage rather than one environmental fault. Run the tsan suite with randomization off:
    # the personality is inherited by ctest's children, it needs no root (unlike lowering
    # vm.mmap_rnd_bits, which is also per-box and lost on reboot), and it costs nothing — TSan hunts
    # data races, not ASLR robustness, and every other variant still runs with ASLR on.
    local runner=()
    if [ "$name" = tsan ] && command -v setarch >/dev/null 2>&1; then
        runner=(setarch "$(uname -m)" -R)
    fi
    if env $pre "${runner[@]}" ctest --test-dir "$dir" -j"$JOBS" --output-on-failure \
            >"/tmp/pw_$name.test.log" 2>&1; then
        echo "[$name] $(grep -E 'tests passed' "/tmp/pw_$name.test.log" | tail -1)"
        # Tests passed: drop this variant's build dir so the four don't pile up and fill the disk
        # (the next variant builds from scratch anyway). Logs under /tmp/pw_$name.* are kept.
        if [ "$KEEP_BUILDS" -eq 0 ]; then rm -rf "$dir" && echo "[$name] cleaned $dir (passed)"; fi
        return 0
    fi
    # Failed: keep the build dir for investigation (re-run failed tests / attach a debugger).
    echo "[$name] TESTS FAILED:"; grep -A8 'The following tests FAILED' "/tmp/pw_$name.test.log" | tail -10
    # A systemic fault — a sanitizer runtime refusing to start, a missing shared library — fails every
    # test identically, and then the list above is hundreds of names that say nothing about the cause.
    # --output-on-failure puts each failure's real output in the log; surface the first diagnostic line.
    local why
    why=$(grep -m3 -hE 'FATAL:|ERROR: |terminate called|Assertion .* failed|error while loading' \
               "/tmp/pw_$name.test.log" 2>/dev/null)
    [ -n "$why" ] && { echo "[$name] first error reported:"; echo "$why" | sed 's/^/    /'; }
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
