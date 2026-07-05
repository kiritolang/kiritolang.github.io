# A19 — Audit: sys + time + proc_compat

Scope: `src/kirito/stdlib_sys.hpp`, `src/kirito/stdlib_time.hpp`, `src/kirito/proc_compat.hpp`.
Method: static reasoning only (no build/run). Read-only; no source/test modified.

Existing tests reviewed: `tools/tests/unit/test_sys.cpp`, `test_time.cpp`, `test_sys_time_deep.cpp`,
`test_proc.cpp`; scripts `sys_proc.ki`, `sys_proc_adversarial.ki`, `sys_proc_encoding.ki`,
`sys_proc_ffmpeg.ki`, `sys_proc_grandchild_kill.ki`, `spec_time.ki`, `audit_time.ki`,
`spec_traceback.ki`, `errors/strptime_trailing.ki`.

Overall: this surface is unusually well-tested (process fork/exec/pipe/thread machinery, deadlock,
grandchild-kill, resource-leak loops, NUL/Unicode fidelity, DateTime fields/arith/strptime/serde).
Findings below are the residual gaps and a genuine correctness hole in `DateTime` range validation.

---

## Confirmed bugs

### A19-1: `DateTime::setEpoch` range guard reads the int-narrowed `tm_year`, so pathological huge epochs are wrongly accepted as a corrupt DateTime
- **severity:** medium
- **location:** `stdlib_time.hpp:49-74` (`gmtimeCompat`) + `stdlib_time.hpp:148-154` (`setEpoch`)
- **category:** correctness / narrowing conversion
- **description:** `gmtimeCompat` computes the true year in `int64_t y`, then narrows: `tm.tm_year = static_cast<int>(y - 1900)`. For a huge epoch (`datetime(secs)` with `secs` up to `INT64_MAX ≈ 9.2e18`), the true year reaches `~2.9e11`, so `y - 1900` does not fit in `int` and wraps (well-defined modulo since C++20, so no UB, but garbage). `setEpoch` then recomputes `int64_t year = static_cast<int64_t>(tm.tm_year) + 1900;` **from the already-narrowed field** and checks `year < -9999 || year > 9999`. Because the check reads the wrapped value, it is not robust: for the (infinitely many) reachable `int64` epochs whose true year wraps back into `[-9999, 9999]`, the guard passes and a `DateTime` is constructed successfully — but its `.year`/`.iso()`/`.month` fields describe the wrapped garbage year while `.timestamp`/`epoch` hold the true huge value. The object is inconsistent, yet it is hashable-by-epoch and serializable.
- **failure-scenario:** Pick `y - 1900 = 2^32` (narrows to `tm_year = 0` → reported year 1900). The true year `4294969196` corresponds to an epoch of roughly `1.355e17` seconds, well within `int64`. `datetime(<that epoch>)` returns a `DateTime` reporting `year == 1900` / `iso() == "1900-..."` but `timestamp == 1.355e17`. `.diff()`/equality/serialization then all operate on the huge epoch, disagreeing with the displayed date. Most huge epochs *do* incidentally throw (the wrap rarely lands in the 20000-wide window), which is exactly why existing tests — which only probe `make(year,...)` where components are bounded to ±2e9 and `tm_year` never overflows `int` — never hit it.
- **why `make` is safe but `datetime`/`add`/`sub` are not:** `make` bounds each component to ±2e9 (`stdlib_time.hpp:361`), and `2e9 - 1900 < INT_MAX`, so `tm_year` never narrows and `make(1000000,1,1)` correctly throws. The overflow is only reachable through `datetime(bigint)`, `DateTime.add/sub` producing a large epoch, and `dump`/`serialize` `_setstate_(bigint)`.
- **proposed-test:** in `test_sys_time_deep.cpp`, assert `datetime(9223372036854775807)` throws, and construct the crafted epoch above and assert it throws (not silently accepted with a mismatched year); assert `datetime(0).add(9000000000000000000)` throws cleanly.
- **proposed-fix:** validate the true year *before* narrowing — have `gmtimeCompat` compute `y` and either return/expose it, or add an explicit range check on the pre-narrow `int64` year inside `setEpoch` (e.g. compute the year with a small helper on `secs` before calling `gmtimeCompat`, or make `gmtimeCompat` throw/flag when `|y| > 9999`). The narrowing `static_cast<int>(y - 1900)` should never run on an out-of-range `y`.
- **confidence:** high (logic is directly readable; the exact triggering epoch is guaranteed to exist within the reachable `int64` range).

