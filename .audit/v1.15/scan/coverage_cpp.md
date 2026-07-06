# v1.15 audit — C++ test coverage completeness

Subsystem: whether `tools/tests/unit/*.cpp` exercises the whole public C++ surface
(`value.hpp` Value/wrapper API, `native.hpp` extension points, `vm.hpp`/`kirito.hpp` embedding)
AND Kirito-visible behavior, from every angle (happy / edge / error / GC).

Repo root: /home/user/kiritolang.github.io. 126 unit test files.

Task = produce a GAP MAP. Do NOT write tests. Name symbol, file:line, missing angle.

## LOG
- Starting: enumerate value.hpp surface, then map tests.

## Surface mapped so far — C++ Value/embedding API (value.hpp / native.hpp / vm.hpp)
The Value facade is ~99% covered by test_value.cpp, test_value_ops.cpp, test_value_containers.cpp,
test_value_extra.cpp, test_cppref_deep.cpp, test_pinned_handle.cpp, and the r4/r7/r8 embed suites.
NativeModule/NativeClass/ModuleBuilder/makeMethod/bindArgs are well covered by test_r4/r7/r8 +
test_embedding_extra. This is a mature, multi-round-audited suite. Gaps below are the genuine holes.

## GAPS — C++ embedding API (untested or shallow)

### G1 [LOW] `NativeFunction::callKw` (direct C++ NamedArg-span call) is never tested from C++
- where: native.hpp / function.hpp (NativeFunction::callKw). test_cppref_deep.cpp:95 claims it is
  "covered separately by the r4/r5/r7/r8 embed suites" but grep of tools/tests finds ZERO call sites.
- missing angle: construct a keyword-aware variadic NativeFunction and invoke callKw(pos, named)
  directly in C++, incl. unknown/duplicate-keyword error paths. Only the *Kirito-driven* path
  (kwfn via import) is tested (test_embedding_extra collect()).

### G2 [LOW] `KiritoVM::registerModule(name, ModuleFactory)` (the std::function factory registration,
  distinct from install<T>()) is never tested.
- where: vm.hpp:203, runtime.hpp. Same cppref_deep:96 comment claims coverage; grep finds none.
- missing angle: register a module via a ModuleFactory lambda, import it, and confirm a second VM
  does not see it. Only install<T>() (NativeModule subclass) is exercised.

### G3 [LOW] `ModuleBuilder::alias` ERROR path (alias target not registered) untested
- where: native.hpp:132-137 — throws "alias target '<x>' not registered".
- missing angle: a module whose setup() calls m.alias("new","doesNotExist") must throw. Only the
  happy alias path (test_embedding_extra twice2) is covered.

### G4 [LOW] `Args::raw()` accessor untested
- where: value.hpp:861 — returns the underlying std::span<const Handle>.
- missing angle: a native that delegates to a low-layer protocol via a.raw(); zero call sites.

### G5 [LOW] `KiritoVM::setMaxStackBytes` untested; the stack-BYTES recursion guard (A04-1) has no
  direct unit test.
- where: vm.hpp:194, enterCall() stackBase_/maxStackBytes_ path (vm.hpp:176-191).
- missing angle: only setMaxCallDepth (the COUNT guard) is tested (test_r8 block 8). The distinct
  stack-usage guard — the one that fires for deep NATIVE frames (sorted(key=g) recursion) before the
  count guard — is never forced. Set a tiny maxStackBytes and drive a key-callback recursion.

### G6 [LOW] `KiritoVM::retainChunk`, `importModule` (C++ direct), `programForFile`,
  `ChunkFileScope`/`currentChunkFile`, `setLastTraceback`/`lastTraceback` (read from C++), `pinConst`,
  `protoGet`/`protoPut`/`protoTried`, `undefined()` — no direct C++ unit test.
- where: vm.hpp various. Several are internal-ish but are public embedding surface. lastTraceback is
  only exercised indirectly via sys.traceback() from Kirito, never read back through the C++ getter.

