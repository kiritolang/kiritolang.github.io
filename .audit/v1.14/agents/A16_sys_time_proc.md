# A16 — Audit: sys + time + proc_compat (v1.14)

Scope: `src/kirito/stdlib_sys.hpp`, `src/kirito/stdlib_time.hpp`, `src/kirito/proc_compat.hpp`
Method: static read + live probes against `build-debug/ki` (binary reports version 1.12.0).
Read-only on `src/`. Prior v1.13 (A19) + v1.12 findings merged; hunted NEW angles.

## Status of prior (v1.13 A19) findings
- **A19-1 (setEpoch narrowing) — FIXED.** `gmtimeCompat` now validates the TRUE int64 year
  BEFORE narrowing (`stdlib_time.hpp:63-67`). Live: the crafted epoch `datetime(135533096448000)`
  and `datetime(INT64_MAX)` both throw cleanly now (v1.13 accepted the former with a wrapped year).
- **A19-4 (env race) — FIXED.** `envMutex()` (`stdlib_sys.hpp:35`) now guards all four env entry
  points (getenv/setenv/unsetenv/environ).
- **A19-2 (POSIX pipe fd leak) — STILL PRESENT** (`proc_compat.hpp:242-243`): the `||` chain still
  throws without closing already-opened pipes. Merged, not re-scored here.
- **A19-3 (Windows timeout UB) — STILL PRESENT** (`proc_compat.hpp:184`). Merged.
- **A19-5 (exit no fstream flush) — STILL PRESENT, re-confirmed live.** `io.open(p,"w"); f.write("important"); sys.exit(0)` leaves the file EMPTY on disk. Merged.
- **A19-9 (negative/NaN timeout → infinite wait) — STILL PRESENT.** Merged.
- **A19-10 (tz format directives) — benign on this glibc** (`%Z`→"UTC", `%z`→"+0000"); platform-dependent elsewhere. Merged.

---

## NEW findings

### A16-1: `time.sleep()` has no upper guard — inf / huge seconds hit float-cast-overflow UB in `sleep_for` (silent no-op or unbounded hang)
- **severity:** low-medium
- **location:** `stdlib_time.hpp:325-329` (`sleep`) — `if (secs > 0) std::this_thread::sleep_for(duration<double>(secs));`
- **category:** correctness / undefined behavior / DoS
- **description:** The only guard is `secs > 0`. For a large-but-finite `secs > ~9.2e18` (INT64 seconds max) or `secs == +inf`, `sleep_for(duration<double>(secs))` internally `duration_cast`s to the clock's int64-rep duration (`chrono::seconds`/`nanoseconds`), and casting an out-of-range/`inf` double to `int64_t` is **float-cast-overflow UB** — the exact class of bug the codebase deliberately guards against elsewhere (`datetime` timestamp cast `stdlib_time.hpp:340-347`, `toInt64Checked` in `stdlib_math.hpp:29`, `math.floor/ceil`). `sleep` alone is unguarded.
- **failure-scenario (live-confirmed against build-debug/ki):**
  - `time.sleep(1.0e19)` → returns IMMEDIATELY (prints "after") instead of sleeping ~317 billion years — the int64-seconds cast overflowed/wrapped. **UB.**
  - `time.sleep(math.inf)` → returns immediately (prints "after inf"). **UB**, silent no-op.
  - `time.sleep(1.0e18)` (< int64 max, so no cast overflow) → **hangs** on a genuine ~31-billion-year sleep (timed out at 3s). Unbounded hang with no guard.
  - `time.sleep(math.nan)` → no-op (NaN > 0 is false) — this one is safe by luck.
- **proposed-test:** none exists — `test_time.cpp` only covers `sleep(0.001)`, `sleep(0)`, `sleep("x")` (throws). Add: `sleep(inf)`/`sleep(1e19)` must throw a clean "sleep: duration out of range" (or clamp), and assert they neither hang nor invoke UB (asan/ubsan gate).
- **proposed-fix:** before sleeping, reject/clamp non-finite and out-of-range: e.g. `if (std::isnan(secs) || secs <= 0) return;` then `if (std::isinf(secs) || secs > (double)LARGE_CAP) throw KiritoError("sleep: duration out of range")` (or clamp to a large finite cap). Mirror the `datetime`/`toInt64Checked` float guards.
- **confidence:** high (UB is directly readable; the immediate-return vs hang boundary was confirmed live).

### A16-2 (coverage gap): `time.sleep` overflow / inf / huge is untested
- **severity:** low
- **location:** `tools/tests/unit/test_time.cpp`, `tools/tests/scripts/audit_time.ki`
- **description:** Existing tests only exercise tiny/zero and the non-number throw. No test asserts behavior for `sleep(inf)`, `sleep(NaN)`, or a huge finite duration — precisely the values that expose A16-1. Pairs with A16-1's proposed-test.
- **confidence:** high.

### A16-3: `createprocess`/`shell` with a non-existent `cwd` reports a misleading "failed to start '<prog>'" error
- **severity:** low
- **location:** `proc_compat.hpp:266-267` (child `chdir` failure writes errno to the exec-error channel) + `:284` (parent formats it as `"failed to start '" + argv[0] + "': " + strerror`)
- **category:** diagnostics clarity
- **description:** The exec-error channel is shared by the pre-exec `chdir` failure and the `execvp` failure, so a bad `cwd` is reported as if the *program* couldn't start. Live: `createprocess(["echo","x"], cwd="/no/such/dir/xyz")` throws `failed to start 'echo': No such file or directory` — the program is fine; the directory is the problem. A user debugging this is pointed at the wrong thing.
- **proposed-test:** `createprocess(["echo"], cwd="/nonexistent")` should raise an error naming the cwd.
- **proposed-fix:** write a small tag byte (or distinct context) on the chdir path so the parent emits "failed to chdir to '<cwd>': <errno>".
- **confidence:** high (behavior), low (severity — cosmetic).

