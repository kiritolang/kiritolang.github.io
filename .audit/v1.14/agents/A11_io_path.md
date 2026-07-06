# A11 — io + path + streams audit (v1.14)

Scope: `src/kirito/stdlib_io.hpp`, `src/kirito/stdlib_path.hpp`.
Prior: v1.13 A12 (merged — hunting NEW angles). READ-ONLY on src/.
Probe binary: /home/user/kiritolang.github.io/build-debug/ki

Status: COMPLETE.

---

### A11-1: Embedded NUL byte silently truncates every path (validation/sandbox bypass)
- **severity**: MEDIUM
- **location**: `stdlib_path.hpp` all path fns (exists/getsize/open/mkdir/remove/...) + `stdlib_io.hpp:643` `open`; root cause: Kirito Strings may contain `\x00`, and `std::filesystem::path`/`fstream` truncate at the first NUL when calling the OS via `c_str()`.
- **category**: security / input-validation
- **description**: A Kirito String genuinely holds embedded NULs (`len("ab\x00cd") == 5`). Passed to any path/filesystem function it is truncated at the first NUL, so `path.exists(real + "\x00.blocked")` checks `real`, and `io.open(real + "\x00.blocked", "w")` opens/writes `real`. Python raises `ValueError: embedded null byte` precisely to stop this.
- **failure-scenario**: A program sandboxes a filename as `user + ".safe"` (or blocks a suffix/extension); an attacker supplies `user = "victim\x00"`, the `.safe` suffix is dropped, and the real target `victim` is read/written — classic NUL-byte injection. CONFIRMED: `io.open(real+"\x00.blocked","w"); f.write("PWNED")` populated `real`.
- **proposed-test**: `path.exists("x\x00y")` must throw or must NOT resolve to `x`; `io.open("x\x00y","w")` must throw "embedded null byte".
- **proposed-fix**: reject any path argument containing `\x00` early (a shared `checkPathArg` used by `pathArg`/`open`), throwing "embedded null byte in path".
- **confidence**: CONFIRMED (reproduced write to truncated target)

### A11-2: `io.write`/`io.print`/`io.eprint` stringify a Bytes arg to its repr; raw-binary write path is surface-dependent (A12-5 unfixed)
- **severity**: LOW
- **location**: `stdlib_io.hpp:611-618` (`write` → `vm.stringify`), 567-593 (print/eprint), vs `FileVal.write`/`BytesIO.write` (`ioRawBytes`, accept Bytes raw).
- **category**: consistency / binary-safety
- **description**: `io.write(Bytes([104,105]), stream=buf)` writes `b'hi'` (5-char repr) while `buf.write(Bytes([104,105]))` writes 2 raw bytes. CONFIRMED. So which bytes hit the sink depends on whether you call the module fn or the stream method. `StdStream.write` also rejects Bytes (asStringRef) while File/BytesIO accept it.
- **failure-scenario**: user pipes binary through `io.write(..., stream=file)` expecting raw bytes; gets the Python-repr text instead.
- **proposed-test**: assert `io.write(Bytes([104,105]), stream=buf); buf.getvalue()` == "hi".
- **proposed-fix**: route io.write/print args through `ioRawBytes` for Bytes (or document text-only + point to stream.write).
- **confidence**: CONFIRMED

### A11-3: Duck-typed stdout/stderr never receives the requested flush (A12-7 unfixed)
- **severity**: LOW
- **location**: `stdlib_io.hpp:490-501` `streamWriteTo` — duck-typed branch returns right after `write`, ignoring `flush`.
- **category**: behaviour gap
- **description**: A user class bound to `io.stdout` exposing `write` + `flush` never gets `flush` called, even though `io.print` passes `flush=true`. CONFIRMED: Logger.flushes stayed 0 after `io.print`.
- **failure-scenario**: a buffering duck-typed sink (log-to-file wrapper) never flushes on print, so lines are lost on crash / delayed.
- **proposed-test**: duck-typed stdout with a `flush` method; assert `io.print` invokes it.
- **proposed-fix**: after the duck-typed `write`, if `flush && hasattr(o,"flush")` call it best-effort.
- **confidence**: CONFIRMED

