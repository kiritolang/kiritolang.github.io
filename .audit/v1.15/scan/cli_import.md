# v1.15 audit — CLI, path resolution, import/module system

Subsystem: main.cpp, src/kirito/cli_paths.hpp, module loader / import machinery
(module.hpp, vm.hpp, runtime.hpp import path).

## LOG
- Starting: reading source files.

## FINDINGS

- Verified WORKING (ruled out as bugs):
  - self-import throws "circular import detected: selfa -> selfa"; a->b->a throws with full chain.
  - non-cyclic diamond (a->b,c->d): dd body runs ONCE. Correct.
  - cache-by-path: import("dd") and import("dd.ki") return same object (id equal).
  - module that throws at load: in-progress set unwinds, a later import retries (fails again same err, not a cycle err).
  - nonexistent module -> "no module named 'X'". import("") -> "no module named ''".
  - parse error in imported module reports THAT module's path (badsyntax.ki:2:9), not importer.
  - arglist/argmain: main file gets argmain True + full arglist; imported module gets False + [].

## Testing native-module vs .ki collisions, .ki extension on native, NUL, KIRITO_PATH edge cases next.

### F1 [HIGH] Deep import chain overflows native stack (uncatchable segfault)
- where: src/kirito/runtime.hpp:2416 importModule (no depth guard on nested imports)
- repro: generate c1.ki..cN.ki where c_i does `import("c_{i+1}")`, main imports c1.
  N=2000 OK; N=3000/4000/5000 -> Segmentation fault (exit 139), no output.
- actual: SIGSEGV (native stack overflow). Each nested import recurses C++
  importModule -> runBytecodeBody -> applyCall(import) -> importModule, unbounded.
- expected: a catchable KiritoError like the call-depth guard gives for deep Kirito
  recursion (docs promise "deeply nested source/data structures throw instead of
  overflowing the native stack" — import nesting is not covered).
- fix idea: track import nesting depth (importStack_.size() already exists!) and throw
  a KiritoError when it exceeds a bound (e.g. reuse the call-depth limit).

### F2 [LOW] import("<native>.ki") fails though comment claims .ki suffix is accepted
- where: src/kirito/runtime.hpp:2444-2458; .ki-suffix stripping (fileBase) only applies
  to the FILE search, not the moduleFactories_ (native/frozen) lookup at line 2444.
- repro: `import("io.ki")` -> "no module named 'io.ki'" (but `import("io")` works, and
  for a real FILE dd.ki both `import("dd")` and `import("dd.ki")` work).
- actual: native/frozen modules reject the .ki suffix; only file modules accept it.
- expected: comment at line 2454-2455 says import("io") and import("io.ki") both resolve.
- fix idea: strip a trailing ".ki" from `name` before the moduleFactories_ lookup too
  (or lookup both name and fileBase).
