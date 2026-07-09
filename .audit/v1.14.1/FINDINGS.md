# Audit v1.14.1 — triaged findings

All 16 scanners (A01–A16) complete. Each raw report is in `scan/AXX.md`. This is the triaged roll-up.
Verdicts: **FIX** (real, triggerable — patch + regression test), **DOC** (doc-only), **TEST** (coverage
gap — add test), **WONTFIX** (by-design / false positive), **CLEANUP** (dead code / cosmetic).

Overall: the codebase is very hard after prior rounds. Engine (lexer/parser/resolver/compiler/VM/
runtime/collections/numeric) is clean of correctness bugs. The real yield is a **GC-rooting cluster** in
native-module setup + the C++ embedding API (confirmed under ASan), plus one parser mis-parse and a
handful of stdlib edge/hardening fixes.

## External blocker (from user's full-suite run)
- **BUILD-1 (release build fails)** — `-Werror=array-bounds` in `char_traits.h:435` (`__builtin_memcpy`
  offset [32,108] out of [0,32] of a `std::string`), 3 occurrences, GCC 13 `-O2`/`_FORTIFY_SOURCE=3`.
  Debug (‑O0) is green (806 tests). NEW failure → suspect new 1.14.0 string-heavy code. **FIX (top
  priority).** Locating exact TU via full release build.

## GC-rooting cluster (HIGH value — confirmed under ASan) — all "unrooted handle across an allocation"
- **A06-2 (HIGH) FIX** — `IoModule::setup()` (`stdlib_io.hpp:~535`) holds `stdout`/`stderr`/`stdin`
  StdStream handles in C++ locals across further allocations before storing them into `members`; a GC
  during setup sweeps them → later `io.print/write/eprint` deref a dangling handle. Reproduced 100% at
  `setGcThreshold(1)` and naturally at thresholds 3–32. **Root cause of A04-X1.** Fix: `RootScope` the
  three handles across setup.
- **A04-X1 (HIGH) FIX (same class)** — under `setGcThreshold(1)`, `io.print/write`, `io.BytesIO()`
  (setup default arg) and `Random.*` throw "dangling handle". io covered by A06-2; verify Random setup
  default-arg + BytesIO default-arg rooting.
