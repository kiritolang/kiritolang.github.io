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
