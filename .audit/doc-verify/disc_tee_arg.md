# Doc-vs-impl audit: `tee` and `arg` (Kirito-authored stdlib, `src/kirito/stdlib_kimodules.hpp`)

Tests: `tools/tests/scripts/verify_tee.ki` (+ `.expected` = `OK tee`) and
`tools/tests/scripts/verify_arg.ki` (+ `.expected` = `OK arg`). Both green under
`build-debug/ki`, exact stdout match, zero stderr. No `src/` changes.

## Verdict: no defects. Docs match implementation. Several behaviours were under-documented
edge cases — PINNED below as the *current, intended-looking* behaviour so a future change is caught.

---

## tee — PINNED behaviour

- **`t.write(data)` returns `len(data)` = String CODE-POINT count, NOT the UTF-8 byte count.**
  The module comment and prose both call it a "byte count", but the impl is `return len(data)`
  (`stdlib_kimodules.hpp:1055`). `t.write("café")` returns **4** (code points), not 5 (UTF-8 bytes).
  ASCII-only writes hide this (existing `spec_tee.ki` only ever writes ASCII). *Minor doc wording
  imprecision, not a bug* — the returned count is consistent with `String`'s `len`. Left as-is;
  pinned by an assertion so any change to the return value trips the test.
- **Write order is copies-first-then-primary, and a failing copy short-circuits BEFORE the primary.**
  `write` loops copies then writes primary (`:1050-1055`). If a copy's `write` throws, the primary
  never receives the chunk — a *partial* write, not all-or-nothing. Documented order is honoured;
  the partial-write consequence is pinned.
- **`flush` / `close` are tolerant.** `flush` routes every stream through `_maybeFlush`, which
  swallows a missing/throwing `flush()` (`:1018-1025`). A duck-typed sink with only `write` flushes
  fine. `close` **only flushes** — it does NOT close the underlying streams, so a Tee is still
  writable after `close()` (matches the doc: "does not close the copy streams you supplied").
- **`write` is NOT tolerant** (by design, and correct): a copy that throws, or a non-stream copy
  (e.g. `42`, no `write`), or a non-sequence `data` (`write(5)` → `len(5)` fails) all raise a
  catchable error.
- `copies` accepts a single stream or a List; `primary=None` makes a pure fan-out sink omitted from
  `streams()`. `streams()` returns `[copies..., primary]`. `Tee` works as a `with`-context
  (`_enter_`→self, `_exit_`→flush). `tee_stdout`/`tee_stderr` hook `io.stdout`/`io.stderr` and
  restore on exit — including on an exception thrown inside the block (restored, copies never closed).
  All verified.

## arg — PINNED behaviour (these are the "untested paths" the inventory flagged)

- **`--flag=value` sets the flag `True` and SILENTLY DISCARDS the inline value** — no error
  (`:1220-1221`; the `inline` value is only consulted in the option branch). `parse(["--loud=x"])`
  → `loud == True`, `x` dropped. Pinned as current behaviour.
- **`-5` (dash + non-letter) is a POSITIONAL, not a short option.** The short-option branch requires
  `len(token) == 2 and token[1:].isalpha()` (`:1232`), so a negative number survives as a positional
  argument (the documented rationale). Pinned.
- **`-ab` (len 3) does NOT bundle into `-a -b`** — it fails the `len == 2` guard and falls through as
  a single POSITIONAL `-ab`. No short-flag bundling. Pinned.
- **`--` alone is an "unknown option: --"** (empty name after stripping `--`), NOT an
  end-of-options marker. **`-` alone is a positional** (len 1). Both pinned.
- **Ambiguous short option → first-declared wins**, and **`_byshort` checks flags before options**,
  so a flag beats an option on a first-letter tie (`:1150-1158`). Pinned.
- **Type conversion keys off the default's type**: Integer default → `Integer(value)` (throws
  `expects an integer` on a bad value), Float default → `Float(value)` (`expects a number`), anything
  else (String, **None**, Bool) keeps the value a String. So `option("who")` (default None) returns a
  String, and a Bool default would too. Pinned.
- **Negative numbers are valid option values**: `--count -3` → `-3` (the `-3` is consumed as the
  option's value before the short-option guard applies).
- Confirmed matching docs: chainable declarations return self; `-h`/`--help` prints `usage()` to
  `io.stdout` and returns `None` (captured in tests to keep stdout deterministic); `--help`
  short-circuits before the missing-positional check; extra positionals collect under `"rest"`;
  `--name value` / `--name=value` / `-n value` / `-f` forms; hardening errors (unknown option,
  missing positional, bad conversion, option-needs-a-value for both long and short) all raise the
  documented messages.

## Coverage / counts

- `verify_tee.ki`: ~46 `assert`s + 1 `io.print`. Functioning (fan-out, order, single/List/None
  copies, no-copy, writelines, flush/close), edge (empty write returns 0, code-point count, unicode,
  100k payload, 3 sinks, nested Tee, tolerant flush, with-context), context managers
  (tee_stdout/tee_stderr incl. exception-restore), hardening + silent-error probes.
- `verify_arg.ki`: ~44 `assert`s + 1 `io.print`. Functioning (declare/chain/parse, defaults, flags,
  short opts, `--name=value`, rest), type conversion (Int/Float/String/None defaults, negatives),
  usage() text, `-h`/`--help` (captured), hardening via `raises_msg` (7 error paths), and 8
  silent-error PIN probes (`--flag=value`, `-5`, `-ab`, `--`, `-`, ambiguity, flag-vs-option tie).