### A11-4: `stream=None` / non-stream yields opaque "None has no attribute 'write'" (A12-8 unfixed)
- **severity**: LOW
- **location**: `stdlib_io.hpp:545-555` `pickStream` (no type check) → `streamWriteTo`.
- **category**: diagnostic quality
- **description**: `io.print("x", stream=None)` throws `type 'None' has no attribute 'write'`; a class w/o write gives `'NoWrite' object has no attribute 'write'`. Never names the offending `stream=` kwarg. CONFIRMED.
- **proposed-fix**: `pickStream` validates the chosen value is an IoStream or has write/readline; else "stream= expects a writable/readable stream".
- **confidence**: CONFIRMED

### A11-5: `io.open(dir, "r")` silently succeeds on a directory and read() returns "" (no IsADirectoryError)
- **severity**: LOW-MEDIUM
- **location**: `stdlib_io.hpp:619-645` `open` (only checks `is_open()`).
- **category**: bug / silent-wrong-behaviour
- **description**: Opening a directory in read mode returns a valid File (`is_open()` true), and `read()`/iteration yield empty rather than throwing. CONFIRMED: `io.open(gettempdir(),"r").read()` == "". Write mode correctly fails ("could not open"). Python raises `IsADirectoryError`. The asymmetry means a bug (passing a dir where a file was expected) is masked as an empty file.
- **failure-scenario**: a program that opens a user-supplied path and reads it treats a directory as an empty file instead of erroring — silent data loss / wrong control flow.
- **proposed-test**: `io.open(gettempdir(),"r")` should throw (or at least `read()` should) "is a directory".
- **proposed-fix**: after a successful open, reject if `std::filesystem::is_directory(path)` (throw "is a directory: <path>").
- **confidence**: CONFIRMED

### A11-6: `File.write`/`StdStream.write` return None; `BytesIO.write` returns the byte count (surface inconsistency)
- **severity**: LOW
- **location**: `stdlib_io.hpp:177-182` (File.write → none), 459-464 (StdStream.write → none), 312-318 (BytesIO.write → count).
- **category**: consistency
- **description**: CONFIRMED: `File.write("abc")` → None, `io.stdout.write("")` → None, `BytesIO.write("abc")` → 3. Python's write returns the count uniformly; code that reads the return (`n = f.write(...)`) works on BytesIO but gets None on a File.
- **proposed-test**: assert all three return the byte count.
- **proposed-fix**: return the written byte count from File.write and StdStream.write too.
- **confidence**: CONFIRMED

### A11-7: `read(None)` reads-all on BytesIO but throws on File/StdStream (A12-6 unfixed)
- **severity**: LOW
- **location**: File.read 162-171, StdStream.read 465-470 (`asInt(None)` throws), BytesIO.read 319-327 (None → read-all).
- **category**: consistency
- **description**: CONFIRMED: `File.read(None)` → "read expected Integer, got 'None'"; `BytesIO.read(None)` → reads all. Negative int reads-all everywhere; only None diverges.
- **proposed-fix**: share a `parseReadSize(Value, allowNone)` across the three surfaces.
- **confidence**: CONFIRMED

### A11-8: `readline()` cannot distinguish a blank line from EOF (A12-4 unfixed)
- **severity**: LOW
- **location**: File.streamReadLine 113-118, BytesIO 286-294, StdStream 422-425 (all strip the trailing '\n').
- **category**: semantic limitation
- **description**: CONFIRMED on BytesIO("\n\nx\n"): lines 1,2 → "", EOF → "" — indistinguishable. A `while line != "":` loop stops at the first blank line. Python returns "\n" for a blank line, "" only at EOF. Line *iteration* is unaffected.
- **proposed-fix**: document, or preserve the newline as an EOF sentinel (behaviour change).
- **confidence**: CONFIRMED

