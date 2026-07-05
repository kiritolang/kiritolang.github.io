# A12 — io + path + streams audit (v1.13)

Scope: `src/kirito/stdlib_io.hpp` (660 L), `src/kirito/stdlib_path.hpp` (261 L).
Tests reviewed: C++ `tools/tests/unit/test_io.cpp`, `test_path.cpp`, `test_io_path_deep.cpp`,
`test_streams.cpp`, `test_bytesio.cpp`; and the golden `.ki` suite
(`file_io.ki`, `path_module.ki`, `spec_io.ki`, `spec_io_stream.ki`, `r4_io.ki`, `r6_io.ki`,
`r7_io_sys.ki`, `r8_io_sys.ki`, `r9_io_err.ki`, `r10_datasys.ki`, `deep_system.ki`).
READ-ONLY audit; no source/test modified. Static reasoning only (no build/run).

**Overall:** The golden `.ki` scripts are dramatically more thorough than the C++ unit tests and
close most of the obvious behavioural gaps (r+ mode, whence 1/2 on File, write-on-readonly,
read-on-writeonly, closed-file, negative seek, whence=3/5, `writelines(42)`, `BytesIO(Bytes)`
rejected, walk recursion + tolerance). Confirmed defects are modest. The most material is `walk`/
`listdir` NOT being tolerant to an *inaccessible subdirectory* despite the documented promise.

Surface enumerated:
- **IoStream** protocol: `streamWrite/streamRead/streamReadLine/streamFlush`.
- **FileVal**: `read(size)`, `readline`, `readlines`, `write(data)`, `writelines(lines)`,
  `seek(offset,whence)`, `tell`, `flush`, `close`/`_exit_`, `_enter_`, lazy + eager line iteration;
  modes `r/w/a/r+` × optional trailing `b` (binary → Bytes).
- **BytesIO**: `write` (returns count), `read(size)`, `readline`, `getvalue`, `seek`, `tell`,
  `size`/`_len_`, `truncate`, `flush`, `close`/`_exit_`/`_enter_`, `length()`.
- **StdStream** (Out/Err/In): `write`, `read` (chunked), `readline`, `flush`, `_enter_`/`_exit_`,
  lazy + eager stdin iteration; rebindable `stdout/stderr/stdin` + `__stdout__/__stderr__/__stdin__`.
- **io module fns**: `BytesIO([initial])`, `print`/`eprint`/`input`/`read`/`write` (all `stream=`-kw
  aware via `pickStream`), `open(path, mode="r")`.
- **streamWriteTo / streamReadLineFrom / streamReadFrom**: IoStream fast path + duck-typed fallback.
- **path module**: `join`(variadic), `dirname`, `basename`, `splitext`, `exists`, `isfile`, `isdir`,
  `getsize`, `listdir`, `walk(dir)`, `getcwd`, `gettempdir`, `executable`, `mkdir(exist_ok)`,
  `remove(missing_ok)`, `rmtree(missing_ok)`, `rename(src,dst)`, `chmod(path,mode)`.

---

### A12-1: `walk` / `listdir` throw (not tolerant) on an inaccessible subdirectory or mid-iteration race
- severity: MEDIUM
- location: stdlib_path.hpp:159-173 (`listdir`, `walk`)
- category: robustness / TOCTOU / doc-contract violation
- description: Both helpers pass an `error_code` only to the iterator *constructor*
  (`directory_iterator(path, ec)` / `recursive_directory_iterator(path, ec)`). The range-based `for`
  then advances with the **throwing** `operator++`. If a subdirectory cannot be entered (e.g. a
  child dir with mode 000), or an entry is removed/replaced concurrently, `operator++` throws
  `std::filesystem::filesystem_error` mid-iteration. `recursive_directory_iterator` is created with
  default `directory_options::none` (no `skip_permission_denied`), so a permission-denied *nested*
  dir aborts the whole walk. Additionally `walk` calls `entry.is_regular_file()` (no-ec overload),
  which likewise throws on a dangling symlink / underlying error. This contradicts the module doc
  ("read-only listing helpers stay tolerant … a missing/inaccessible dir lists as []" and
  "exists/isfile/isdir/listdir/walk are tolerant"). Note: top-level inaccessible dir *is* tolerant
  (ctor ec catches it → empty), so only the nested case leaks.
