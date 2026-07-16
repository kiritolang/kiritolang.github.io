# A10 — deep static analysis + DRY / single-source-of-truth review (v1.15.1)

## Scope
`src/kirito/*.hpp`, `src/kirito.hpp`, `main.cpp`.

## Tooling available
- `clang++ 18` — `--analyze` (clang static analyzer) works. USED.
- `g++ 13` — used for `-fanalyzer` spot checks.
- `clang-tidy` — **NOT INSTALLED** (`which clang-tidy` empty). Not installed per brief.
- `cppcheck` — **NOT INSTALLED**. Not installed per brief.
- `rg`/`grep` pattern hunts — USED (primary DRY tool).
- `./build-debug/ki` — live probing; every triggerable claim below is proven with real output.

## Log
(appended as I go)

---

## Findings

### A10-1: `Dict.update(<instance with _iter_>)` hands out a dangling handle  [severity: HIGH] [confidence: confirmed]
- **Location:** `src/kirito/runtime.hpp:816-823` (`Dict.update`)
- **What:** `update` calls raw `o.iterate(vm)` and holds the resulting `std::vector<Handle>`
  **unrooted** across `vm.arena().deref(pairH).iterate(vm)` — which, when `pairH` is an Instance,
  runs the user's `_iter_` and therefore **allocates**. `InstanceValue::iterate` roots its result
  list only in a `RootScope` local to itself, which dies on return — so both the returned list and
  every element handle in `it` are reachable from no GC root. The next allocation collects them.
  This is exactly the v1.15 A19-1 class.
  **It is also a DRY defect:** the `Dict()` constructor (`runtime.hpp:3057-3061`) implements the very
  same "iterate an iterable of `[key, value]` pairs into a Dict" logic and does it **correctly**, via
  `rootedIterate` for both the outer iterable and each inner pair. Two copies; one rooted, one not —
  the `tensor.apply` Float/Complex shape verbatim.
- **Repro:** `/tmp/a10_upd.ki`
```kirito
var io = import("io")
class Pair:
    var _init_ = Function(self, k, v):
        self.k = k
        self.v = v
    var _iter_ = Function(self):
        return [self.k, self.v]

class Src:
    var _iter_ = Function(self):
        var out = []
        var i = 0
        while i < 30:
            out.append(Pair(1000 + i, 2000 + i))
            i = i + 1
        return out

var d = {}
d.update(Src())
io.print(len(d))
io.print(d)
```
```
$ ./build-debug/ki --gc-threshold 1 /tmp/a10_upd.ki
Traceback (most recent call last):
  File "/tmp/a10_upd.ki", line 19, in <module>
/tmp/a10_upd.ki:19:9: error: dangling handle (stale generation)

$ ./build-debug/ki /tmp/a10_upd.ki          # default threshold: passes, masking the bug
30
{1000: 2000, 1001: 2001, ... 1029: 2029}
```
  Note the values are **> 255**, deliberately outside the small-int intern range — the exact reason
  the v1.15 soak missed this class.
- **Impact:** any `Dict.update(x)` where `x` is a user class with `_iter_` (or any iterable whose
  elements are instances with `_iter_`). Under an unlucky GC it throws `dangling handle`; with a
  different allocation pattern the slot is reused and `update` silently inserts a **wrong key/value**.
- **Proposed fix:** route both loops through `rootedIterate`, exactly as the `Dict()` ctor does:
  ```cpp
  RootScope rs(vm);
  auto it = rootedIterate(vm, a[0], rs, "update expects a Dict or an iterable of [key, value] pairs");
  for (Handle pairH : it) {
      auto pit = rootedIterate(vm, pairH, rs, "update: each pair must have exactly 2 elements (key, value)");
      if (pit.size() != 2) throw KiritoError("update: each pair must have exactly 2 elements (key, value)");
      dict(vm, self).set(vm.arena(), pit[0], pit[1]);
  }
  ```
  Breaks no contract (`rootedIterate` throws the same "not iterable" text). Better still, factor the
  shared pair-walk out so `Dict()` and `Dict.update` cannot drift again — see the DRY table.
- **Proposed test:** `test_gc_generational.cpp` at `--gc-threshold 1` (where the sibling A19-1 pins
  live), plus a `.ki` check in `tools/tests/scripts/`. Asserts `d.update(Src())` yields the 30
  expected pairs. Fails on the unfixed build (proven above).