### A19-2: POSIX `proccompat::run` leaks pipe fds when a later `pipe()` in the short-circuit chain fails
- **severity:** low (only under fd exhaustion, but it worsens exactly that condition)
- **location:** `proc_compat.hpp:242-243`
- **category:** resource leak
- **description:** `if (::pipe(inP) != 0 || ::pipe(outP) != 0 || ::pipe(errP) != 0 || ::pipe(exP) != 0) throw ProcError(...)`. If `pipe(inP)` succeeds but `pipe(outP)` fails, the `||` short-circuits straight to `throw` without closing the two already-open `inP` fds. If `errP`/`exP` fail, 4/6 fds leak respectively. The Windows path (`proc_compat.hpp:118-122`) is careful to `CloseHandle` all partially-created handles on `CreatePipe` failure; the POSIX path is not symmetric.
- **failure-scenario:** run near the process fd limit (e.g. after many leaked handles or a low `RLIMIT_NOFILE`); each failed spawn permanently leaks 2–6 fds, accelerating exhaustion instead of recovering cleanly.
- **proposed-test:** hard to unit-test without fault injection; a `setrlimit(RLIMIT_NOFILE)` harness that drives `run()` to `pipe()` failure and then verifies fd count didn't grow would cover it.
- **proposed-fix:** create the pipes one at a time and close all previously-opened fds on the first failure (mirror the Windows cleanup), or use a small RAII fd-pair guard.
- **confidence:** high.