### A11-9: append-mode `seek()`/`tell()` are actively misleading (writes ignore the cursor, tell() reports the wrong offset)
- **severity**: LOW
- **location**: `stdlib_io.hpp` `FileVal` opened with `std::ios::app` (line 640) + seek/tell (211-246).
- **category**: semantic footgun
- **description**: In append mode every write goes to end (std::ios::app), but seek/tell operate on the get pointer. CONFIRMED on a 5-byte file opened "a": `f.seek(0); tell()==0; f.write("AB"); tell()==2` yet the data landed at offset 5 (file became "12345AB"). So tell() reports 2 while the bytes are at 5 — seek/tell arithmetic in append mode is silently wrong.
- **failure-scenario**: code that records `pos = tell()` before/after an append write to index records computes wrong offsets.
- **proposed-fix**: document that seek/tell are meaningless for writes in append mode (or force tell() to report the put position / reject seek in append mode).
- **confidence**: CONFIRMED

### A11-10: `BytesIO.truncate(size)` silently ignores its argument (always truncates to the cursor)
- **severity**: LOW
- **location**: `stdlib_io.hpp:361-366` `truncate` (bound with no params; body always `buf.resize(pos)`).
- **category**: bug / API divergence
- **description**: CONFIRMED: `b = BytesIO("hello world"); b.seek(3); b.truncate(2)` returns 3 and leaves value "hel" — the `2` argument is dropped, truncation is to the cursor (3). Python's `truncate([size])` truncates to `size` if given. Because the method has zero declared params the extra positional arg is silently swallowed (same for `tell(99)`, `getvalue(9)` etc. — extra positional args on no-param native methods are ignored, not rejected).
- **failure-scenario**: `b.truncate(n)` to cap a buffer at n bytes silently truncates to wherever the cursor happens to be instead.
- **proposed-test**: `b.truncate(2)` truncates to 2 bytes; or asserts it throws on an extra arg.
- **proposed-fix**: accept an optional `size` param (truncate to size when given, else to pos), matching Python.
- **confidence**: CONFIRMED

### A11-11: `File.seek` silently no-ops on an out-of-range large target while `BytesIO.seek` accepts it (inconsistency)
- **severity**: LOW
- **location**: `FileVal::seek` 220-246 vs `BytesIO::seek` 342-356.
- **category**: consistency
- **description**: CONFIRMED: on a 3-byte file/buffer, `seek(9223372036854775800, 2)` (no arithmetic overflow) leaves the File at position 3 with NO error (the underlying seekg past a huge offset fails, tell() clears failbit → reports 3), whereas `BytesIO.seek` sets `pos = 9223372036854775803` and tell() reports it. So an impossible seek is a silent no-op on File but succeeds logically on BytesIO. File gives no signal the seek didn't take.
- **proposed-fix**: on File, detect the failed seek (stream fail state) and throw, or clamp consistently with BytesIO; make the two surfaces agree.
- **confidence**: CONFIRMED

