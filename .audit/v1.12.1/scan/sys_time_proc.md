# v1.12.1 (audit loop) — sys, time, external-process execution

Source: src/kirito/stdlib_sys.hpp, src/kirito/stdlib_time.hpp, src/kirito/proc_compat.hpp
Probe: ./build-debug/ki

## LOG
- Read all three source files deeply.
- time.make: normalization (C-mktime rollover) works as documented — month 13→next year, day 32→
  next month, hour 25/sec 61 roll over, negative/zero components roll back. Out-of-int32-band
  components (±2e9) throw "make: date component out of range"; huge in-band years throw at the
  DateTime ctor (year outside [-9999,9999]). No overflow/UB — verified epoch extremes.
- strptime: correctly REJECTS month 99, day 32, hour 25, sec 60, feb-29 nonleap, trailing garbage,
  unsupported directives (%A). Partial formats + empty format OK. Leading-zero years OK.
- DateTime value-equality + hashing by instant confirmed (== , dict key, set dedup all work).
  add/sub (Int + Float via toInt64Checked truncation), diff, overflow guard (__builtin_*_overflow),
  out-of-year-range results throw cleanly. NaN/inf deltas rejected. serialize + dump round-trip OK.
  _setstate_ one-shot guard fires on an established value.
- sleep: ≤0 no-op, NaN/inf throw, >1e9 throw. monotonic non-decreasing.
- datetime(): NaN/inf/2^63 float and out-of-range int all throw cleanly (no float-cast UB).
- subprocess: exit-code capture (true/false/exit N/signaled 128+sig), stdout+stderr separate drain
  (5000-line both-stream flood, no deadlock), timeout SIGKILL (~0.5s), grandchild killed via process
  group, SIGTERM-ignoring child still dies (SIGKILL). Binary Bytes in/out byte-exact through cat.
  String input UTF-8 encoded. argv passed verbatim (no shell injection). Nonexistent program,
  empty args, non-String arg, bad cwd all throw. cwd works. env round-trips + edge cases OK.
- Ruled out: fork-in-multithreaded-worker uses only async-signal-safe calls pre-exec (correct);
  exec-error channel + drain-thread ordering cannot deadlock; capture 256MiB cap + drain-thread
  try/catch prevent bad_alloc escaping a thread; env mutex serializes the 4 entry points.

## FINDINGS

### F1 [LOW] DateTime.iso() malformed for negative years -1..-999 (sign eats a pad digit)
- where: src/kirito/stdlib_time.hpp:164 (iso() `snprintf(..., "%04d", tm.tm_year+1900, ...)`)
- repro:
```
var time = import("time")
io.print(time.make(-5,1,1).iso())     # -005-01-01T00:00:00
io.print(time.make(-999,1,1).iso())   # -999-01-01T00:00:00
io.print(time.make(-1000,1,1).iso())  # -1000-01-01T00:00:00  (correct width)
```
- actual: year -5 renders as `-005` (sign counts toward the width-4), -999 as `-999` — only 3
  year digits. expected: an ISO-8601 expanded year is minus-plus-four-digits, i.e. `-0005`,
  `-0999`. Years <= -1000 and all non-negative years are fine.
- Compounds an existing asymmetry: strptimeCompat's `%Y` has no sign handling, so a negative-year
  iso string can't be re-parsed at all (round-trip already broken). DateTime's documented contract
  is year in [-9999,9999], so negative years are in-range and should format correctly.
- fix idea: format the year with an explicit sign/width, e.g. `%s%04d` on `abs(year)` with a
  leading `-`, or `snprintf(..., "%+05d", year)` semantics giving a 4-digit magnitude. (strptime
  gaining optional leading `-` on %Y would restore round-trip, but that's separate.)

### F2 [LOW/SUSPECT] Embedded NUL in an argv element / cwd / shell command / env name silently truncates
- where: src/kirito/proc_compat.hpp:248 (`cargv.push_back(const_cast<char*>(s.c_str()))`), and the
  same c_str() truncation for cwd (chdir), the shell command string, and sys.setenv/getenv names.
- repro:
```
var sys = import("sys")
io.print(sys.createprocess(["printf","[%s]","a\x00b"])["stdout"])   # => [a]
```
- actual: a Kirito String may contain NUL bytes; passed as an argv element it is silently truncated
  at the first NUL ("a\0b" -> "a"), losing data. Same for cwd, a shell command, and an env var
  name/value. expected: arguably a clean throw (Python's subprocess raises "embedded null byte";
  os.setenv likewise). At minimum it is silent data loss.
- Marked SUSPECT because embedded NUL in argv is impossible at the OS (execvp) level, so *some*
  handling is unavoidable; the question is throw-vs-truncate. Low severity / esoteric.
- fix idea: in runExternalProcess / createprocess arg collection (and setenv), reject a String whose
  bytes contain '\0' with a clear error, mirroring the rest of the numeric/IO validation policy.

## NON-FINDINGS / notes (by-design, verified safe)
- binary=False on binary data returns a String whose `len` counts code points, so raw bytes
  mis-count / merge — but this is the documented reason binary=True + Bytes exists; the docstring
  warns of exactly this. Not a bug.
- Negative `timeout` (or NaN) is treated as "no timeout" (blocks). By design (docs say "positive
  timeout"); note only. On Windows `static_cast<DWORD>(inf*1000.0)` would be UB IF timeout were
  +inf, but POSIX (the build here) handles inf/NaN safely via chrono comparison; flag for the
  Windows reviewer only.
- bad cwd surfaces as "failed to start '<prog>': No such file or directory" (chdir errno reuses the
  exec-error channel) — slightly misleading message but a correct, catchable error.
