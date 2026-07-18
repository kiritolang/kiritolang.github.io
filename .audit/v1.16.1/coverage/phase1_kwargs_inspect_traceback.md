# Phase 1 — kwargs/inspect + throw/traceback coverage audit

READ-ONLY audit of every native (C++-implemented) builtin, module member, and type/class method in
`src/kirito/*.hpp`. No files were modified. Scope: `runtime.hpp` (all builtins + String/List/Dict/Set/
Integer/Float methods), every `stdlib_*.hpp`, `value.hpp`, `collections.hpp`, `class_value.hpp`,
`bytes.hpp`, `object.hpp`, `native.hpp`, the exception/VM boundary (`exceptions.hpp`, `bytecode_vm.hpp`),
and the compat layers (`proc_compat.hpp`, `net_compat.hpp`, `deflate.hpp`, `regex_engine.hpp`).

## Mechanism (established, for reference)

- **kwargs + inspect** require a real parameter-name list at registration: `defSig(name,{params},ret,fn)`
  (global), `builder.fn(name,{sig},ret,fn)` (module member), `makeMethod(vm,name,{params},impl,...)`
  (type method). A bare `def(name,fn)`, a 2-arg `builder.fn(name,impl)`, `makeMethod(...,{},...)`, or a
  per-module `bind`/`kwfn` helper without a param list gives positional-only + minimal inspect.
- **Span/traceback attachment.** `bytecode_vm.hpp::located(in.span, fn)` (line 504) wraps every call /
  operator / member dispatch and, on a `KiritoError` with `span.line == 0`, stamps it with the current
  instruction's span (508). So **`throw KiritoError("msg")` with no span is correct by design** — it gets
  the call-site span and the unwinding frames build the traceback (`appendFrame`, 531). At top level an
  uncaught `KiritoError` prints `where:line:col: error: <what>` + traceback (`main.cpp:266-267`).
- **The gap in the boundary.** `located()` only re-spans `KiritoError`. A **non-`KiritoError`
  `std::exception`** (a custom `DeflateError`/`ProcError`/`RegexError`/`TensorError`, or a raw `std::…`)
  is caught only by the run loop's generic arm (`bytecode_vm.hpp:469-478`), which unwinds it as a String
  with an **empty `SourceSpan{}`** (477). Consequence:
  - caught by a Kirito `try/catch` → stringified fine, a fresh single-frame traceback is exposed; but
  - **uncaught** → re-thrown raw, escapes `evalIn` (its inner try only catches `KiritoError`/`KiritoThrow`,
    `runtime.hpp:4136-4147`), and reaches `main.cpp:270-273` → **`file: error: <what>` with NO line:col
    and NO traceback**;
  - **reraised** inside Kirito → `excSpan_` is `{}`, so the reraise reports no site line.
  This is exactly why every custom C++ exception must be translated to `KiritoError` at its native
  boundary. The issues below are the sites that miss that translation.

---

## kwargs/inspect GAPS

**No real (functional) gaps.** Every fixed-arity native that reads positional args (`a[0]`, `a[1]`, …)
across all files is registered with a matching parameter-name list, so it accepts keyword arguments and
describes itself under `inspect`. All `{}`-signature registrations were checked against their impls and
are genuinely 0-arity or genuinely variadic (see Acceptable-by-design). No signature/impl arity mismatch
(a declared `{"a","b"}` whose impl reads `a[2]`, or vice-versa) was found anywhere.

Two **cosmetic** notes (not functional gaps — the impls read the span directly and self-guard arity):

- `stdlib_io.hpp:237` (`FileVal.seek`) and `stdlib_io.hpp:359` (`BytesIO.seek`) declare
  `{"offset","whence"}` with **no default on `whence`**, but the impls treat `whence` as optional
  (`a.size() > 1 ? … : 0`). `inspect`/kwargs therefore present `whence` as required though it isn't.
  Contrast `io.open` (`stdlib_io.hpp:642`), which correctly gives `mode` a default in its param spec.
