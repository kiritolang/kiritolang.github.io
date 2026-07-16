# Audit v1.15.1 — exhaustive full-codebase round

Target: the **1.15.1** patch (this loop and the completed `v1.15` loop both land there; `kVersion` is
still `1.15.0` and a release is the user's call, not ours).

## What this round is

The broadest round yet. The mandate is not "find some bugs" — it is **every class, method, attribute,
argument and feature probed from every angle, in both the C++ and the `.ki` suites**: typical values,
edge cases, malformed input, adversarial input, fuzz. Plus static analysis, single-source-of-truth
(DRY) review, and a low-risk pass at the interpreter's *timing instability* (high variance, not low
throughput).

## The lesson that shapes this round

v1.15 shipped 18 scanner agents that concluded **"no HIGH memory-safety findings"** and **"the write
barrier is COMPLETE."** Both were **wrong**. Fourteen memory-safety sites (A19-1/A19-2) turned up
afterwards, during unrelated perf work, including `enumerate` past index 255, every `.compare()` that
omits a default, `net.urlsplit`, `regex.groupdict`, and `json.loads` silently losing nested arrays.

They hid because the write-barrier soak (`--gc-threshold 1`) had only ever been run over tests using
**small values** — integers ≤ 256 are interned and permanently rooted, so the entire bug class was
invisible to the very test designed to catch it.

**So: a test that passes proves only what its inputs exercise. Prefer probing the real binary over
reading code and reasoning that it looks right. `verified live` beats `confirmed by reading`.**

## Layout

```
v1.15.1/
  README.md          this file — plan, rules, the agent brief
  scan/AXX_name.md   ONE file per agent. The agent owns it and writes to it AS IT GOES.
  FINDINGS.md        the triaged roll-up (written after the scan phase)
```

## Agent brief (every scanner gets this)

**Your `.md` file is yours. Write to it incrementally — never hold findings only in your context.**
This session may be force-stopped at any moment by usage limits; anything not on disk is lost. Append
each finding the moment you confirm it, before moving on. Start the file immediately with your scope,
then keep appending.

**Do NOT build. Ever.** Every test TU includes the whole header-only interpreter; a second concurrent
build OOM-kills the dev box and reboots the WSL2 distro (peak RAM = jobs × ~1.7–3.2 GB, and it scales
with core count). A `build-debug/ki` binary is already built for you — **probe with it**:

```sh
./build-debug/ki /tmp/your_probe.ki                 # run a probe script
./build-debug/ki --gc-threshold 1 /tmp/probe.ki     # the write-barrier soak (see below)
./build-debug/ki --gc-stats /tmp/probe.ki           # GC counters
echo 'io.print(1)' | ./build-debug/ki               # REPL/stdin
```

If you need a C++-level probe, WRITE the test but do not compile it; say so in your file and hand it
to the main agent.

**How to hunt (in rough order of yield):**
1. **Probe the live binary.** Every claim you report must have a repro you actually ran, with the real
   output pasted in. A finding "confirmed by reading the code" is a hypothesis, not a finding.
2. **Use values that escape the happy path.** Integers > 256 and < -256 (outside the intern range),
   int64 bounds, empty/1-element/huge collections, non-ASCII and astral-plane text, NaN/±inf/-0.0,
   deep nesting, cycles, and **the same probe under `--gc-threshold 1`** (this is what found A19).
3. **Omit optional arguments.** A19-1's whole class only fires when a caller omits a default.
4. **Every argument of every function**: wrong type, wrong count, negative, zero, huge, None, keyword
   form, out-of-order keywords, duplicate keyword, unknown keyword.
5. **Check the tests, not just the code.** For your surface, list what has NO test (C++ or `.ki`) and
   propose the test. Untested surface is a finding in this round.
6. **DRY.** If the same logic is written twice, say where — a fix applied to one copy and not the
   other is how `tensor.apply`'s Float branch stayed broken while its Complex branch was correct.

**Before reporting, read `.audit/README.md`'s false-positives table.** Behaviours in it are by design
and were deliberately re-affirmed across rounds; re-"fixing" one is a regression. If you believe an
entry is wrong, argue it explicitly with a repro — do not silently contradict it.

**Findings format** (append one block per finding):

```
### AXX-N: one-line symptom  [severity: HIGH|MED|LOW] [confidence: confirmed|likely|speculative]
- Location: file:line
- What: the defect, and why it is a defect (which contract it breaks)
- Repro: the exact script/command + the REAL output you got
- Impact: who hits it, how
- Proposed fix: (and whether it risks a documented contract)
- Proposed test: where it belongs, what it asserts
```

Also end your file with:
- **Coverage gaps** for your surface (C++ and `.ki`), most valuable first.
- **Non-findings**: things you probed that turned out correct — so the next round doesn't re-probe
  them, and so a wrong "all clear" can be audited later. Say what inputs you used.
- **Status: DONE** when finished (so a resumed session knows).

## Rules for the fix phase (main agent)

- Only fix REAL, triggerable bugs. **Double-check the bug was a bug** before fixing it.
- Never break a documented contract. Check the false-positives table.
- Stay idiomatic; match surrounding style. Single-source anything reused.
- Every fix ships a regression test named for the SYMPTOM, and the test must be verified to FAIL on
  the unfixed build.
- Update `CLAUDE.md` / `docs/` in the same change when behaviour or design moves.
- Validate on debug + release; asan + tsan for anything touching GC/parallel. Commit in small batches.