### A11-12: unbounded read-all can OOM (A12-3 carried over)
- **severity**: LOW
- **location**: `stdlib_io.hpp:97` (File.streamRead no-n `ss << rdbuf()`), 398 (StdStream.streamRead no-n), 603-609 (io.read).
- **category**: resource-limit (by-design CPython parity, but inconsistent with Kirito's guard policy)
- **description**: `read()` with no size buffers the entire remaining stream; `/dev/zero`/an infinite pipe → bad_alloc, not the bounded catchable error Kirito applies to string/list repetition, range, tensor, regex, BytesIO (256 MiB cap). Sized reads ARE clamped. Unchanged from v1.13.
- **proposed-fix**: cap accumulated size in the no-n branches (mirror BytesIO kMaxBuf) and throw "read result too large".
- **confidence**: HIGH (behaviour), design-judgement on severity.

## Confirmed FIXED since v1.13
- **A12-1 (walk/listdir tolerance)** — FIXED: `listdir` now iterates with `increment(ec)`, `walk` uses `directory_options::skip_permission_denied` + `increment(ec)` + `is_regular_file(sec)`; no throw on a perm-denied nested dir (P19 confirmed no throw).
- **A12-2 (writelines non-iterable → bad_optional_access)** — FIXED: `f.writelines(42)` now throws a clean catchable "type 'Integer' is not iterable" (P1b confirmed).

## Coverage gaps (behaviour plausible/tested-at-runtime but no automated test)
- **A11-13**: no test for embedded-NUL path truncation (A11-1) — highest-value gap (security).
- **A11-14**: no test for `io.open(dir,"r")` (A11-5) — should assert throw/empty-and-documented.
- **A11-15**: no test asserting `File.write`/`StdStream.write` return value (A11-6) or `BytesIO.truncate(size)` semantics (A11-10).
- **A11-16**: no test calls io stream methods by keyword (`f.read(size=)`, `f.seek(offset=,whence=)`, `b.write(data=)`) though they WORK (P24) — the uniform-kwargs guarantee is unverified for io streams (A12-9 carried).
- **A11-17**: no test for duck-typed stdout flush (A11-3), `stream=None` diagnostic (A11-4), append-mode seek/tell (A11-9), read(None) divergence (A11-7), `io.write(Bytes)` repr (A11-2).
- **A11-18**: no test for the BytesIO 256 MiB guard at the .ki level (P20 confirms it fires) or File seek-overflow no-op (A11-11).

### A11-19: `BytesIO.truncate()` after a large seek bypasses the 256 MiB write guard and EXTENDS the buffer
- **severity**: MEDIUM
- **location**: `stdlib_io.hpp:361-366` `truncate` — `b.buf.resize(b.pos)` with NO size cap; `seek` (342-356) lets `pos` grow to any non-negative int64 without allocating.
- **category**: resource-limit / guard-bypass
- **description**: `streamWrite` enforces `kMaxBuf = 256 MiB`, but `truncate()` calls `buf.resize(pos)` unguarded. CONFIRMED: `b = BytesIO(); b.seek(400000000); b.truncate()` allocates a 400 MB buffer (getvalue len == 400000000) — well past the 256 MiB cap the write path refuses. Two problems: (1) the documented BytesIO size cap is bypassed; (2) when `pos > buf.size()`, `truncate()` GROWS the buffer (fills with NUL) rather than truncating — "truncate" that extends is surprising (Python's truncate to a larger size also extends, but Kirito exposes no size arg so the only way to hit it is a seek-then-truncate, and it silently dodges the guard). An extreme `seek(9e18); truncate()` throws bad_alloc (catchable), so no crash, but any value between 256 MiB and available RAM OOMs the process silently.
- **failure-scenario**: `b.seek(n); b.truncate()` with a large n materializes an n-byte buffer, defeating the resource guard that protects every other BytesIO write path.
- **proposed-test**: `b.seek(300*1024*1024); b.truncate()` must throw "BytesIO too large".
- **proposed-fix**: apply the same `kMaxBuf` check in `truncate` before `resize` (and consider only ever shrinking, i.e. `resize(min(pos, buf.size()))`, plus an explicit optional size arg per A11-10).
- **confidence**: CONFIRMED

## Summary
Confirmed defects (all reproduced on build-debug/ki): **2 MEDIUM**, **1 LOW-MEDIUM**, **10 LOW**.
- **MEDIUM**: A11-1 (embedded-NUL path truncation → sandbox/validation bypass, write confirmed),
  A11-19 (`BytesIO.truncate()` after a large seek bypasses the 256 MiB guard and extends the buffer).
- **LOW-MEDIUM**: A11-5 (`io.open(dir,"r")` silently succeeds, read()=="", no IsADirectoryError).
- **LOW**: A11-2 (io.write/print stringify Bytes to repr), A11-3 (duck-typed stdout never flushed),
  A11-4 (opaque stream= diagnostic), A11-6 (File/StdStream.write return None vs BytesIO count),
  A11-7 (read(None) divergence), A11-8 (readline blank-vs-EOF ambiguity), A11-9 (append-mode seek/tell
  misleading), A11-10 (truncate(size) arg ignored), A11-11 (File.seek out-of-range silent no-op vs
  BytesIO), A11-12 (unbounded read-all OOM, carried).
- **Coverage gaps**: A11-13..18 (NUL path, open-dir, write-return, kwarg stream methods, duck flush /
  append seek / read(None) / io.write(Bytes), BytesIO guard + seek-overflow — none automated).
- **Fixed since v1.13**: A12-1 (walk/listdir tolerance), A12-2 (writelines non-iterable message).

Top priority: **A11-1** (NUL-byte path injection — the only security-class defect) and **A11-19**
(guard bypass, single-line fix). A11-5 (open-on-dir) is the next behavioural correctness gap.