- failure-scenario: `path.mkdir(d); path.mkdir(d+"/sub"); path.chmod(d+"/sub", 0o000); path.walk(d)`
  → throws a raw `filesystem_error` instead of returning the readable entries. A cron/backup script
  that walks a tree containing one unreadable dir crashes instead of skipping it.
- proposed-test: create `d/sub` chmod 000, assert `path.walk(d)` and `path.listdir(d)` do NOT throw
  (return the accessible entries); also a symlink to a nonexistent target under `d`.
- proposed-fix: iterate manually with the `error_code` `increment(ec)` overload (or construct with
  `directory_options::skip_permission_denied`) and use `entry.is_regular_file(ec)`; break/skip on ec
  so the helpers stay tolerant as documented.
- confidence: HIGH (behaviour of default recursive_directory_iterator is well-defined)

### A12-2: `File.writelines(non-iterable)` surfaces `std::bad_optional_access`, not a clean message
- severity: LOW
- location: stdlib_io.hpp:201-204
- category: diagnostic quality
- description: `auto items = vm.arena().deref(a[0]).iterate(vm);` yields `std::optional<...>`; a
  non-iterable argument returns `std::nullopt` and `items.value()` throws `std::bad_optional_access`.
  It *is* catchable (bare `catch` catches any `std::exception`, and `r4_io.ki:183` asserts it
  throws), but the surfaced text is "bad_optional_access", not an actionable
  "writelines expects an iterable" — inconsistent with the module's otherwise clear diagnostics.
- failure-scenario: `f.writelines(42)` → user sees `bad_optional_access`.
- proposed-test: assert the thrown message contains "iterable"/"writelines" (currently only asserts
  that it throws).
- proposed-fix: `if (!items) throw KiritoError("writelines expects an iterable");` before `.value()`.
- confidence: HIGH

### A12-3: unbounded read-all can OOM instead of a clean catchable error (no resource guard)
- severity: LOW
- location: stdlib_io.hpp:97 (`FileVal::streamRead` no-`n` `ss << rdbuf()`), 398 (`StdStream::streamRead`
  no-`n`); `io.read()` at 603-609
- category: resource-limit / weak-spot
- description: A no-argument `read()` on a File or stdin buffers the entire remaining stream into one
  `std::string`. A multi-GB file / infinite pipe triggers `std::bad_alloc` (or thrash), not the
  bounded, catchable error Kirito applies elsewhere (huge string/list repetition, `range`, Tensor
  shape, regex repeat are all guarded — see CLAUDE.md "Resource guards"). `FileVal::streamRead(n)`
  DOES clamp `want` to bytes-remaining (good), and `BytesIO`/`StdStream(n)` are bounded, so only the
  *read-all* path is unguarded. Python-parity, but inconsistent with Kirito's stated guard policy.
- failure-scenario: `io.open("/dev/zero","rb").read()` or `io.read()` on `cat huge | ki` → OOM.
- proposed-test: (hard to test portably) — document the limit; optionally a bounded read-all with a
  configurable cap that throws "read too large".
