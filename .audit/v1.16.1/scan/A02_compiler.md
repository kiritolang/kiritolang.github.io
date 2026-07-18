# A02 — compiler + resolver + analyzer + bytecode (v1.16.1)

Scope: resolver.hpp, analyzer.hpp, compiler.hpp, bytecode.hpp, locals.hpp

Findings appended incrementally below.

### F02-1 [Med] Analyzer false-positive: a local captured by an EARLIER-defined nested function is flagged "assigned but never used"
- analyzer.hpp:65-72 (popScope unused check) + :302-313 (analyzeFunction) / :77-82 (markUsed) — CONFIRMED (repro run)
- What/why: The analyzer declares names in source/execution order and `markUsed` walks only the scopes that already hold a `Decl`. A nested function defined BEFORE the `var` it captures is analyzed first; its reference to the outer local calls `markUsed(name)` before that local is `declare()`d, so the mark is silently dropped (markUsed returns without finding it). When the `var` is later declared and the enclosing scope pops, the local is still `used=false` → spurious "variable 'y' is assigned but never used". The resolver correctly resolves this by MEMBERSHIP (collectDecls before checkBlock); the analyzer has no such pre-declare pass, so the two disagree.
- Trigger:
  ```
  var outer = Function():
      var g = Function(): return y   # captures y, defined before y's var
      var y = 5
      return g()
  ```
  emits `warning: variable 'y' is assigned but never used` though y is read. Reordering (var y before var g) suppresses it.
- Also hits MUTUALLY RECURSIVE nested functions (a pattern CLAUDE.md says must resolve): `var isEven = Function(n): ... isOdd(n-1)` then `var isOdd = Function(n): ... isEven(n-1)` warns `variable 'isOdd' is assigned but never used` (isEven, defined first, references isOdd before its var). CONFIRMED.
- Fix idea: give analyzeFunction/analyzeBlock a pre-pass that `declare()`s all block-bound names (mirroring resolver's collectBlockDecls) before walking, OR defer the unused-check until after a whole scope subtree (including later-declared names) is analyzed. Simpler: in markUsed, if the name isn't found, remember it in a per-scope "used-before-declared" set and consult it in popScope.
- Test to add: an errors/warnings golden with a forward-captured local that must NOT warn.
- Verified-real: YES (build-debug/ki, warning printed; -w suppresses; program prints 5 correctly).

