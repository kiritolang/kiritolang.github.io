# A15 C++ API

Status: IN PROGRESS -> COMPLETE.

Scope: `src/kirito/value.hpp` (Value/Bool/Integer/Float/String/Bytes/List/Dict/Set wrappers, Args,
PinnedHandle, RootScope usage), `src/kirito/native.hpp` (ModuleBuilder, NativeModule, NativeClass,
makeMethod, argString/argInt/requireNoNulPath/requireArgs/sliceIndices), plus the barrier/rooting
machinery they depend on (`vm.hpp`, `collections.hpp`, `module.hpp`, `class_value.hpp`,
`environment.hpp`, `function.hpp`, `bytes.hpp`, `object.hpp`, `pool.hpp`), and coverage in
test_r4_cpp_api.cpp, test_r7_embed_api.cpp, test_r8_embed_api.cpp, test_cppref_deep.cpp,
test_pinned_handle.cpp, test_value_extra.cpp, test_embedding_extra.cpp, test_protocol.cpp,
test_gc_generational.cpp, and docs/pages/11-cpp-api.md.

Read `.audit/README.md`'s false-positive table first. The one item scoped to this subsystem —
"v1.12.1: value.hpp comparison UB (MED, fixed)" — was verified: `Value::operator==/!=/<//<=/>/>=`
(value.hpp:1025-1066) now consume the `applyBinaryOp` result via `.truthy()`, not a
`static_cast<BoolVal&>`, with an inline comment at every one of the six operators explaining why (a
user `_eq_`/`_lt_`/... may legally return a non-Bool, and the old cast type-confused that case — UB).
Confirmed still fixed; not re-litigated.

## Method

1. Read value.hpp end-to-end (1070 lines) and native.hpp end-to-end (221 lines).
2. Traced every wrapper mutator (`List::push/set/pop/clear`, `Dict::set/remove/clear`, `Set::add/
   discard/clear`, `Value::setAttr`) down to its underlying `ListVal::append/setElem`,
   `DictVal::set/remove`, `SetVal::add`, `InstanceValue::setAttr`, `ModuleValue::setAttr/setMember`,
   `EnvValue::define`/rebind — confirming each calls `gcWriteBarrier` before/around the raw
   `std::vector`/bucket write (runtime.hpp:64-74 defines the barrier: a no-op if the container is
   itself young or already remembered; otherwise enrolls the container in the remembered set iff the
   incoming value is young).
3. Traced `PinnedHandle` copy/move/copy-assign/move-assign/self-assign/reset for refcount balance
   against `KiritoVM::pinHandle`/`unpinHandle` (a `Handle -> count` map, vm.hpp:121-126), and
   `forEachRoot` (vm.hpp:149-166) to confirm `pinnedRoots_` and `auxRoots_` are scanned by BOTH
   `collectGarbage` (major) and `minorCollect` (minor) — they are: both call the same
   `forEachRoot`.
4. Cross-checked `RootScope`'s LIFO contract (mark/truncate, vm.hpp:513-528) — correctness depends on
   C++ scope-exit ordering (reverse of construction), which is guaranteed as long as no RootScope
   escapes its lexical scope; nothing in value.hpp/native.hpp does that.
5. Read every existing embedding test file for what's covered, then hunted for the specific
   generational-crossing scenario (build container -> promote via minor -> mutate via the WRAPPER ->
   minor again -> read back) and for PinnedHandle/Args/ModuleBuilder edge cases.
6. Skimmed docs/pages/11-cpp-api.md's ~2100 lines against the current signatures (constructor lists,
   `PinnedHandle`, `RootScope`, the rooting-discipline table) for drift; found none.

## Findings

No bugs found. This subsystem has clearly been through several prior audit rounds (A19-1/A19-2/A07-4/
A09-3/A08-4 fix-comments are inline throughout value.hpp and runtime.hpp) and the generational-GC
migration was done carefully: every wrapper mutator that writes a Handle into a container goes through
the exact same barriered low-layer primitive the bytecode VM uses (no separate/duplicate write path
that could drift out of sync), so there is no parallel "the wrapper forgot the barrier" surface to
regress. Findings below are coverage gaps and one very-low-severity observation, not defects.

### A15-1: No direct C++ test that `Dict::set`/`Set::add` (the value.hpp WRAPPER methods, as opposed to
the underlying `DictVal::set`/`SetVal::add`) survive an old-container-gains-young-value minor GC
[LOW] [high confidence — verified absent]
- **Location**: tools/tests/unit/test_gc_generational.cpp:105-114 (existing coverage), vs.
  src/kirito/value.hpp:687-703 (`Dict::set`), :789-796 (`Set::add`)
