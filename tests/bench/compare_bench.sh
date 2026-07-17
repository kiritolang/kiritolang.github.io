#!/usr/bin/env bash
# Bash baseline for the cross-language benchmark. Same workloads, same 31-bit LCG, same protocol as
# compare_bench.{cpp,py,ki,lua}:   compare_bench.sh <workload> <N> <reps>  ->  prints "<mean_ns> <stddev_ns>".
#
# Bash is a *very* slow interpreter, so instead of always running the full `reps` (which for the
# builtin-heavy workloads would take many minutes) it uses ADAPTIVE reps: it repeats until ~0.5 s of
# measured time has elapsed (min 5 reps), capped at the requested `reps`. The reported metric is the
# per-repetition mean ± population stddev, which is directly comparable to the other languages'
# fixed-reps numbers. Timing uses the EPOCHREALTIME builtin (bash 5.0+, microsecond resolution) so no
# process is spawned per sample.
LC_ALL=C            # force '.' as the EPOCHREALTIME decimal separator regardless of locale
export LC_ALL

wl=$1; N=$2; reps=$3
: "${reps:=1}"

_ret=0
lcg=0
lcg_next() { lcg=$(( (lcg * 1103515245 + 12345) & 2147483647 )); _ret=$lcg; }

# --- workloads -------------------------------------------------------------------------------------
run_sum_loop() {
    local s=0 i
    for (( i = 0; i < N; i++ )); do s=$(( s + i * 2 - i )); done
    _ret=$s
}

fib() {  # global-var return (no subshell) so ~fib(17) stays function-call-bound, not process-bound
    local n=$1
    if (( n < 2 )); then _ret=$n; return; fi
    fib $(( n - 1 )); local a=$_ret
    fib $(( n - 2 )); _ret=$(( a + _ret ))
}
run_fib() { fib "$N"; }

run_sieve() {
    local -a sv; local i count=0 p m
    for (( i = 0; i <= N; i++ )); do sv[i]=1; done
    p=2
    while (( p <= N )); do
        if (( sv[p] )); then
            (( count++ ))
            m=$(( p * p ))
            while (( m <= N )); do sv[m]=0; m=$(( m + p )); done
        fi
        (( p++ ))
    done
    _ret=$count
}

qsort() {  # in-place quicksort of the global `arr` between indices $1..$2 (median-of-position pivot)
    local lo=$1 hi=$2 i j pivot tmp
    (( lo >= hi )) && return
    i=$lo; j=$hi; pivot=${arr[(lo + hi) / 2]}
    while (( i <= j )); do
        while (( arr[i] < pivot )); do (( i++ )); done
        while (( arr[j] > pivot )); do (( j-- )); done
        if (( i <= j )); then
            tmp=${arr[i]}; arr[i]=${arr[j]}; arr[j]=$tmp; (( i++ )); (( j-- ))
        fi
    done
    (( lo < j )) && qsort "$lo" "$j"
    (( i < hi )) && qsort "$i" "$hi"
}
run_sort() {
    arr=( "${base[@]}" )
    qsort 0 $(( N - 1 ))
    _ret=$(( arr[0] + arr[N - 1] + arr[N / 2] ))
}

run_dict_ops() {
    local -A d; local k s=0
    for k in "${keys[@]}"; do d[$k]=$(( k * k )); done
    for k in "${keys[@]}"; do s=$(( s + d[$k] )); done
    _ret=$s
}

run_string_ops() {
    local -a words; local joined
    read -r -a words <<< "$text"          # split on whitespace
    local IFS=-; joined="${words[*]}"     # join with '-'
    _ret=$(( ${#joined} + ${#words[@]} ))
}

once() {
    case $wl in
        sum_loop)   run_sum_loop ;;
        fib)        run_fib ;;
        sieve)      run_sieve ;;
        sort)       run_sort ;;
        dict_ops)   run_dict_ops ;;
        string_ops) run_string_ops ;;
        *) echo "unknown workload '$wl'" >&2; exit 2 ;;
    esac
}

# --- inputs for the optimistic workloads, built once (outside the timed loop) ----------------------
declare -a base keys
text=""
if [[ $wl == sort ]]; then
    lcg=12345
    for (( i = 0; i < N; i++ )); do lcg_next; base+=( $(( _ret % 1000000 )) ); done
elif [[ $wl == dict_ops ]]; then
    lcg=777
    for (( i = 0; i < N; i++ )); do lcg_next; keys+=( "$_ret" ); done
elif [[ $wl == string_ops ]]; then
    lcg=99; parts=""
    for (( i = 0; i < N; i++ )); do lcg_next; parts+="w$(( _ret % 10000 )) "; done
    text=${parts% }
fi

# --- adaptive timed loop -------------------------------------------------------------------------
now_ns() { local t=${EPOCHREALTIME}; _ns=$(( ${t%.*} * 1000000000 + 10#${t#*.} * 1000 )); }

once   # warmup
declare -a samples
sink=0; count=0; elapsed=0
while (( count < reps )); do
    now_ns; t0=$_ns
    once
    now_ns; t1=$_ns
    samples+=( $(( t1 - t0 )) )
    sink=$(( sink ^ _ret ))
    (( count++ )); elapsed=$(( elapsed + t1 - t0 ))
    (( count >= 5 && elapsed >= 500000000 )) && break   # ~0.5 s budget, min 5 reps
done

printf '%s\n' "${samples[@]}" | awk '
    { x[NR] = $1; s += $1 }
    END {
        m = s / NR
        for (i = 1; i <= NR; i++) { d = x[i] - m; v += d * d }
        v /= NR
        printf "%.1f %.1f\n", m, sqrt(v)
    }'
echo "checksum=$sink" >&2
