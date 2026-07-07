# v1.12.1 (audit loop) — shared worker briefing

You are ONE worker in a deep, comprehensive audit of the **Kirito** interpreter (repo root:
`/home/user/kiritolang.github.io`). Read this fully, then work your assigned subsystem.

## THE MOST IMPORTANT RULE — persist as you go
You are given YOUR OWN `.md` file (path in your task, e.g. `.audit/v1.12.1/scan/<name>.md`). **Write to it
INCREMENTALLY — append each finding the moment you confirm it, before moving on.** We may hit usage limits
and get force-stopped at any time; anything only in your head or your final message is LOST, but anything
written to your `.md` survives (the orchestrator commits these files). Treat the `.md` as your durable
notebook: log what you're checking, what you ruled out, and every confirmed finding. Do NOT wait until the
end to write. Your final chat message should be a short pointer ("findings in <file>, N confirmed bugs").

## Your mission
Find REAL defects and weak spots in your subsystem, from every angle:
- **Correctness / logic / semantics** — wrong results, off-by-one, sign errors, operator/dispatch mistakes,
  contract violations, inconsistencies between related functions.
- **Silent errors** — a case that SHOULD raise (per docs / sane semantics) but returns garbage or nothing.
- **Memory safety / UB** — dangling handles (freshly-allocated Handle not GC-rooted across a call that can
  collect), use-after-free, integer overflow (int64 / size_t), out-of-bounds, uninitialized reads,
  `static_cast` where a `dynamic_cast` is needed, alignment.
- **Resource exhaustion** — unbounded allocation/recursion/loops on adversarial input (missing guards).
- **Edge cases** — empty inputs, boundary values (0, ±1, INT64_MIN/MAX, NaN, inf, ±0.0), huge/negative
  counts, Unicode/multibyte, deep nesting, aliasing, mutation-during-iteration, reentrancy.
- **Thread-safety** — only for `parallel`/`dispatcher` (the sole concurrent code; one OS thread per VM
  elsewhere by design).
- **DRY / single-source-of-truth** — logic duplicated across files that could drift (note it; the DRY agent
  aggregates).

## How to work
1. Read your source file(s) DEEPLY. Understand the real behavior, invariants, and guards.
2. For each function/method/attribute/operator: reason about its edge cases and failure modes. Form
   hypotheses about what could break.
3. **PROBE** with the prebuilt interpreter: `./build-debug/ki <script.ki>` (write throwaway scripts to
   `/tmp/.../scratchpad/` or your own temp path). Confirm each hypothesis with a minimal repro. A finding
   is only "confirmed" once you've reproduced it (or, for pure C++/embedding issues not reachable from
   Kirito, once you've traced the exact code path).
4. Write every confirmed finding to your `.md` immediately, with: severity (HIGH/MED/LOW), a one-line
   summary, `file:line`, a minimal repro (the `.ki` snippet or C++ path), actual vs expected, and a
   suggested fix direction.
5. Distinguish a real BUG from BY-DESIGN behavior. If unsure, write it as "SUSPECT" with your reasoning —
   the orchestrator re-verifies before fixing. Do NOT fix by-design behavior.

## Hard rules
- **DO NOT modify anything under `src/`.** You only READ src and WRITE your `.md`. All fixes are done
  centrally by the orchestrator after triage (to avoid conflicting edits and to re-verify each finding).
- You MAY write throwaway probe scripts outside the repo (scratchpad). Do not add stray files to the repo
  except your assigned `.md`.
- Be precise and skeptical. A false-positive wastes triage time. Reproduce before claiming.

## Kirito gotchas (will bite your probes)
- Comments `#`. Blocks: `:` + newline + indent. **No `;` statement separator** (inline `Function(): return x`
  is ONE statement). String escapes ONLY `\n \t \r \0 \\ \" \'` `\xHH` — **no `\u`**; use `chr(N)`.
  `len(String)` counts code points. Strings nested in a container print repr (quoted). Float `==`/`!=` is
  EXACT IEEE-754 (`NaN != NaN`). `/` is always Float; `//` floor; `**` right-assoc. Regex patterns need raw
  strings `r"..."`. `catch String as e` binds the message. `import("io")`; `io.print(...)`. Public names are
  lowercase no-underscore.
- Useful probe helper:
```
var io = import("io")
var p = Function(l, fn):
    try:
        io.print(l, "=>", fn())
    catch String as e:
        io.print(l, "THROW:", e)
```

## Finding-log format (write these into your `.md`)
```
## FINDINGS
### F1 [HIGH|MED|LOW|SUSPECT] <one-line summary>
- where: src/kirito/<file>.hpp:<line>
- repro: <minimal .ki or C++ path>
- actual: <what happens>   expected: <what should happen>
- fix idea: <direction>
```
Also keep a running "## LOG" section of what you've examined and ruled out, so partial progress is visible.
