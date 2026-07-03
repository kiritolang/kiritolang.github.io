# Post-work checklist

The routine to run **after every change, before declaring it done**. The mechanics live in
`tools/scripts/post_work_check.sh`; this file is the human-readable contract.

## The routine

1. **Write tests for what changed.** Every new feature or fixed bug gets a focused test in the same
   change (CLAUDE.md rule). Prefer many small tests over one big one.
   - Behaviour at the C++/embedding boundary → a `tools/tests/unit/test_*.cpp` (register it in
     `tools/tests/CMakeLists.txt`).
   - End-to-end `.ki` behaviour with known output → `tools/tests/scripts/NAME.ki` + `NAME.expected`.
   - Code that *should fail* → `tools/tests/errors/NAME.ki` + `NAME.experr` (each `.experr` line is a
     required substring of stderr; the script must exit non-zero). Cover the bad path, not just the
     good one.

2. **Build the variants and run the WHOLE suite — SEQUENTIALLY, in order** (`tools/scripts/post_work_check.sh`).
   The order is the workflow gate, not just a list:

   1. **`debug`** — g++ `-O2` with the **hardened** warning set (`-Wconversion -Wshadow -Wreorder
      -Wunused -fstack-protector-all -Werror`, on top of the release warnings). The strictest compile
      gate. (binaryDir `build-debug`)
   2. **`release`** — g++ `-O2` with the looser warnings-as-errors set (no `-Wconversion`/`-Wshadow`);
      the build to benchmark and ship. (binaryDir `build-release`)
   3. **Commit and push to `main`** — **once `debug` AND `release` are both green.** This is the
      durability point: push *before* the long asan run so a crash, preemption, or container rollback
      can never lose the work. (Branches are forbidden — see CLAUDE.md and the `block_new_branches`
      hook.)
   4. **`asan`** — AddressSanitizer + UBSan (`-fno-sanitize-recover=all`) with the hardened warning
      set; the memory/UB-safety gate and a slow one. **Fix any error it surfaces, then re-run and
      push the fix.** (binaryDir `build-asan`)
   5. **`tsan`** — ThreadSanitizer with the hardened warning set; the data-race + lock-order-inversion
      gate for the `parallel` multiprocessing dispatcher (the only concurrent code). **Fix any error
      it surfaces, then re-run and push the fix.** (binaryDir `build-tsan`)

   Each variant is a clean reconfigure + build + `ctest`. The suite is **auto-discovered**: unit
   executables register in `tools/tests/CMakeLists.txt`, and the `tools/scripts/` and `errors/` directories are
   globbed, so the routine never enumerates test files and stays correct as tests come and go.

   > Disk hygiene: the four build dirs together are large (~1.3 GB debug + 1.3 GB release + ~12 GB asan
   > + ~9 GB tsan ≈ 24 GB) and have filled a small disk mid-run (`No space left on device`). The
   > routine therefore **deletes each variant's build dir as soon as that variant's tests pass**, so
   > peak footprint is one variant at a time; a *failed* variant keeps its dir for debugging, and the
   > `/tmp/pw_<v>.*.log` logs are always retained. Pass `--keep-builds` to retain every build dir.

   > Why push between release and asan? asan is slow, and this environment has rolled the container
   > back to an earlier commit mid-run before. Pushing the green debug+release state first means the
   > work survives regardless; the asan pass then only ever *adds* a fix on top.

3. **Update documentation and `CLAUDE.md` — always, in the same change.** The docs must reflect
   reality:
   - Update `CLAUDE.md` whenever a change touches the language design, architecture, builtins,
     stdlib, or workflow (it is the source of truth and must always describe Kirito as it *is*).
   - Update the HTML docs: edit the relevant `docs/pages/*.md` and regenerate with
     `python3 docs/build_docs.py`.
   A feature without matching doc + CLAUDE.md updates is **not done**.

## Run it

```sh
tools/scripts/post_work_check.sh           # debug -> release -> (commit gate) -> asan -> tsan
tools/scripts/post_work_check.sh --no-asan # debug + release only (the commit gate), for a fast inner loop
```

After `debug` and `release` pass, the script prints `READY TO PUSH` — that's the cue to commit and
push. It then runs `asan` and `tsan`. Exit status is non-zero if any variant fails to build or has a
failing test; a full run is "done" only when it prints `ALL GREEN`.

## Notes

- The project ships four presets: `debug`, `release`, `asan`, `tsan` (the former `strict` preset was
  folded into the hardened `debug`). The Windows cross build is no longer part of the routine.
- The codebase is `-Wconversion`-clean; the deliberate native-binding idiom (bound-method lambdas
  whose `vm`/`self` parameters shadow the enclosing `getAttr`/`setup` — same VM by design) is silenced
  with a scoped `#pragma GCC diagnostic ignored "-Wshadow"` in the stdlib glue + runtime type-methods,
  so `-Wshadow` stays active and meaningful in the evaluator/parser/lexer/GC core.
- Optional features behind flags (e.g. `-DKIRITO_ENABLE_TLS=ON`) are not part of the default sweep;
  build them explicitly when a change touches that code path.
