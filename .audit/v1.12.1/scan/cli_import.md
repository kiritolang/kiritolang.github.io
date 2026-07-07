# v1.12.1 (audit loop) — CLI, path resolution, import/module system

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

### F3 [MED] .ki-file modules leak top-level `_private` names as members; frozen modules hide them (inconsistent)
- where: src/kirito/runtime.hpp:2488-2490 (.ki path) vs 2399-2402 (frozen path)
- repro: privmod.ki: `var _secret = "x"` ; importer reads `m._secret` -> works and prints it.
  A frozen (registerSourceModule) module filters `k.front() != '_'` so its `_names` are hidden.
- actual: .ki module export loop only excludes "arglist"/"argmain"; it does NOT skip leading-`_`
  names, so a .ki module cannot keep private top-level helpers and they leak to importers.
- expected: same export policy for both loaders — top-level `_name` should be a private module
  internal (as the frozen path already treats it and its comment states).
- fix idea: apply the same `!k.empty() && k.front() != '_'` filter in the .ki export loop.
- note: DRY — the two export-filter loops duplicate logic and have already drifted.

### F4 [MED] Script's own directory is searched LAST — a package/KIRITO_PATH module shadows the script's sibling module
- where: main.cpp:181-204. Order added: "." (181), --lib (182), KIRITO_PATH+packages (195-197),
  then script's parent dir (204, LAST). So the script dir has the LOWEST priority.
- repro: scriptdir/prog.ki imports "foo"; scriptdir/foo.ki says "SCRIPT SIBLING",
  KIRITO_PATH dir has foo.ki saying "KIRITO_PATH". Run prog.ki from an unrelated cwd:
  resolves to "KIRITO_PATH", not the script's own sibling.
- actual: KIRITO_PATH/package module wins over the script's sibling module.
- expected: CLAUDE.md + cli_paths.hpp doc the order as cwd, script dir, KIRITO_PATH, packages
  — i.e. the script's directory should outrank KIRITO_PATH and installed packages. A local
  helper next to the script can be silently hijacked by an installed package of the same name.
- fix idea: add the script's parent dir to the path BEFORE the environment/package paths
  (move the addLibPath at main.cpp:204 up above lines 195-197, guarded on !file.empty()).