- `stdlib_regex.hpp:90` (`Match.group`) declares `{"index"}` but the impl is variadic (no arg → whole
  match; one arg → one group; N args → a List). The single declared name under-describes the multi-key
  form. Harmless; matches its `inspectMembers` string.

---

## throw/traceback ISSUES

### 1. (REALISTIC) Match-time `RegexError` escapes untranslated — `stdlib_regex.hpp`
`reng::RegexError` (a `std::runtime_error`, `regex_engine.hpp:27`) is thrown **at match time**, not only
at compile time: `regex_engine.hpp:619-620` raises *"regex match exceeded its complexity budget…"* when the
Pike-VM capture-volume budget (`kMaxMatchWork`) is exceeded. Every `RegexVal` matching method calls
`reng::run` with **no** `catch (reng::RegexError)` translation:
- `stdlib_regex.hpp:280` — `match` / `search` / `fullmatch` (direct `reng::run`).
- `stdlib_regex.hpp:158` — `redetail::allMatches`, used by `finditer` (293), `findall` (306), `sub` (338),
  `split` (367), and the module one-shots (`oneShot` 461, `oneShotExtra` 471, module `sub` 439).

Only `compileRegex` (`stdlib_regex.hpp:453-459`) wraps `RegexError → KiritoError`, i.e. **compile errors
only**. A pathological pattern-over-input that trips the budget at runtime therefore surfaces to a Kirito
program as a raw `std::runtime_error` — degraded top-level diagnostic, lost span on reraise. The engine's
own comment (`regex_engine.hpp:602`, "throws a clean, catchable RegexError") is true only for C++, not for
Kirito `catch as e`. Fix: mirror `compileRegex` — wrap the `reng::run`/`allMatches` calls (or each method
body) in `catch (const reng::RegexError& e){ throw KiritoError(e.what()); }`.

### 2. (REALISTIC) Tensor `_setstate_` — raw `TensorError` on a corrupt dump — `stdlib_tensor.hpp:2096, 2100`
`_setstate_` reads `shape` (from `items[1]`) and `data` (from `items[2]`) independently, then builds the
tensor: `t.store = TensorVal::CT(shape, std::move(data))` (2096) / `FT(...)` (2100). `tns::checkSize(shape)`
(2088) validates only the shape; the **`Tensor` ctor** (`tensor.hpp:107-110`) throws raw
`tensor::TensorError("tensor data size does not match its shape")` when `data.size() != numel(shape)`. This
construction is **not** inside `tns::wrap` and has no `try/catch`, so a corrupt/hand-crafted serialized blob
(`serde.loads` / deserialize of a malformed tensor) escapes as a raw `std::runtime_error` — no
file:line:col/traceback. The siblings guard exactly this with an explicit KiritoError pre-check:
`stdlib_matrix.hpp:317` (`if (data.size() != r*c) throw KiritoError(...)`) and `stdlib_complex.hpp:503`.

### 3. (EXTREME-INPUT) `matrix.vector` / `complex.vector` — raw `TensorError` — `stdlib_matrix.hpp:389`, `stdlib_complex.hpp:663`
`return vm.alloc(mat::fromTensor(tensor::Tensor<double>(tensor::Shape{1, n}, std::move(xs))))` — the
`tensor::Tensor` is built as the **argument** to `fromTensor`, so its `checkedNumel` runs *before*
`fromTensor`'s own KiritoError cap check. For `n` above the engine cap (`kMaxElems` = 64M, `tensor.hpp:65`)
the ctor throws raw `tensor::TensorError("Tensor too large")` outside any wrapper. Requires a >64M-element
input list; the `mat::make`-based creators (`zeros`/`ones`/`identity`/`Matrix`) are guarded — only `vector`
bypasses the guarded path.

### 4. (LOW-CONFIDENCE) Tensor `backward` / autograd not behind `tns::wrap` — `stdlib_tensor.hpp:2048`
`tns::runBackward` and its stored backward closures invoke engine ops (`tensor::add`/`reshape`/`matmul`/
`transposeLast2` — the last throws `TensorError` at `stdlib_tensor.hpp:293`) and are **not** wrapped in
`tns::wrap`, unlike every other tensor method. A shape-inconsistent graph would let a raw `TensorError`
escape. A correctly-built graph keeps gradient shapes consistent, so it shouldn't fire in practice — but
it is the one autograd entry point not behind the translating wrapper.