- **What**: `test_gc_generational.cpp` has exactly the adversarial pattern this round was asked to
  hunt for — `List xs(vm, {}); vm.minorCollect(); xs.push(Value(vm, 424242)); vm.minorCollect(); ...
  CHECK(xs[0].asInt() == 424242);` — but only for `List::push`. There is no analogous block using the
  `Dict` or `Set` wrapper types (`Dict d(vm); vm.minorCollect(); d.set("k", Value(vm, BIG));
  vm.minorCollect(); CHECK(d["k"].asInt() == BIG);` and the `Set::add` equivalent). The underlying
  mutators (`DictVal::set`, `SetVal::add`) ARE exercised crossing generations, but only indirectly via
  a Kirito-source `d[i] = i` / set-literal path in the same file's "instance/module/env" block
  (:82-102), which drives the identical `gcWriteBarrier` call through bytecode `SetItem`/`Add`, not
  through the value.hpp wrapper call site. Since the wrapper is a thin pass-through to the same
  mutator, this is very unlikely to hide a real bug, but it is the one gap in an otherwise
  systematically covered area, and it's cheap to close.
- **Repro**: COVERAGE GAP — add to test_gc_generational.cpp:
  ```cpp
  {
      KiritoVM vm; vm.setGcEnabled(false);
      Dict d(vm); vm.minorCollect();                 // d old
      d.set("k", Value(vm, 424242)); vm.minorCollect(); vm.minorCollect();
      CHECK(d["k"].asInt() == 424242);
      Set s(vm); vm.minorCollect();                  // s old
      s.add(Value(vm, 424243)); vm.minorCollect(); vm.minorCollect();
      CHECK(s.contains(Value(vm, 424243)));
  }
  ```
- **Proposed fix**: N/A (no code change — test-only).
- **Proposed test**: as above.

### A15-2: No C++ test drives `List::set` (in-place overwrite, `setElem`) or `Value::setAttr`
(`InstanceValue`/`ModuleValue` writes) across a minor-GC generation boundary via the wrapper
[LOW] [medium confidence]
- **Location**: src/kirito/value.hpp:579-585 (`List::set`), :307-309 (`Value::setAttr`)
- **What**: Same shape of gap as A15-1 — `setElem` and `InstanceValue::setAttr`/`ModuleValue::setAttr`
  are barriered correctly (confirmed by reading) and exercised crossing generations through Kirito
  source (`a.bump(i)` mutating `self.total` in test_gc_generational.cpp:82-102), but never through the
  C++ wrapper call sites `List::set(i, v)` / `Value(vm, instanceHandle).setAttr(...)` under an explicit
  `minorCollect()` sequence.
- **Repro**: COVERAGE GAP — `List xs(vm, {1}); vm.minorCollect(); xs.set(0, Value(vm, BIG));
  vm.minorCollect(); vm.minorCollect(); CHECK(xs[0].asInt() == BIG);` and the analogous
  `setAttr` case on a promoted instance/module handle.
- **Proposed fix**: N/A.
- **Proposed test**: as above.

### A15-3: docs/pages/11-cpp-api.md's ```cpp fenced examples are not executed by any test
[LOW] [informational — not a bug, doc content verified accurate by inspection]
- **Location**: docs/pages/11-cpp-api.md (whole file, ~2132 lines, ~40 ```cpp blocks)
- **What**: `tools/scripts/test_docs_examples.py` (the doc-as-test harness) only walks ```kirito fences
  in `docs/pages/*.md`; the C++-API page's ```cpp blocks (constructor signatures, PinnedHandle usage,
  the rooting-discipline table, ModuleBuilder examples) have no automated staleness check — a future
  signature change could silently drift the docs. I manually cross-checked the constructor lists,
  `PinnedHandle`, `RootScope`, and the wrapper-mutator/barrier claims (lines ~291-297, 359-420, 576-579,
  833-913, 1649-1936, 2105-2106) against the current value.hpp/native.hpp and found them accurate as of
  this round.
- **Repro**: COVERAGE GAP — no automated verification; would need a `cpp`-block harness (compile+run
  each snippet against the current headers) analogous to the `kirito`-block one, which is a bigger
  lift than this round's budget.
- **Proposed fix**: N/A this round.
- **Proposed test**: N/A this round (flagging for awareness; not asking for new infra now).

## Coverage matrix (spot-checked, not exhaustive)