### A16-4 (minor): `DateTime.format` treats a legitimately-empty `strftime` result as "buffer too small", reallocating up to ~256 MiB before returning `""`
- **severity:** low (hard to trigger on glibc)
- **location:** `stdlib_time.hpp:244-259`
- **category:** robustness / potential DoS
- **description:** `strftime` returns 0 both when the result didn't fit AND when the (valid) result is genuinely empty. The retry loop cannot tell these apart, so a format whose output is legitimately empty grows the buffer `256 → 256*4^5 ≈ 256 MiB` over 6 tries then returns `""`. On this glibc every directive tried (`%Z %z %p`) is non-empty, so not reachable here, but a locale/directive combination yielding empty output would waste large allocations. Not a correctness bug.
- **proposed-fix:** distinguish empty-but-valid from too-small (sentinel-fill the buffer), or cap the grow loop at 1–2 tries + a modest max, since datetime format output is inherently small.
- **confidence:** medium (logic), low (reachability/severity).

### A16-5 (minor): negative-year `iso()` is not `strptime`-round-trippable
- **severity:** low
- **location:** `stdlib_time.hpp:162-168` (`iso` uses `"%04d"`) vs `strptimeCompat` (`stdlib_time.hpp:98-135`, `%Y` reads only unsigned digits)
- **description:** `time.make(-5,1,1).iso()` → `"-005-01-01T00:00:00"` (live). `strptime` has no negative-year handling (`num` consumes only `[0-9]`), so this ISO string cannot be parsed back — an asymmetry between the two text↔DateTime paths. DateTime supports years down to -9999 as fields but its textual output for negative years is one-way.
- **proposed-fix:** document that negative-year ISO is display-only, or handle a leading `-` in `%Y`.
- **confidence:** high (behavior), low (severity).

---

## Verified-correct (probed, no defect)
- **DateTime fields / leap day / rollover:** `make(2024,2,29,...)` fields exact; C-mktime rollover `make(2024,1,32)`→`2024-02-01`, `make(2024,13,1)`→`2025-01-01`, `make(2024,0,15)`→`2023-12-15` (documented).
- **Equality by instant + hashing:** two same-instant DateTimes compare `==` and share a Dict key (live).
- **diff sign:** `diff` is `self - other`, negative when self earlier (live).
- **strptime:** round-trips; rejects bad directive (`%Q`), literal mismatch, month 13, Feb 30, trailing input (all throw).
- **datetime float epoch:** guards NaN/inf/out-of-range before the int64 cast.
- **sys env:** getenv-missing→None; default honored; setenv/getenv round-trip; empty value distinguishable from unset; Unicode value preserved; unsetenv of a missing name is a clean no-op; environ() snapshot; non-String key/value throw; `setenv("A=B",...)` correctly rejected (POSIX EINVAL).
- **process exec:** empty argv throws; non-String arg throws; non-existent program → clean "failed to start"; non-zero exit captured (code 3) not thrown; signal death → 128+signal (137); NUL byte in stdout preserved (len 3); 5 MB stdout no deadlock; 2 MB stdin fed to a program that ignores it → no deadlock / no SIGPIPE crash; timeout=1 kills `sleep 30` and throws in <3 s; grandchild (`sleep 30 & sleep 30`) also killed by the process-group SIGKILL; timeout=0 disables the timeout.
- **sys.exit codes:** `exit(3)`→3, `exit()/exit(None)/exit(0)`→0, `exit("boom")`→stderr "boom" + status 1, `exit(258)`→2 (OS 8-bit truncation), `exit(-1)`→255.
- **clocks:** monotonic/perfcounter steady; time/timens consistent.

## DRY / structure notes (no defect)
- `runExternalProcess` cleanly shares the spawn/capture core between `createprocess` and `shell`.
- `timegmCompat`/`gmtimeCompat` (Hinnant civil arithmetic) are globals-free inverses; the ±2e9
  component bound in `make` keeps `days*86400` well within int64.
- `envMutex` is the right minimal fix for the one legitimately-shared global (env).

## Summary
- **NEW confirmed bugs:** 1 real (A16-1 sleep float-cast-overflow UB / unbounded hang — low-med), plus 3 minor (A16-3 misleading cwd error; A16-4 format empty-result balloon; A16-5 negative-year iso not round-trippable).
- **NEW coverage gaps:** 1 (A16-2 sleep overflow/inf untested).
- **Prior findings re-confirmed still-present (merged, not re-scored):** A19-2, A19-3, A19-5, A19-9, A19-10.
- **Prior findings verified FIXED in v1.14:** A19-1, A19-4.

**Top item:** A16-1 — `time.sleep(inf)` / `time.sleep(1e19)` silently return immediately via
float-cast-overflow UB in `sleep_for`, while `time.sleep(1e18)` hangs unboundedly; `sleep` is the one
float entry point missing the finite/range guard the rest of the numeric stack applies. Fix: guard/clamp
non-finite and out-of-range durations before `sleep_for`.
