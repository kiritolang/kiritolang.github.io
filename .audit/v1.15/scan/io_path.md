# v1.15 audit — io and path subsystem

Source: `src/kirito/stdlib_io.hpp`, `src/kirito/stdlib_path.hpp`. Probe: `./build-debug/ki`.

## LOG
- Read both source files fully.
- Starting probes under path.fasttemp() unique subdir.

## FINDINGS

### F1 [MED] File.seek and BytesIO.seek disagree on a negative target; source comment is wrong
- where: src/kirito/stdlib_io.hpp:240-242 (File.seek) vs :351-354 (BytesIO.seek)
- repro:
```
var io = import("io")
var b = io.BytesIO("abc")
io.print(b.seek(-5))          # => 0  (silently clamps)
# vs a File:
var f = io.open("/dev/shm/kiaud_x","w"); f.close()
var g = io.open("/dev/shm/kiaud_x","r")
io.print(g.seek(-5))          # THROW: seek: resulting position is negative
```
- actual: `File.seek(-5)` throws "resulting position is negative"; `BytesIO.seek(-5)` silently clamps to 0 (`if (np < 0) np = 0;`). The File.seek comment at :240-242 explicitly claims "reject it (BytesIO::seek guards the same way)" — but BytesIO does NOT reject, it clamps. Two stream types implementing the same `seek` protocol give opposite results on the same negative input, and the comment misdescribes the sibling.
- expected: consistent behavior across the interchangeable stream types (both throw, or both clamp) and a correct comment.
- fix idea: make BytesIO.seek throw on a negative resulting position too (matching File), and fix/remove the misleading comment; or if clamping is intended, make both clamp and correct the comment.

### F2 [LOW] Opening a directory in read mode succeeds silently (reads yield "")
- where: src/kirito/stdlib_io.hpp:623-651 (open) — no directory guard
- repro:
```
var io = import("io"); var path = import("path")
var d = path.gettempdir()
var f = io.open(d, "r")       # => <File> (no error)
io.print(repr(f.read()))      # => ""  (silent)
```
- actual: `io.open(<dir>, "r")` returns a File and `read()` returns "" (glibc fstream opens a directory for input). Opening a dir for write DOES throw. Python raises IsADirectoryError.
- expected: opening a directory as a file should raise (a case that should error but silently succeeds).
- fix idea: after open, reject when `std::filesystem::is_directory(path)` (or check the stream after a probe read).

### F3 [LOW] BytesIO cannot be constructed from a Bytes value (only String)
- where: src/kirito/stdlib_io.hpp:562-568 (BytesIO ctor) — accepts only ValueKind::String
- repro:
```
var io = import("io")
io.print(io.BytesIO(Bytes([1,2,3])))   # THROW: BytesIO expects a byte String
```
- actual: `BytesIO(Bytes(...))` throws; only a String initial is accepted. Inconsistent with the rest of the binary-I/O surface — BytesIO.write() and File(binary).write() both accept String OR Bytes (via ioRawBytes). Bytes is THE binary type, yet you cannot seed the in-memory binary buffer with one.
- expected: `BytesIO(Bytes)` should build the buffer from the raw bytes.
- fix idea: in the ctor, accept a BytesVal too (use ioRawBytes-style extraction) instead of only StrVal.

## LOG (examined)
- Binary round-trip wb/rb byte-exact (Bytes[0,1,2,255,10,13,0,65]) => OK.
- Unicode text write/read; len() code-point count => OK (16 cp).
- seek/tell: set/cur/end, past-end (returns "", tell reports the seeked pos), negative-abs throws => OK (except F1).
- readline/readlines/iteration: no trailing newline, EOF empty, CRLF retains \r (binary-internal, by design), embedded NUL preserved => OK.
- BytesIO cursor, overwrite-at-cursor, gap-fill with \x00, truncate-at-cursor, size/len, String-returning reads => OK.
- Resource guards: huge read clamps (no OOM), BytesIO 256MB cap on write/truncate after big seek, seek overflow throws => OK.
- open modes: missing-r throws, w/a/r+/wb/ab/rb/r+b OK, rb+ rejected (documented), empty/x/rw rejected, bad-parent throws, dir-w throws => OK.
- write non-String/Bytes (int/list) throws; read on write-only & write on read-only throw => OK.
- r+ interleaved read/write correct; append tell correct; read-all after EOF returns "" => OK.
- Redirection: io.stdout=BytesIO/duck-Sink, stream= keyword, duck readline for input, unexpected-keyword rejected => OK.
- use-after-close throws cleanly (no UAF); mid-iteration close throws; double-close no-op => OK.
- path: join (zero-arg throw, abs reset, leading-backslash not abs, trailing slash, empty parts), dirname/basename/splitext (dotfiles/multi-dot/no-ext/dir-with-dot) all match os.path => OK.
- path queries tolerance (missing->False/[]), getsize throws on missing & on dir, listdir/walk nested => OK.
- path mutation: mkdir exist_ok, remove/rmtree missing_ok + return contracts, remove nonempty throws, rename None, chmod Bool(missing->False) => OK.
- NUL rejected in filesystem ops (exists/open) but not in pure string join (documented) => OK.
- with-context manager for File => OK.