### A19-3: Windows timeout conversion — `static_cast<DWORD>(timeoutSecs * 1000.0)` is UB for large timeouts and rounds sub-millisecond to an immediate timeout
- **severity:** low-medium (Windows-only; no Windows sanitizer gate in CI)
- **location:** `proc_compat.hpp:184`
- **category:** integer/float overflow + edge behavior
- **description:** `waitMs = timeoutSecs > 0 ? static_cast<DWORD>(timeoutSecs * 1000.0) : INFINITE`. (a) A timeout larger than ~49.7 days (`4294967 s`) makes `timeoutSecs*1000` exceed `DWORD` (`uint32`) max; a float→unsigned conversion whose truncated value doesn't fit the target is **undefined behavior** in C++ (not the defined signed-narrowing of A19-1). (b) A small positive timeout (`< 0.001 s`) truncates to `0` → `WaitForSingleObject(h, 0)` returns `WAIT_TIMEOUT` immediately, so a process that would finish instantly is reported as timed-out and killed. The POSIX path avoids both by using a `steady_clock` deadline in `double`.
- **failure-scenario:** `sys.shell("...", timeout = 5000000)` (58 days) → UB in the cast on Windows; `sys.shell("true", timeout = 0.0005)` on Windows → spurious "process timed out".
- **proposed-test:** Windows-only (portable smoke set can't easily assert this); at minimum a comment/clamp.
- **proposed-fix:** clamp to `INFINITE` when `timeoutSecs*1000.0 >= (double)INFINITE`/`MAXDWORD`, and round up a tiny positive timeout to at least `1` ms (or loop with `WaitForSingleObject(h, small)` against a `steady_clock` deadline like POSIX).
- **confidence:** high on the UB; medium on real-world impact (Windows + large/tiny timeout).

### A19-4: `sys` environment access is not thread-safe across `parallel` worker VMs
- **severity:** medium
- **location:** `stdlib_sys.hpp:94-150` (`getenv`/`setenv`/`unsetenv`/`environ`)
- **category:** data race / concurrency
- **description:** `KiritoVM`s are share-nothing per the design, but the C process environment is genuinely shared global state. `getenv`/`setenv`/`unsetenv` and iterating `environ` are not thread-safe with each other (POSIX explicitly says so; `setenv` may reallocate the `environ` array). Two `parallel` workers — each a separate VM on its own OS thread — that call `setenv`/`getenv`/`environ()` concurrently race on the CRT's `environ`, risking a torn read or use-after-free while `environ()` walks the array. There is no lock. The CLAUDE.md `parallel` claim that "workers share nothing" does not hold for the process environment.
- **failure-scenario:** `parallel.spawn` a worker that loops `sys.setenv(...)` while the main VM loops `sys.environ()` / `sys.getenv(...)` → intermittent crash or garbage under TSan.
- **proposed-test:** a `parallel` soak that has one worker churn `setenv` while another reads `environ()`/`getenv`, run under `tsan` (the existing `tsan` gate would flag the race).
- **proposed-fix:** guard the four env entry points with a process-wide mutex (a function-local `static std::mutex`), accepting that this is the one legitimately-shared global; document that env is process-global, not VM-local.
- **confidence:** medium-high (the race is real; whether any current program triggers it is unknown, hence not "high").

### A19-5: `sys.exit` (`std::_Exit`) does not flush buffered open files, losing their unwritten data
- **severity:** low-medium
- **location:** `stdlib_sys.hpp:161-166` (`doExit`)
- **category:** data loss (deliberate tradeoff, but incomplete)
- **description:** `doExit` flushes `std::cout`/`std::cerr` and all C `stdio` streams (`fflush(nullptr)`) then calls `std::_Exit`, which does **not** run static/automatic destructors (chosen in A10-2 to avoid the `parallel`-worker atexit deadlock). But Kirito's `io.open` files are `std::fstream` (`stdlib_io.hpp:59`), a C++ buffered stream that is neither a C `stdio` stream nor flushed by `cout.flush()`. So any buffered writes to an open file that hasn't been `.flush()`/`.close()`d are lost when a program calls `sys.exit(...)`. `BytesIO` buffers are in-memory and lost regardless.
- **failure-scenario:** `var f = io.open(p, "w"); f.write("important"); sys.exit(0)` → `p` may be empty/short on disk (data sat in the `fstream` buffer, never flushed).
- **proposed-test:** open a file, write without flushing, `sys.exit(0)` from a child process, then assert the file content from the parent (needs a subprocess harness since `sys.exit` terminates the runner).
- **proposed-fix:** hard given the `_Exit` constraint; options: have the io module register open files on the VM and flush them in `doExit`, or document that `sys.exit` does not flush user file buffers (call `.flush()`/`.close()` first). At minimum document the sharp edge.
- **confidence:** medium (behavior follows directly from `_Exit` + `fstream` buffering; real impact depends on whether callers rely on it).

---

## Coverage gaps (no correctness claim, but untested angles)

### A19-6: argv / cwd elements containing an embedded NUL silently truncate
- **severity:** low
- **location:** `stdlib_sys.hpp:196` (argv build), `proc_compat.hpp` (`execvp`/`chdir` use `c_str()`)
- **category:** gap + silent-truncation
- **description:** Kirito `String`s are byte-transparent and may hold `\0`. `createprocess([prog, "a" + chr(0) + "b"])` passes `std::string("a\0b").c_str()` to `execvp`, which sees only `"a"`. An OS argv element cannot contain NUL, so this is unavoidable at the syscall level, but the current code silently truncates rather than rejecting. The encoding test deliberately uses NUL only as a *separator* ("a NUL can never appear inside an OS argv element"), so the truncation case is untested.
- **proposed-fix:** reject an argv/cwd element containing `\0` with a clear error ("createprocess: argument contains a NUL byte"), analogous to how `strptime` rejects trailing input.
- **confidence:** high (behavior), low (severity).

### A19-7: `sys.exit` is untested
- **severity:** low (understandable — it terminates the process)
- **location:** `stdlib_sys.hpp:152-174`
- **category:** gap
- **description:** No test drives `sys.exit` (Integer status, None→0, non-Integer→stderr+exit 1) because it would kill the CTest runner. The status-code truncation to 8 bits by the OS, the non-Integer "error message to stderr, exit 1" path, and the flush behavior are all unverified. Could be covered via a subprocess harness (spawn `ki -c "import('sys').exit(3)"` and assert the code), similar to how the proc scripts already spawn `ki` as a child.
- **proposed-fix:** add a subprocess-based test asserting exit codes and the non-Integer→1 path.
- **confidence:** high.

### A19-8: extreme-epoch `datetime`/`add`/`sub` clean-throw is unasserted
- **severity:** low (pairs with A19-1)
- **category:** gap
- **description:** Tests cover `make` component/year ranges thoroughly and `add/sub` with inf/NaN, but nothing asserts that `datetime(<near INT64_MAX>)`, `datetime(0).add(<huge int>)`, or a huge `_setstate_` epoch throw a clean "out of representable range" rather than either overflowing or (per A19-1) succeeding with a corrupt year.
- **proposed-fix:** see A19-1 proposed-test.
- **confidence:** high.

### A19-9: negative / NaN `timeout` silently means "wait forever", undocumented and untested
- **severity:** low
- **location:** `proc_compat.hpp:184` (Win), `proc_compat.hpp:304` (POSIX) — both gate on `timeoutSecs > 0`
- **category:** gap + edge behavior
- **description:** A negative timeout, and (because `NaN > 0` is false) a NaN timeout, both fall through to the "no timeout / INFINITE wait" branch. The docs say a *positive* timeout kills; the negative/NaN behavior (silent infinite wait — a potential hang) is neither documented nor tested. `sys.shell(cmd, timeout = 0/0)` (NaN) would hang on a non-terminating child instead of erroring.
- **proposed-fix:** reject a NaN/negative timeout with a clear error, or document that `<= 0`/NaN disables the timeout.
- **confidence:** high (behavior), low (severity).

### A19-10: `DateTime.format` with timezone directives (`%Z`, `%z`, `%s`) on a tz-less `tm` is platform-dependent and untested
- **severity:** low
- **location:** `stdlib_time.hpp:226-255`
- **category:** gap
- **description:** `dt.tm` is populated by `gmtimeCompat` with no `tm_gmtoff`/`tm_zone` (not set; also not portable fields), so `strftime("%Z")`/`"%z"`/glibc `"%s"` produce platform-dependent output (empty, "GMT", or the *local* zone). DateTime is UTC-only by design, so these directives are meaningless here but silently pass through to `strftime`. Untested; a user formatting `%Z` gets undefined-ish results.
- **proposed-fix:** document that `format` is a thin `strftime` and timezone directives are unsupported for the UTC-only DateTime (or explicitly set `tm_gmtoff = 0`/`tm_zone = "UTC"` where available).
- **confidence:** medium.

---

## DRY / structure notes (no defect)
- `runExternalProcess` (`stdlib_sys.hpp:33`) is a clean shared core for `createprocess` and `shell`; the only difference is argv construction — good DRY.
- `proc_compat.hpp`'s platform split mirrors `net_compat.hpp` and keeps the drain-thread/timeout/exec-error-channel logic in one `run()` per platform. The exec-error channel (CLOEXEC pipe → EOF = success) and the process-group / Job-Object timeout kill are both correct and well-commented.
- `timegmCompat`/`gmtimeCompat` (Hinnant civil arithmetic) are shared inverses reused by `make`/`strptime`/`setEpoch`/`tm_yday`; sound and globals-free. The one weakness is the narrowing in A19-1.

---

## Summary
- **Confirmed bugs:** 5 (A19-1 medium correctness; A19-2 low fd leak; A19-3 low-med Windows UB; A19-4 medium env race; A19-5 low-med exit flush).
- **Coverage gaps:** 5 (A19-6 NUL-in-argv; A19-7 sys.exit untested; A19-8 extreme-epoch throw; A19-9 NaN/negative timeout; A19-10 tz format directives).

**Top 5:**
1. **A19-1** — `setEpoch` validates the int-narrowed `tm_year`, so certain huge `int64` epochs (via `datetime()`/`add`/`sub`/`_setstate_`) are accepted as a corrupt DateTime whose `.year`/`.iso()` disagree with its `.timestamp`. Fix: range-check the year before narrowing.
2. **A19-4** — `sys.getenv/setenv/unsetenv/environ` race across `parallel` worker threads on the shared process `environ` (no lock); would trip TSan. Fix: process-wide mutex.
3. **A19-2** — POSIX `run()` leaks 2–6 pipe fds when a later `pipe()` in the `||` chain fails, worsening fd exhaustion. Fix: close partial pipes on failure (mirror the Windows path).
4. **A19-3** — Windows `static_cast<DWORD>(timeout*1000)` is UB for timeouts > ~49.7 days and rounds sub-ms timeouts to an immediate spurious timeout. Fix: clamp/round.
5. **A19-5** — `sys.exit` (`std::_Exit`) flushes stdio + iostreams but not open `std::fstream` files, silently dropping their buffered data.