### A10-2: `#include "kirito.hpp"` does not compile under clang++ (any version) with default flags — the documented libFuzzer build is dead  [severity: HIGH] [confidence: confirmed]
- **Location:** `src/kirito/object.hpp:95` (`::operator delete(p, n, a);`)
- **What:** `Object` re-exposes the aligned deallocation path, including the **sized** aligned form
  `operator delete(void*, std::size_t, std::align_val_t)`. libstdc++ declares the *global*
  `::operator delete(void*, size_t, align_val_t)` only under `__cpp_sized_deallocation`. **g++ enables
  sized deallocation by default; clang does NOT** (it requires `-fsized-deallocation`). So on clang the
  global overload does not exist and the call is a **hard compile error**, not a warning.

  This breaks two contracts CLAUDE.md states outright:
  1. *"`#include "kirito.hpp"` embeds Kirito in any C++ program (Lua-style)"* — an embedder using
     clang cannot include the umbrella header at all.
  2. *"Toolchain present: `g++ 13`, `clang++ 18`"* and *"`-DKIRITO_ENABLE_LIBFUZZER=ON` builds
     `ki_fuzz` (**needs clang**)"* — the coverage-guided fuzzer is documented as clang-only and
     therefore **cannot currently be built at all**. CMake adds no `-fsized-deallocation` anywhere.
- **Repro:**
```
$ clang++ -std=c++20 -I src -fsyntax-only /tmp/a10_tu.cpp        # tu is just: #include "kirito.hpp"
src/kirito/object.hpp:95:9: error: no matching function for call to 'operator delete'
exit=1

$ clang++ -std=c++20 -fsized-deallocation -I src -fsyntax-only /tmp/a10_tu.cpp
exit=0

$ g++ -std=c++20 -I src -fsyntax-only /tmp/a10_tu.cpp
exit=0
```
  And the documented fuzzer target specifically:
```
$ clang++ -std=c++20 -I src -fsyntax-only tools/tests/fuzz/fuzz_libfuzzer.cpp
src/kirito/object.hpp:95:9: error: no matching function for call to 'operator delete'
exit=1
```
  Minimised to the exact cause:
```
$ echo '#include <new>
int main(){ ::operator delete((void*)0,(std::size_t)1,(std::align_val_t)16); }' | clang++ -std=c++20 -x c++ -fsyntax-only -
  -> error        # with -fsized-deallocation -> ok;  g++ default -> ok
```
- **Origin:** commit `8048c97` ("fix(core): finish v1.14 deferred audit items"), which added the
  aligned-delete guard block. It was only ever compiled with g++, so the break was invisible — the four
  CMake presets all use g++.
- **Impact:** every clang embedder; the entire libFuzzer workflow; and any clang-based static analysis
  (this is why my `clang++ --analyze` run over the umbrella header aborted before emitting a single
  report — the tool never got past the header).
- **Proposed fix:** the header must not depend on a compiler flag. Guard the sized form and fall back
  to the unsized aligned delete (always declared, always correct — dropping the size hint is legal):
  ```cpp
  #if defined(__cpp_sized_deallocation)
      static void operator delete(void* p, std::size_t n, std::align_val_t a) noexcept {
          ::operator delete(p, n, a);
      }
  #endif
  ```
  The unsized `operator delete(void*, std::align_val_t)` on line 94 already covers the over-aligned
  case, so simply `#if`-ing out the sized overload is sufficient and zero-risk. Additionally add
  `-fsized-deallocation` for Clang in `CMakeLists.txt` so a clang build keeps the sized fast path.
  Breaks no documented contract; no current type is over-aligned (the block is itself defensive).
- **Proposed test:** a CI/lint step that runs `clang++ -std=c++20 -I src -fsyntax-only` on a TU
  containing only `#include "kirito.hpp"`. Cheap (no link, one TU, ~seconds) and it pins the
  "embeds in any C++ program" contract that currently has **no** test at all. Fails on the unfixed
  tree (proven above).

### A10-3: the "row index out of range" check exists four times with two different texts; `matrix` and `complex` are indistinguishable  [severity: LOW] [confidence: confirmed]
- **Location:** `stdlib_matrix.hpp:143` + `:253`; `stdlib_complex.hpp:318` + `:396`
- **What:** the same conceptual bound check is written **four times**. The `getItem`/`row()` accessor
  path says `"Matrix row index out of range"` / `"ComplexMatrix row index out of range"`, while the
  `.row()` *method* path (10 lines of otherwise byte-identical copy-paste) says a bare
  `"row index out of range"` with **no type prefix**. Every other Matrix/ComplexMatrix diagnostic names
  its type; these two don't — and because both types emit the identical unqualified string, the message
  cannot tell a user which type threw.
- **Repro:**
```kirito
var m = matrix.Matrix([[1,2],[3,4]])
m.row(9)   # -> row index out of range
m[9, 0]    # -> Matrix index out of range
```
```
$ ./build-debug/ki /tmp/a10_row.ki
m.row(9)      : row index out of range
m[9, 0]       : Matrix index out of range
```
  `complex.ComplexMatrix(...).row(9)` emits the same bare text.
- **Impact:** diagnostics only — CLAUDE.md's "Clear diagnostics ... a message a user can act on".
  No corruption. Low, but it is the *visible symptom* of the matrix/complex copy-paste (DRY table row 2):
  the drift proves the two files are edited independently.