### 5. (CONSISTENCY-ONLY) `matrix.transpose` unwrapped — `stdlib_matrix.hpp:218`
`tensor::transpose` (→ `permute`, throws `TensorError`) is called with no `try/catch`, unlike the sibling
`determinant`/`inverse` in the same class which wrap. For a rank-2 `MatrixVal`, transpose cannot actually
throw, so it is unreachable in practice — flag as a consistency gap only.

### 6. (MINOR / message quality) Thin "what/where" on file-open failures
`stdlib_serialize.hpp:240, 246` (`serialize.save`/`load`) and `stdlib_dump.hpp:227, 233` (`dump.save`/
`load`) throw `KiritoError("could not open file for saving/loading")` — **omitting the path and the OS
reason** (`ec.message()`), unlike `stdlib_path.hpp` which includes both. Still `KiritoError` (file:line:col
+ traceback intact); just less context than the rest of the codebase's diagnostics.

---

## Acceptable-by-design (audited, no action)

**Local `std::` throws that never escape (translated in place):**
- `runtime.hpp:3465, 3475, 3480, 3490, 3518, 3522` — `throw std::invalid_argument(...)` in the
  `Integer("…")` / `Float("…")` String-parse paths, plus `std::stoull`/`parseDouble`, are all inside a
  local `try { … } catch (...) { throw KiritoError("cannot convert String to …: '…'"); }`
  (`runtime.hpp:3450/3492`, `3511/3524`). `runtime.hpp:1453-1454` (`String.format` `std::stoull`) is
  wrapped `catch(std::out_of_range)→KiritoError`, and `spec` is pre-validated all-digit.
- `common.hpp:83, 85` (`parseDouble` throwing `std::invalid_argument`/`std::out_of_range`) — every caller
  guards it: `stdlib_json.hpp:204-215` (translates via `fail()`), `stdlib_serialize.hpp:120` (under the
  `loads` catch-all), `runtime.hpp:3520` (under the Float-parse catch). Parser use is compile-time.