| Surface | Empty | GC-between | Type-mismatch | Aliased/shared | Minor-GC-crossing |
|---|---|---|---|---|---|
| `List` (push/set/pop/contains/clear/iterate) | yes (test_r8 §1, test_value_extra) | yes (pop via A19-2 comment + test_r4 §5 major only) | yes (test_r8 §2) | n/a | **push: yes** (test_gc_generational); set/pop: no (A15-2) |
| `Dict` (set/get/remove/clear/keys/values/pairs) | yes | major-GC yes (test_r4) | yes (test_r8 §2) | n/a | **no** (A15-1) |
| `Set` (add/contains/discard/clear) | yes | major-GC yes | yes | n/a | **no** (A15-1) |
| `String`/`Bytes` (index, contains, items) | yes | yes (A07-4, test_value_extra §GC) | yes | n/a | not explicitly, but String/Bytes are immutable so no mutator-barrier risk exists — moot |
| `PinnedHandle` (ctor/copy/move/assign/self-assign/reset/vector-growth) | yes | yes, exhaustively (test_pinned_handle.cpp, all 9 blocks) | n/a | yes (shared-pin-count block) | yes (setGcThreshold(1) heavy-churn block exercises many minors) |
| `Args` (empty/size/at/opt/require) | yes (test_value_extra, test_r4) | n/a (no allocation inside Args itself) | via `.asX()` on wrapped Value, covered | n/a | n/a |
| `ModuleBuilder` (`fn`, signatured `fn`, `kwfn`, `value`, `alias`) | fn/kwfn/value: yes (throughout stdlib_*.hpp + r4/r7/r8); `alias`: yes (io.hpp `gunzip`-style aliases exist in stdlib, exercised by golden .ki tests) | signatured `fn`'s RootScope-over-defaults: yes (native.hpp:118-126 comment cites `hash.hmac`'s "sha256" default, exercised transitively) | n/a | n/a | n/a |
| `NativeFunction::bindArgs` (out-of-order keywords, defaults, duplicate/unknown keyword, positional-then-keyword overflow) | yes, all error paths (test_r4/r7/r8 + the whole "every stdlib fn accepts kwargs" golden suite) | n/a | n/a | n/a | n/a |
| `makeMethod` (positional fast path, keyword fill, hole-before-required-slot error) | yes (native.hpp inline comment + golden `.ki` kwargs suite: sqldb_kwargs/webserver_kwargs) | yes (multi-capture GC test, test_r8 §5) | n/a | n/a | n/a |
| Over-aligned `NativeClass` | n/a — verified NOT a bug: `Object::operator new(size_t, align_val_t)` (object.hpp:92-95) delegates to the real `::operator new(n,a)`, and the compiler selects that overload automatically whenever `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`, bypassing `pool::allocate` entirely for such types. No misalignment risk. | | | | |

## Summary

**Verdict: the C++ embedding API is barrier-safe and its public signatures are unchanged.**

Every wrapper mutator in `value.hpp` (`List::push/set/pop`, `Dict::set/remove`, `Set::add/discard`,
`Value::setAttr`) routes through the identical barriered primitive (`ListVal::append/setElem`,
`DictVal::set/remove`, `SetVal::add`, `InstanceValue::setAttr`, `ModuleValue::setAttr`,
`EnvValue::define`) that the bytecode VM itself uses for `xs.append(v)` / `d[k]=v` / `self.x=v` — there
is no second, wrapper-only write path that could have forgotten the write barrier during the
generational-GC migration, and I found none that had. `PinnedHandle`'s refcounted pin is correctly
balanced across copy/move/assign/self-assign/reset (verified against `test_pinned_handle.cpp`'s
exhaustive suite), and both `pinnedRoots_` and `auxRoots_` are scanned by `forEachRoot`, which backs
BOTH `collectGarbage` (major) and `minorCollect` (minor) — so a `PinnedHandle` genuinely survives every
collection kind, not just majors. `RootScope` is correctly LIFO by construction (C++ scope-exit
ordering). The one pre-existing false positive scoped here (value.hpp comparison UB) is confirmed still
fixed. `NativeFunction::bindArgs` and `makeMethod` bind out-of-order keywords, defaults, and error cases
correctly and identically to before.

The only gaps found are coverage gaps, not bugs: the specific "old WRAPPER-level container gains a
young value, survives a minor" adversarial pattern this round emphasized is already present for `List`
(test_gc_generational.cpp) but missing for `Dict`/`Set`/`List::set`/`Value::setAttr` at the wrapper call
site (A15-1, A15-2) — low severity since the underlying mutators are barrier-identical and are
exercised crossing generations via Kirito source in the same file. The C++-API doc page's code
examples have no automated staleness check (A15-3), but were manually verified accurate this round.
No fixes were required; three test-only additions are proposed for a future round to close the
matrix gaps.