- **Proposed fix:** make both say `"Matrix row index out of range"` / `"ComplexMatrix row index out of
  range"`, sourced from the type's existing `indexOf`-style prefix rather than a fresh literal. Check
  `tools/tests/errors/*.experr` for a pinned `row index out of range` first — if a test pins the bare
  text, update it in the same change.
- **Proposed test:** `tools/tests/errors/matrix_row_oob.ki` + `.experr` asserting the type-qualified text.

### A10-4: `kMaxRepeat` is the declared "single source of truth" but the literal `256ull * 1024 * 1024` is re-typed in 6 places — one with a comment that is now factually wrong  [severity: LOW] [confidence: confirmed]
- **Location:** `common.hpp:159` declares the authority:
  ```cpp
  // ~256 MB is far beyond any legitimate scripting use. Single source of
  // truth for String/List/Bytes repetition, padding, and `range` — defined here so bytes.hpp
  // (compiled before runtime.hpp) shares the exact same cap.
  inline constexpr uint64_t kMaxRepeat = 256ull * 1024 * 1024;
  ```
  Re-typed as a bare literal at: `bytes.hpp:326`, `deflate.hpp:229` (`kMaxInflateOut`),
  `stdlib_io.hpp:275` (`kMaxBuf`), `proc_compat.hpp:52` (`kMaxCapture`), `stdlib_net.hpp:279`
  (`kMaxRecvAll`), `stdlib_random.hpp:248`.
- **What:** the worst offender is `stdlib_random.hpp:248`, whose comment **explains why it can't use
  the constant — and the explanation is stale**:
  ```cpp
  // Resource guard on the result length, matching runtime.hpp's kMaxRepeat for list
  // repetition (not visible here — stdlib_random is included before that constant).
  if (static_cast<uint64_t>(k) > 256ull * 1024 * 1024)
  ```
  `kMaxRepeat` was **moved out of runtime.hpp into common.hpp** precisely so earlier-compiled headers
  could share it (see `runtime.hpp:77`: *"kMaxRepeat ... is defined in common.hpp so bytes.hpp shares
  it."*). Proof it is visible: **the same file already uses it 220 lines earlier** —
  `stdlib_random.hpp:28`: `if (static_cast<uint64_t>(n) > kMaxRepeat) throw ...`. So one guard in
  `stdlib_random.hpp` uses the named constant and another duplicates the literal while claiming it
  cannot. A reviewer trusting the comment would not fix it.
- **Impact:** no live bug today (all six literals happen to agree). It is a **latent** one: changing
  `kMaxRepeat` in common.hpp silently leaves six guards at the old value, and the stale comment
  actively misdirects. Exactly the class the brief targets — "a fix to one copy would miss the other".
- **Proposed fix:** (a) `stdlib_random.hpp:248` → `if (static_cast<uint64_t>(k) > kMaxRepeat)` and
  delete the stale parenthetical — zero risk, same value, and it removes a false claim from the code.
  (b) the four *named* caps (`kMaxInflateOut`/`kMaxBuf`/`kMaxCapture`/`kMaxRecvAll`) are separate
  policies that merely coincide at 256 MB; define each as `= kMaxRepeat` (or leave, but drop the bare
  literal) so the relationship is explicit rather than coincidental. `bytes.hpp:326` should use
  `kMaxRepeat` outright — `bytes.hpp` is the header the constant was *moved to common.hpp for*.
- **Proposed test:** compile-time only — a `static_assert(kMaxInflateOut == kMaxRepeat)`-style tie
  is not appropriate for genuinely independent policies; the fix's value is in (a), which needs no test.

### Verified-good: the v1.15 A19-1 rooting fixes are complete (no regression, no missed sibling)
I re-probed the whole A19-1 class rather than trusting the fix note:
- `toleranceSig` (`native.hpp:185`) is the sole tolerance signature; all **5** `.compare` sites
  (`runtime.hpp:324`, `stdlib_matrix.hpp:189`, `stdlib_complex.hpp:158`+`:364`,
  `stdlib_tensor.hpp:1660`) route through it and root via `rs.add`. No 6th copy exists.
- The `GcPauseScope` around `mod->setup(builder)` (`runtime.hpp:2421`) is present and is the **only**
  `GcPauseScope` in the tree — it covers every `m.fn(...)` default in every module (I enumerated all
  31 such sites; all are inside `setup()`).
- The only two signature defaults built **outside** `setup()` (so not covered by the pause) are
  `stdlib_int.hpp:646` and `native.hpp:188-189` — **both** correctly `rs.add(...)`.
- Live sweep under `--gc-threshold 1` with values > 255 across `List/Set/Dict` ctors, `List.extend`,
  `Set.union`, `sorted`, `enumerate`, `sum`, `min`, `zip`, `map`, `filter`, `all`: **all clean**.
  `Dict.update` was the single failure → A10-1.
