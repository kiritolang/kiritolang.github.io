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
   - **Regression-per-bug.** A fix for a specific bug ships an executable test the bug would have
     failed. Name it after what it covers, not the bug's ID — `spec_dict_iter_after_delete.ki`,
     not `fix_pr47.ki` — so a reader who trips it three years from now sees the *symptom* and can
     read the fixture to understand it. The `r4_`/`r5_`/`r6_`/`r10_`/`r11_` families collect
     these round-by-round; the newer `probe_*` files hold the property-based / doc-as-test /
     conformance flavours. This turns the bug tracker into an executable diary that can never
     silently rot.

2. **Build the variants and run the WHOLE suite — SEQUENTIALLY, in order** (`tools/scripts/post_work_check.sh`).
   The order is the workflow gate, not just a list:

   **SEQUENTIALLY is a memory constraint, not a preference.** Every test TU includes the whole
   header-only interpreter: one compile peaks at ~1.7 GB RSS at `-O2` and ~3.2 GB under asan, so peak
   build RAM is `jobs × that` and scales with CORE COUNT rather than with the box's RAM. A 24-core
   `-j24` wants ~41 GB (release) / ~78 GB (asan); two variants at once want ~82 GB. On the WSL2 dev box
   (24 cores / 47 GB, booted `panic=-1`) that OOMs the kernel and reboots the whole distro — you lose
   the VM, not just the build. The script sizes `-j` from MemAvailable (cap `PW_MAX_JOBS`, default 16;
   `PW_SANITIZER_JOBS` overrides asan/tsan) and **refuses to start** if another instance holds its lock
   or if a stray `cc1plus`/`ninja` is already running. Don't background a second variant to save time,
   and don't run a bare `cmake --build -j$(nproc)` beside it — the script can only budget the jobs it
   knows about.

   1. **`debug`** — g++ `-O2` with the **hardened** warning set (`-Wconversion -Wshadow -Wreorder
      -Wunused -fstack-protector-all -Werror`, on top of the release warnings). The strictest compile
      gate. (binaryDir `build-debug`)
   2. **`release`** — g++ `-O2` with the looser warnings-as-errors set (no `-Wconversion`/`-Wshadow`);
      the build to benchmark and ship. (binaryDir `build-release`)
   3. **Commit and push to `claude-branch`** — **once `debug` AND `release` are both green.** This
      is the durability point: push *before* the long asan run so a crash, preemption, or container
      rollback can never lose the work. (Every commit and push goes to `claude-branch` only; see
      CLAUDE.md and the `enforce_claude_branch` hook.)
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