- **A09-1 (MED, confirmed) FIX** — `ModuleBuilder::fn` signatured overload (`native.hpp:~117`) lets a
  heap-allocated param *default* (e.g. `hash.hmac`'s `"sha256"`) be swept during the `NativeFunction`
  alloc; a later kw-omitted call derefs it. Fix: `RootScope` the sig defaults across the alloc.
- **A09-3 (MED, confirmed) FIX** — `Value::call(initializer_list)` (`value.hpp:~292`) materializes
  arg handles without rooting → swept in callee prologue. Fix: `RootScope` the handle vector.
- **A09-2 (MED, confirmed) FIX** — `Value::isBytes()/asBytes()` (`value.hpp:~170`) discriminate Bytes
  by `typeName()=="Bytes"` string compare → a user `class Bytes` is `static_cast` to `BytesVal` → SEGV.
  Fix: virtual `isBytesVal()` discriminator on `Object`/`BytesVal`.
- **A06-1 (MED, confirmed) FIX** — `_activeVM` thread-local (`vm.hpp:303`, `runtime.hpp:3542`) dangles
  on non-LIFO multi-VM teardown → UAF, reachable via the embedding "multiple VMs" goal. Fix: set
  `_activeVM` via an RAII scope at each run entry, not tied to ctor/dtor lifetime.

## Parser / semantics
- **A02-1 (MED) FIX** — expression continuation glues across a block-bodied function literal's
  terminating dedent: `blockJustClosed_` is honored only in `parseConditional`, so `(5)`, `, 2`, `.foo`,
  `[0]`, `- 2` on the line after `var f = Function(): <block>` get silently swallowed onto the function
  value. Fix: short-circuit each continuation loop (`parsePostfix`/`parseAdd`/…/`parseValueSeq`) on
  `blockJustClosed_`, mirroring the existing guard. (Bracketed fns stay inline — newlines suppressed in
  `()[]{}` — so the flag never leaks out of brackets; verified safe.)
- **A07-1 (LOW) FIX** — `"{0} {}".format("a","b")` yields `"a a"` (auto/manual field numbering not
  coordinated) where Python raises. Fix: track mixed auto+manual and throw. Add test.

## Networking
- **A13-1/A13-2 (MED) FIX** — HTTP `Host` header omits a non-default port and unbrackets IPv6 literals
  (`stdlib_net.hpp:~933`; parseUrl stores IPv6 unbracketed). Verified: `get("http://127.0.0.1:8137/…")`
  sent `Host: 127.0.0.1`. Fix: include `:port` when non-default; bracket IPv6.
- **A13-4 (MED) FIX/DOC** — `KiritoDispatcher::shutdown()` unconditionally `join()`s workers; a worker
  blocked in a bare syscall (`recv`/`accept`/`io.input`/`sys.shell` w/o timeout) never unwinds → hang at
  exit (reproduced exit 124). Contradicts the documented "unwinds anywhere" invariant. Fix: timed-join +
  hard-exit fallback (sys.exit already uses `_Exit`), and/or narrow the doc claim.
- **A13-6 (LOW-MED) FIX** — multipart `files` field-name/filename concatenated raw into part headers
  (unlike the header/cookie CRLF hardening) → injectable. Fix: sanitize/reject CR/LF in field name +
  filename.
- **A13-3 (LOW) FIX** — protocol-relative redirect `Location: //host/path` mis-resolved to
  `scheme://orig//host/path`. Fix: handle `//` as scheme-relative.
- **A13-5 (LOW) NOTE** — cross-VM primitives never reclaimed; largely forced by cross-VM-by-identity.
  Note only.

## io / path / sys / time
- **A10-1 (MED) FIX** — `FileVal::streamRead` (`stdlib_io.hpp:95`) huge-`read(n)` guard defeated for
  non-seekable files (FIFO/device/`/proc`): `tellg()==-1` skips the clamp → `std::string(n,'\0')`
  allocates n → `bad_alloc`. Fix: chunked read when size is unmeasurable (like StdStream); fix comment.
- **A10-2 (LOW) FIX** — `File.read(None)` throws while `BytesIO.read(None)` reads-all. Unify None.
- **A10-5 (LOW) FIX** — `path.mkdir(exist_ok=True)` over an existing **file** returns False (masks that
  the path is a non-directory); Python raises. Consider raising when the existing path isn't a dir.
- **A10-3 (LOW) FIX** — `path.join` accepts an embedded NUL while dirname/basename/splitext reject it;
  comment claims all four skip the check. Unify + fix comment.
- **A10-4/A10-6 (LOW)** — `stream=None` → obscure `'None' has no attribute 'write'`; Windows-only DWORD
  overflow on very large `timeout*1000` (UB; POSIX unaffected). Fix messages/guard.

## Numeric / tensor
- **A11-1 (LOW) FIX** — tensor two-arg (data) ctor calls `numel` not `checkedNumel`, so engine
  `reshape`/`flatten` bypass the rank cap (`tensor.hpp:107`); the comment at :52 wrongly claims coverage.
  Not reachable from Kirito (all Kirito allocs route through `checkedNumel`) but a latent stack-overflow
  for direct C++ engine users. Fix: route the 2-arg ctor through `checkedNumel`; fix comment (DRY).

## Serialization / compression / crypto / bigint
- **A12-1 (LOW) FIX** — `deflate::compress` uses `int` for LZ77 window positions; >2 GiB input wraps
  `static_cast<int>(i)` negative → latent OOB in `prev[]`/`in[]` (inflate is size-capped; compress is
  not). Fix: widen to `int64_t` or add a size guard.
- **A12-2 (LOW) FIX** — `aesdecrypt` accepts a truncated 1..16-byte tag while encrypt emits 16 →
  ~1/256 forgery resistance if a caller verifies a short tag. Fix: require 16 bytes.
- **A12-3 (INFO) WONTFIX** — BigInt `/` of huge equal operands → `nan` (`inf/inf`); consistent with
  documented lossy true-division. Optional doc note. → add to false-positive table.

## Frozen ki modules
- **A14-2 (MED) FIX** — `base64.decode` throws on embedded whitespace/newlines → real-world line-wrapped
  MIME/PEM/HTTP base64 fails. Fix: skip ASCII whitespace in the decode loop. Add test.
- **A14-1 (RETRACTED) WONTFIX** — `statistics.quantiles` extreme-clamp is deliberately pinned
  (`verify_statistics.ki:64`, `r8_kimods_a.ki:235`). → add to false-positive table + doc note.
- **A14-3..7 (LOW)** — textwrap newline normalization; deque `maxlen`; `copy.copy` deep-for-instances;
  tabular Series positional alignment; `Counter.mostcommon` tie order. Mostly documented design; assess
  individually, likely DOC/NOTE.

## Docs
- **A15-1 (doc-critical) DOC** — `02-language-guide.md` claims f-string unknown escapes are lenient
  (`f"\q"`→`"q"`); code throws `invalid escape` (deliberate, `parser.hpp:997`). Delete the parenthetical.
- **A15-2 (doc-critical) DOC** — `11-cpp-api.md` §6 greet example uses `asString("name")` (returns the
  `String` wrapper, won't compile with `const char* +`); should be `asStringRef("name")`.
- **A07-2 (DOC)** — document that `\xHH` writes a raw byte (byte-transparent String), not a code point.

## Lexer / analyzer / collections cleanup
- **A01-1 CLEANUP** — dead `\r` branch (`lexer.hpp:78`) unreachable after `normalizeNewlines`.
- **A01-2 (LOW)** — `tokenize()` not idempotent (resets `indent_` but not `pos_`/`line_`/…). Doc single-
  use or reset all.
- **A08-1 CLEANUP** — `ValueKind::Array` enumerated + described in CLAUDE.md but no class produces it;
  guard branches dead. Remove or comment; update CLAUDE.md.
- **A03-1 (LOW)** — resolver/analyzer/CaptureScan hand-roll parallel `dynamic_cast` chains; a future AST
  node is silently skipped (capture-sensitive). Add an exhaustiveness guard.
- **A03-2/A03-3 (LOW)** — dup-param warning span points at `Function` kw; `declare()` overwrite can drop
  an earlier unused-var lint. Cosmetic.

## Coverage (A16) — 27 gaps, add tests (highest priority: new 1.14.0 surface)
Top: BigInt reflected-op invariant right-side (A16-13); json Inf/NaN round-trip (A16-19); time.make
out-of-range normalization (A16-22); crypto tag-length guard (A16-1); BigInt `**` negative exp → Float
(A16-14); BigInt unary neg (A16-11); BigInt `.isprime()`/`.isprobableprime()` method forms (A16-12);
crypto kwargs-uniformity (A16-5); crypto unknown-`algo` rejection (A16-2); crypto EC non-sha256 + wrong
key (A16-3). Full list in `scan/A16.md`.
</content>
