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
