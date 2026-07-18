# Coverage findings — system stdlib (io, path, sys, time)

Test: `tests/scripts/cov_system.ki` (251 assertions), golden `cov_system.expected`.
Verified against `build-bin/ki-release` (kVersion 1.16.1). Deterministic (byte-identical across
runs), side-effect-free (all files under a private `path.gettempdir()/kirito_cov_system_scratch`
tree, removed on exit).

## Bugs / silent errors
None. Every documented function/method/attribute behaves as documented; every should-error case
(open missing-for-read, NUL in path, bad mode, wrong-mode/closed stream, negative/bad-whence seek,
non-Integer File.read, strict path mutation, out-of-range/mismatched strptime, non-finite/too-large
sleep, join with no parts, getsize on a missing path) throws a catchable error with the correct
diagnostic substring. No silent fallback observed.

## Brief-vs-implementation deltas (NOT bugs — the task brief over-listed; docs match the impl)
These names appear in the task brief but are **not implemented** and are (correctly) absent from
`docs/pages/10-stdlib.md`. Accessing them raises the normal "no member"/"no attribute" error, so the
brief's list was aspirational, not a doc gap:

- `path.abspath`, `path.normpath`, `path.split` — do not exist on the `path` module
  (`module 'path' has no member '<name>'`). Only `join/dirname/basename/splitext` are the string ops.
- `sys.argv` — does not exist on `sys` (`module 'sys' has no member 'argv'`). `sys` exposes
  `platform/arch/version/getenv/setenv/unsetenv/environ/traceback/exit/createprocess/shell`.
- `File.truncate` — does not exist (`type 'File' has no attribute 'truncate'`). `truncate()` is a
  `BytesIO`-only method; `File` has read/readline/readlines/write/writelines/seek/tell/flush/close.

## Minor observations (no action required)
- `io.open`'s internal arity guard string `"open expected 1 or 2 arguments"` is effectively dead:
  the argument framework enforces arity first, so `io.open()` yields
  `"open() missing required argument 'path'"` and `io.open(p, "r", "extra")` yields
  `"open() takes at most 2 positional argument(s) but 3 given"`. The internal message is never
  surfaced. Cosmetic only; the failure is still correct and catchable.
- `BytesIO.close()` and `BytesIO.flush()` are intentional no-ops (documented as "part of the stream
  protocol"); the buffer stays fully readable/writable after `close()`. Matches docs — reads are not
  promised to throw on a closed BytesIO (unlike a closed `File`, which does throw). Not a silent error.
- On POSIX, `sys.setenv(name, "")` stores an empty string and `getenv` returns `""` (the docs note
  that empty-as-unset is Windows-only behaviour). Confirmed on this Linux box.