- proposed-fix: cap the accumulated size in the no-`n` branches (mirroring BytesIO's 256 MiB `kMaxBuf`)
  and throw a catchable "read result too large" past it.
- confidence: MED (real, but by-design parity with CPython)

### A12-4: `readline()` cannot distinguish EOF from a trailing empty line
- severity: LOW
- location: stdlib_io.hpp:113-118 (`FileVal::streamReadLine`), 286-294 (`BytesIO::streamReadLine`),
  422-425 (`StdStream::streamReadLine`)
- category: semantic limitation
- description: All three strip the trailing newline (getline / manual pop), so an empty line and EOF
  both return `""`. Python's `readline()` returns `"\n"` for a blank line and `""` only at EOF,
  giving an unambiguous EOF sentinel; Kirito's loses it. Line *iteration* (`for line in f`) is
  unaffected (getline's bool result terminates the loop), but code that drives `readline()` in a
  `while line != "":` loop will stop early on the first blank line, or loop forever on a stream that
  only yields blanks. At least the three implementations are mutually consistent.
- failure-scenario: `f = "\n\nx\n"`; a `while True: l = f.readline(); if l == "": break` loop stops
  at the first blank line, dropping "x".
- proposed-test: readline() over a file whose first line is empty; assert the empty line is
  distinguishable from EOF (would require a documented sentinel change).
- proposed-fix: document the limitation, or preserve the newline (larger behaviour change).
- confidence: HIGH (behaviour), design-judgement on whether it's a defect.

### A12-5: `io.write`/`io.print` stringify a Bytes arg (repr) rather than writing raw bytes; StdStream.write rejects Bytes though File.write accepts it
- severity: LOW
- location: stdlib_io.hpp:611-618 (`write` uses `vm.stringify`), 567-580/582-593 (print/eprint),
  459-464 (`StdStream::write` uses `asStringRef`), vs 177-182 (`FileVal.write` uses `ioRawBytes` →
  accepts String *or* Bytes)
- category: consistency / binary-safety
- description: Module-level `io.write(b, stream=…)` runs every arg through `vm.stringify`, so a Bytes
  value is written as its repr (`b'...'`), not its raw bytes — whereas the stream object's own
  `stream.write(b)` writes raw bytes. Separately, `StdStream.write` only accepts a String
  (`asStringRef`), so `io.stdout.write(Bytes(...))` throws, while `File.write`/`BytesIO.write` accept
  Bytes. Net: the "write raw binary" path depends on which surface you call and what the stream is.
- failure-scenario: `io.write(Bytes([104,105]), stream=buf)` writes `b'hi'` (5 chars), not `hi`
  (2 bytes); `io.stdout.write(Bytes([...]))` throws.
- proposed-test: assert `io.write(Bytes([104,105]), stream=buf); buf.getvalue()` behaviour is
  documented/intended; assert StdStream.write(Bytes) behaviour.
- proposed-fix: either accept Bytes uniformly via a shared `ioRawBytes` on StdStream.write, or
  document that io.print/write are text surfaces (use `stream.write` for raw bytes).
- confidence: HIGH (behaviour), MED on whether it's a defect vs intended text/binary split.

### A12-6: `read(None)` inconsistent across the three stream surfaces
- severity: LOW
- location: stdlib_io.hpp:162-171 (File.read: `asInt(None)` throws), 319-327 (BytesIO.read: explicit
  `kind()!=None` → None means read-all), 465-470 (StdStream.read: `asInt(None)` throws), 603-609
  (io.read: negative → read-all)
- category: consistency
- description: `BytesIO.read(None)` is treated as read-all; `File.read(None)` and `StdStream.read(None)`
  throw ("read expects an Integer"). Meanwhile negative sizes are read-all everywhere. So `None` and
  a negative int behave differently, and only BytesIO special-cases None. Minor surprise for code that
  passes an optional size through uniformly.
- failure-scenario: `f.read(None)` throws but `bytesio.read(None)` reads all.
- proposed-test: parametrized read(None) / read(-1) across File/BytesIO/StdStream asserting identical
  behaviour.
- proposed-fix: normalize None→read-all (or None→throw) in all three; share a `parseReadSize` helper.
- confidence: HIGH

### A12-7: duck-typed stdout is never flushed even when the caller requests flush
- severity: LOW
- location: stdlib_io.hpp:490-501 (`streamWriteTo`)
- category: behaviour gap
- description: For a built-in IoStream the `flush` flag calls `streamFlush()`. For a duck-typed
  target (a user class exposing `write`) the function returns right after calling `write`, ignoring
  `flush` — even though `print`/`eprint` pass `flush=true` precisely so a server banner / progress log
  is visible immediately. A duck-typed sink with its own buffering won't be flushed, and its `flush`
  method (if any) is never invoked.
- failure-scenario: a user `class Logger` used as `io.stdout` that batches writes never gets a flush
  signal from `io.print`.
- proposed-test: duck-typed stdout with a `flush` method; assert `io.print` invokes it.
- proposed-fix: after the duck-typed `write` call, best-effort call `flush` when `flush && hasattr`.
- confidence: HIGH

### A12-8: `io.print(..., stream=None)` / non-object stream yields an opaque error
- severity: LOW
- location: stdlib_io.hpp:545-555 (`pickStream`) + 490-501 (`streamWriteTo`)
- category: diagnostic quality
- description: `pickStream` accepts any handle bound to `stream=` without type-checking. `stream=None`
  flows into `streamWriteTo`, which derefs None, finds it is not an IoStream, then calls
  `None.getAttr("write")` → throws whatever `Object::getAttr` says for None (e.g. "no attribute
  'write'"), not "stream= must be a writable stream". `stream=42` is tested (`test_io.cpp:120`) and
  throws, but the message quality isn't asserted. Low, but the error could name the offending kwarg.
- failure-scenario: `io.print("x", stream=None)` → confusing "None has no attribute write".
- proposed-test: assert the thrown text mentions `stream`.
- proposed-fix: in `pickStream`, if the chosen value is neither an IoStream nor has a `write`/
  `readline`, throw "stream= expects a writable/readable stream".
- confidence: MED

---

## Coverage gaps (behaviour is correct/plausible but untested)

### A12-9: kwarg invocation of File/BytesIO/StdStream methods untested
- severity: LOW / coverage
- location: File `read`/`seek`/`write`/`writelines`; BytesIO `read`/`seek`/`write`; StdStream
  `read`/`write` — all bound via `makeMethod` with named params (`size`, `offset`, `whence`, `data`)
- description: `makeMethod` gives these keyword-argument support, but no test calls e.g.
  `f.read(size=5)`, `f.seek(offset=0, whence=2)`, `b.write(data="x")`, `s.write(data="y")`. Only
  `path.chmod(path=…, mode=…)` exercises kwargs (test_io.cpp:88). CLAUDE.md claims keywords work
  "uniformly across every callable … built-in type methods … native-object methods"; the io stream
  methods are unverified for it.
- proposed-test: call each stream method by keyword and assert equal to the positional form; also
  out-of-order (`f.seek(whence=2, offset=-2)`).
- confidence: HIGH (gap)

### A12-10: `File.tell`/`flush` in write/append mode; `File.seek(whence=1/2)` return value untested at C++ layer
- severity: LOW / coverage
- location: stdlib_io.hpp:211-246
- description: C++ `test_io.cpp` only tests `tell`/`seek(4)` on a read handle. `r6_io.ki` covers
  whence 0/1/2 and r+ at the script layer, but the C++/embedding entry point (the audited surface for
  a host) has no coverage of: `tell()` after a write, `flush()` explicitly, seek return value,
  seek-from-end on an empty file. Also no test of `tell()` returning the correct value in append mode
  (where the initial position is implementation-defined).
- proposed-test: extend `test_io.cpp` / `test_streams.cpp` with write-mode tell/flush and
  seek-return assertions.
- confidence: HIGH (gap; script-layer partially covers)

### A12-11: `BytesIO` 256 MiB overflow guard untested
- severity: LOW / coverage
- location: stdlib_io.hpp:270-277 (`kMaxBuf`, "BytesIO too large")
- description: The write-size guard (`pos > kMaxBuf || data.size() > kMaxBuf - pos`), including the
  overflow-safe `write-after-seek(9e18)` case the comment cites, has no test. This is exactly the
  resource-guard family Kirito tests elsewhere.
- proposed-test: `b.seek(300*1024*1024); b.write("x")` → throws "BytesIO too large"; and a direct
  giant write. (Actual 256 MiB alloc is heavy — the seek-past-cap path avoids allocating.)
- confidence: HIGH (gap)

### A12-12: `path.chmod` with a negative / out-of-range mode; permission-denied vs missing not distinguished
- severity: LOW / coverage + minor design
- location: stdlib_path.hpp:245-252
- description: `mode` is `static_cast<unsigned>(asInt) & 0xFFF`, so a negative Integer silently wraps
  into valid bits with no diagnostic; untested. `chmod` returns `!ec`, so a permission-denied chmod on
  an *existing* file returns `False` indistinguishably from a missing file — a caller can't tell "no
  such file" from "not permitted". Lenient-by-design, but the conflation is undocumented and untested.
- proposed-test: `path.chmod(f, -1)` behaviour; chmod on a file owned by another user (throws-vs-False).
- proposed-fix (optional): validate `mode >= 0`; consider surfacing the ec reason.
- confidence: MED

### A12-13: `open` embedded-NUL path, empty mode, and `"br"`/`"rb+"` ordering untested
- severity: LOW / coverage
- location: stdlib_io.hpp:619-646
- description: Binary detection is `mode.back()=='b'` only, so `"br"` (Python-valid) is NOT binary and
  falls through to "unsupported file mode 'br'" (arguably fine, but untested and asymmetric with
  Python). Empty mode `""` → "unsupported file mode ''" untested. A path containing a NUL byte (Kirito
  strings may contain NUL) passed to `fstream::open` is untested (likely a clean "could not open").
- proposed-test: `io.open(p, "br")` throws; `io.open(p, "")` throws; open a path with `\x00`.
- confidence: HIGH (gap)

### A12-14: `path.rename` cross-device (EXDEV) and onto a non-empty dir untested
- severity: LOW / coverage
- location: stdlib_path.hpp:236-241
- description: `std::filesystem::rename` fails with EXDEV across mount points and with ENOTEMPTY
  renaming onto a populated dir → "rename failed: …". Overwrite-existing-file is tested
  (`test_io_path_deep.cpp:221`), but the failure modes (which a real deploy hits: `/tmp`→home across
  tmpfs) are untested; behaviour is correct (throws), just unverified. `rename` also can't move across
  filesystems (no fallback copy), unlike `shutil.move` — worth documenting.
- proposed-test: rename onto a non-empty directory asserts throw.
- confidence: HIGH (gap)

### A12-15: `walk` / `listdir` building an unbounded List on a huge tree
- severity: LOW / weak-spot
- location: stdlib_path.hpp:159-173
- description: Both flatten every entry into an in-memory List with no cap, so walking a tree with
  millions of files can OOM. No lazy/generator variant exists (comprehensions/generators are a
  documented future enrichment). Parity with a naive `os.walk` materialization, but unguarded.
- proposed-test: n/a (resource) — note as documented limit.
- confidence: MED

---

## DRY / structural notes

### A12-16: three near-duplicate "parse read size" + "validate whence" + "read up to n" implementations
- severity: LOW / maintainability
- location: File (162-170, 220-246, 95-112), BytesIO (319-356, 278-285), StdStream (465-470, 396-411)
- description: The `!a.empty() → asInt → (>=0 ? clamp)` read-size parsing is repeated 3× (with the
  None-handling divergence flagged in A12-6); the `whence<0||whence>2` validation + `__builtin_add_
  overflow` absolute-target computation is duplicated in File.seek and BytesIO.seek with subtly
  different negative-target policy (File throws, BytesIO clamps to 0 — intentional and tested, but the
  divergence lives in copy-pasted code); and "read up to n bytes" has three independent bodies
  (File's tellg/seek clamp, StdStream's chunked loop, BytesIO's substr). A shared `parseReadSize`
  helper and a shared `resolveSeekTarget(base, off)` would remove the drift risk and fix A12-6 in one
  place. `print`/`eprint` also duplicate the args-join+newline+streamWriteTo body verbatim (567-593).
- proposed-fix: extract `parseReadSize(Value, allowNone)`, `resolveSeekTarget`, and a
  `printLine(target, args, sep, end)` helper.
- confidence: HIGH

---

## Things verified CORRECT (no defect)
- `open` cleans up on failure: FileVal `unique_ptr` destroyed (stream closed) before throw — no fd
  leak (stdlib_io.hpp:643-645). Double-`close()` is a safe no-op (fstream::close on a closed stream).
- `FileVal::streamRead(n)` correctly clamps `want` to bytes-remaining incl. cursor unknown/past-end
  (99-107) — a huge `read(n)` does not pre-allocate n bytes.
- `seek` computes an absolute target once (avoiding the shared get/put double-count) and rejects a
  negative absolute target overflow-safely (232-244). BytesIO seek clamps negative to 0 — both tested.
- Lazy line iteration keeps the FileVal rooted via the iterator's `src` handle (124-138); readlines/
  iterate use RootScope per-line — GC-safe.
- `recursive_directory_iterator` default does NOT follow directory symlinks, so a *cyclic symlink*
  does not loop infinitely (the permission-denied throw of A12-1 is the separate issue).
- `join` absolute-reset (only `/`, not `\`), zero-arg throw, empty-component handling — correct+tested.
- `splitext` leading-dot-run protection over the final component only (`dir.x/file`, `..`, `.bashrc`)
  — correct+tested.
- `mkdir`/`remove`/`rmtree` strict-by-default with exist_ok/missing_ok leniency — correct+tested,
  incl. non-empty-dir `remove` throw and `rmtree` recursive delete.
- `BytesIO(Bytes(...))` rejection is INTENTIONAL and tested (`r4_io.ki:435`) — flagged only as a
  consistency observation under A12-5, not a bug.
- whence=3/5 throw on both File and BytesIO — tested (`r4_io.ki:242,368`, `r10_datasys.ki:99`).
- write-on-readonly / read-on-writeonly / closed-file / negative-seek all throw — tested (`r6_io.ki`,
  `r9_io_err.ki`). r+ read+write-in-place — tested (`r6_io.ki:107`).

---

## Summary
- **1 MEDIUM** confirmed defect: A12-1 (walk/listdir not tolerant to inaccessible *nested* dirs —
  contradicts the documented tolerance).
- **7 LOW** defects/inconsistencies: A12-2 (bad_optional_access message), A12-3 (unbounded read-all
  OOM), A12-4 (readline EOF ambiguity), A12-5 (Bytes stringified by io.write / rejected by
  StdStream.write), A12-6 (read(None) inconsistency), A12-7 (duck-typed stdout not flushed),
  A12-8 (stream= opaque error).
- **7 coverage gaps**: A12-9..A12-15 (kwarg method calls, write-mode tell/flush, BytesIO size guard,
  chmod negative mode, open mode-string edges, rename EXDEV/non-empty, unbounded walk list).
- **1 DRY** cluster: A12-16.

Top 5:
1. **A12-1** — `path.walk`/`listdir` throw a raw `filesystem_error` on a permission-denied subtree /
   mid-iteration race despite the "tolerant / inaccessible → []" contract (MEDIUM).
2. **A12-2** — `File.writelines(non-iterable)` leaks `std::bad_optional_access` instead of a clear
   "expects an iterable".
3. **A12-6** — `read(None)` reads-all on BytesIO but throws on File/StdStream (surface inconsistency).
4. **A12-5** — `io.write`/`io.print` write a Bytes arg as its repr, and `StdStream.write` rejects
   Bytes while `File.write` accepts it — the raw-binary write path is surface-dependent.
5. **A12-9** — no test calls io stream methods by keyword (`f.read(size=…)`, `f.seek(offset=,whence=)`)
   despite the uniform-kwargs guarantee.