### G7 [LOW] `Value(vm, std::string_view)` ctor overload and `Value(vm, long)/(unsigned long)` not
  directly exercised (const char*/std::string/int/size_t/int64_t/ull are). Trivial but on the surface.

## FINDINGS — real defects surfaced by the coverage gap

### F1 [MED] `Value::str()` on an unbound (default-constructed) Value SEGFAULTS instead of throwing
- where: src/kirito/value.hpp:161  `std::string str() const { return vm_->stringify(h_); }`
- repro (C++): `Value empty; empty.str();` -> SIGSEGV (confirmed: null deref of vm_, exit 139).
- actual: hard crash of the host. expected: a clean KiritoError, like every OTHER accessor.
  test_value_ops.cpp:217-225 deliberately asserts unbound `typeName()/asInt()/truthy()/len()` all
  throw — but str() was omitted from that list, and str() is the one that lacks a requireBound()
  guard (kind/typeName/truthy/asX go through ref() which calls requireBound(); str() does not).
- fix idea: `requireBound(); return vm_->stringify(h_);`. Then add str() to the unbound-accessor
  CHECK_THROWS block. This is BOTH a bug and a test gap (the gap let the bug survive).

### F1 siblings (same root cause) — confirmed
- `Value::operator+` (and all binary/comparison operators via applyBinaryOp(*vm_,...)) and `Value::at()`
  (via vm_->makeInt) also SEGFAULT on an unbound Value (confirmed exit 139). `hash()` throws cleanly
  (uses ref()). So the "every accessor on an unbound Value throws" contract asserted at
  test_value_ops.cpp:217-225 is only partially real — the operator/at/str/call/setAttr paths that use
  `*vm_` directly break it. Fix: requireBound() before any `*vm_` use; extend the CHECK_THROWS block.

### G8 [MED] The `adopting`-pin on FRESH results of getAttr / call / at / pop / operators is asserted
  in comments (A19-1/A19-2) but never tested with a NON-interned result across a forced GC.
- where: value.hpp:150,288-303,591,983 (adopting), operator defs 997-1044.
- what's missing: every existing GC-stress test uses either PinnedHandle-kept callees or results that
  are small interned ints (test_pinned_handle.cpp:162-166 returns 42 — interned, survives even
  unpinned). No test forces a collection BETWEEN a getAttr/call/at/pop/operator producing a fresh
  heap object (a String, a big Integer, a list concat) and the caller's first use of it — the exact
  UAF the `adopting` pin exists to prevent. Add: setGcThreshold(1); Value r = a.call(...) returning a
  fresh String; allocate churn; then read r. Same for getAttr (a computed String attr), at() (a
  1-char String slice), pop() (a heap element), and operator+ on Strings.

## LOG
- Read value.hpp (full), native.hpp (full), vm.hpp (full). Mapped every public symbol.
- Read test_value.cpp, test_value_ops.cpp, test_value_containers.cpp, test_value_extra.cpp,
  test_cppref_deep.cpp, test_pinned_handle.cpp, test_r4/r7/r8_embed_api.cpp, test_embedding_extra.cpp.
  Value facade + NativeModule/NativeClass/makeMethod/bindArgs coverage is near-total.
- grep-mapped un-called embedding symbols: callKw, registerModule(factory), alias-error, Args::raw,
  setMaxStackBytes, retainChunk, importModule(C++), programForFile, ChunkFileScope, lastTraceback
  getter, pinConst, protoGet, undefined() -> G1-G7.
- Confirmed F1 crash (Value::str() unbound -> SIGSEGV) + siblings (operator+, at()) via compiled probes.
- Structural check: every stdlib_*.hpp module and every kimodule has a dedicated/importing test file
  (grep counts 11-22 files each). No wholesale-missing module. Core engine headers (lexer/parser/
  resolver/analyzer/bytecode/compiler/gc/arena/pool) each have named test files.
- G8: adopting-pin of fresh call/getAttr/at/pop/operator results not GC-stress-tested with a
  non-interned result.
- Ruled out: the Value facade scalar/container/operator surface — thoroughly covered incl. edge +
  error + Unicode + wraparound + zero-divisor + unbound-throw (except the F1 gaps).