- `stdlib_regex.hpp:201` (`std::stoi` for `\g<NNN>`) and `stdlib_regex.hpp:202` `catch(std::exception)→
  KiritoError` — the replacement path is translated (only the **match-time** budget path, issue #1, is not).

**Custom-exception translation that IS complete:**
- `DeflateError` — `stdlib_zlib.hpp:32-34` and `stdlib_gzip.hpp:108-111` translate all codec paths; in net,
  `decodeBody`/`gunzip` (`stdlib_net.hpp:510-535`) are under `parseRaw`'s `catch (...)` at
  `stdlib_net.hpp:587` (a deliberate, external-data-tolerant swallow — legitimate under the external-failure
  exemption, though it drops the diagnostic for a genuinely corrupt `Content-Encoding`).
- `ProcError` — the only `proccompat::run(...)` call reachable from a native (`stdlib_sys.hpp:63`) is inside
  `try { … } catch (const proccompat::ProcError& e) { throw KiritoError(…); }` (`stdlib_sys.hpp:62-66`);
  every `throw ProcError` in `proc_compat.hpp` fires only within `run()`.
- `TensorError` — single-sourced translation via `tns::wrap` (`stdlib_tensor.hpp:218`), `tns::make`/
  `tns::checkSize` (201, 212), and the `catch(const tensor::TensorError&)→KiritoError` wrappers in
  `MatrixVal::binary/determinant/inverse` (`stdlib_matrix.hpp:134, 224, 230`) and `ComplexMatrixVal::binary`/
  `cpx::determinant`/`inverse` (`stdlib_complex.hpp:281, 287, 311). Covers all determinant/inverse/matmul/
  solve/reduction/creation/binary/method paths — the four sites in issues #2-#5 are the only leaks.
- `net_compat.hpp` never throws (all shims return error codes/bools; net code checks and throws
  `KiritoError`). The `httpExchange` `catch (...) { …cleanup…; throw; }` blocks (`stdlib_net.hpp:696, 702,
  718, 734`) release fd/SSL and re-throw the original `KiritoError` unchanged — preserve, not swallow.
- JSON number overflow (`stdlib_json.hpp:206-215`) and `serialize.loads`/`dump.read`
  (`stdlib_serialize.hpp:215-222`, `stdlib_dump.hpp:196-200`) translate every `std::exception` (incl. the
  bare `"stoi"/"stol"/…` messages) into a readable `KiritoError`.

**`throw KiritoError("msg")` with no span (across all files):** correct by design — `located()` stamps the
call-site span and the frames build the traceback. Messages carry a specific condition + context (offending
value/type, expected-vs-actual); no bare generic diagnostics of note (the only truly generic strings, e.g.
`runtime.hpp:302` "unsupported numeric operator", are unreachable fall-throughs guarding future opcodes).

**Signature-less registrations that are legitimate (variadic / 0-arity / special form):**
- Global `def(zip)` (`runtime.hpp:3851`) and the raw `NativeFnKw` `range` (3658), `min` (3810), `max` (3813)
  — genuinely variadic; no fixed parameter names possible; inspect shows `(...)`.
- `String.format` (`runtime.hpp:1436`, `makeMethod(…,{},…)`) — variadic positional substitution driven by
  the template; advertised as `format(...) -> String` in `inspectMembers`.
- `io.print`/`eprint`/`input`/`read`/`write` (`stdlib_io.hpp:590-641`) and `parallel.spawn`
  (`stdlib_parallel.hpp:542`) — `m.kwfn` (variadic + kwargs). `path.join` (`stdlib_path.hpp:96`, 2-arg
  `m.fn`) — genuinely variadic (`join(parts…)`). `tensor.einsum` (2527, `spec, *tensors`), `tensor.arange`
  (2273) and `matrix.Matrix` (333) — variadic via `m.kwfn`.
- The no-params `bind` helper at `stdlib_complex.hpp:148` `(const char* nm, NativeFn fn)` registers only
  genuinely 0-arity self-reductions (`conjugate`, `modulus`/`abs`, `argument`/`phase`, `norm2`, `is_zero`,
  `_getstate_`) that read no `a[i]`; `compare`/`_setstate_` use the params-carrying paths. No signature lost.
- All other `{}`-signature methods verified 0-arity (read no `a[i]`): `bytes.hex`/`_getstate_`;
  `Integer.isprime`/`bitlength`/`toint`/`_getstate_`; List `reverse`/`copy`/`clear`; Dict `keys`/`values`/
  `items`/`copy`/`clear`/`popitem`; Set `copy`/`clear`/`pop`; String `upper`/`lower`/`isdigit`/…; DateTime
  `iso`/`isoformat`/`_getstate_`; the io stream 0-arg methods (`readline`/`close`/`flush`/`tell`/`getvalue`/
  `size`/`truncate`/`_enter_`/`_exit_`); net/parallel 0-arg methods (`accept`/`recvall`/`close`/`fileno`/
  `getsockname`/`cipher`/`getnowait`/`qsize`/`join`/…); tensor `item`/`tolist`/`shape`/`detach`/`transpose`/
  the element-wise math ops (2125) / `NoGradCtx._enter_`/`_exit_`.

**`stdlib_kimodules.hpp` — embedded Kirito source, out of both audits.** The module is a set of
`R"KI(…)"` string literals (itertools, functools, collections, statistics, string, textwrap, base64, csv,
heapq, bisect, …) evaluated as Kirito code. Its functions are Kirito `def`s (inherent kwargs + inspect), and
its `throw "…"` statements are **Kirito-level** throws that carry the `Op::Throw` span (file:line:col)
automatically. No C++ native registration and no C++ exception. Acceptable for both questions.

**Compile-time and Kirito-throw paths (spans built in directly):** lexer/parser/compiler/resolver
`KiritoError`s carry real source spans; `Op::Throw` (`bytecode_vm.hpp:446`) and `Op::Reraise` (439) attach
`in.span`/`excSpan_` + depth to a Kirito `throw <value>`.
