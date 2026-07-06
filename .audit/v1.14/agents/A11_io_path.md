# A11 — io + path + streams audit (v1.14)

Scope: `src/kirito/stdlib_io.hpp`, `src/kirito/stdlib_path.hpp`.
Prior: v1.13 A12 (merged — hunting NEW angles). READ-ONLY on src/.
Probe binary: /home/user/kiritolang.github.io/build-debug/ki

Status: IN PROGRESS.

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
