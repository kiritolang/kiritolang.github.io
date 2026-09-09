# Post-work checklist

The routine to run **after every change, before declaring it done**. The mechanics live in
`tools/scripts/post_work_check.sh`; this file is the human-readable contract.

## The routine

1. **Write tests for what changed.** Every new feature or fixed bug gets a focused test in the same
   change (CLAUDE.md rule). Prefer many small tests over one big one.
   - Behaviour at the C++/embedding boundary → a `tests/unit/test_*.cpp` (register it in
     `tests/CMakeLists.txt`).
   - End-to-end `.ki` behaviour with known output → `tests/scripts/NAME.ki` + `NAME.expected`.
   - Code that *should fail* → `tests/errors/NAME.ki` + `NAME.experr` (each `.experr` line is a
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

   The presets cover two orthogonal axes — the sanitizer axis (none/`-O2`, ASan+UBSan, TSan) and the TLS
   axis (OpenSSL on/off) — so **both the OpenSSL-linked paths and the no-OpenSSL fallbacks are sanitized
   by default**. (A `debug` preset, g++ `-O0`, exists for local iteration but is **not** in the gate: its
   only unique value is `-O0`, and its hardened warnings are already carried by the sanitizer presets and
   now `release`.)

   1. **`release`** — g++ `-O2` with the **hardened** warnings-as-errors set (now incl. `-Wconversion
      -Wshadow -Wreorder -Wunused`), TLS **on**. The build users ship, the only `-O2` build (validates
      the real optimized codegen and optimizer-only warnings the `-O1`+instrumented sanitizers do not),
      and the strict warning gate. (binaryDir `build-release`)
   2. **Commit and push to `claude-branch`** — **once `release` is green.** It is the sole non-sanitized
      functional build, so it is the pre-push gate. This is the durability point: push *before* the long
      sanitizer runs so a crash, preemption, or container rollback can never lose the work. (Every commit
      and push goes to `claude-branch` only; see CLAUDE.md and the `enforce_claude_branch` hook.)
   3. **`asan`** — AddressSanitizer + UBSan (`-fno-sanitize-recover=all`), hardened warnings, TLS **on**;
      the memory/UB gate over the OpenSSL + HTTPS/TLS glue (the highest-value surface). Slow. **Fix any
      error it surfaces, then re-run and push the fix.** (binaryDir `build-asan`)
   4. **`asan-notls`** — the SAME ASan+UBSan gate with TLS **off**; sanitizes the no-OpenSSL build (the
      `#else` fallbacks) and proves nothing accidentally hard-depends on OpenSSL. (binaryDir `build-asan-notls`)
   5. **`tsan`** — ThreadSanitizer, hardened warnings, TLS **on**; the data-race + lock-order-inversion
      gate for the `parallel` multiprocessing dispatcher (the only concurrent code — TLS-independent, so
      TLS on merely also compiles the OpenSSL glue under TSan). **Fix any error it surfaces, then re-run
      and push the fix.** (binaryDir `build-tsan`)

   Each variant is a clean reconfigure + build + `ctest`. The suite is **auto-discovered**: unit
   executables register in `tests/CMakeLists.txt`, and the `tools/scripts/` and `errors/` directories are
   globbed, so the routine never enumerates test files and stays correct as tests come and go.

   OpenSSL (libssl-dev) is a prerequisite for release/asan/tsan (all TLS-on); only asan-notls builds
   without it. LeakSanitizer reports "definitely lost", not OpenSSL's "still reachable" globals (freed by
   `OPENSSL_cleanup`'s atexit handler), so a TLS-linked asan run is clean; if a future OpenSSL genuinely
   leaks or trips TSan on its own internal locking, add a NARROW suppression scoped to libcrypto/libssl
   (external code) rather than dropping TLS from the sanitizers.

   > Disk hygiene: the four build dirs together are large (~1.3 GB release + ~12 GB asan + ~12 GB
   > asan-notls + ~9 GB tsan ≈ 34 GB) and have filled a small disk mid-run (`No space left on device`).
   > The routine therefore **deletes each variant's build dir as soon as that variant's tests pass**, so
   > peak footprint is one variant at a time; a *failed* variant keeps its dir for debugging, and the
   > `/tmp/pw_<v>.*.log` logs are always retained. Pass `--keep-builds` to retain every build dir.

   > Why push right after release? The sanitizers are slow, and this environment has rolled the container
   > back to an earlier commit mid-run before. Pushing the green release state first means the work
   > survives regardless; the sanitizer passes then only ever *add* a fix on top.

3. **Update documentation and `CLAUDE.md` — always, in the same change.** The docs must reflect
   reality:
   - Update `CLAUDE.md` whenever a change touches the language design, architecture, builtins,
     stdlib, or workflow (it is the source of truth and must always describe Kirito as it *is*).
   - Update the HTML docs: edit the relevant `docs/pages/*.md` and regenerate with
     `python3 docs/build_docs.py`.
   A feature without matching doc + CLAUDE.md updates is **not done**.

## Run it

```sh
tools/scripts/post_work_check.sh           # release -> (commit gate) -> asan -> asan-notls -> tsan
tools/scripts/post_work_check.sh --no-asan # release only (the commit gate), for a fast inner loop
```

After `release` passes, the script prints `READY TO PUSH` — that's the cue to commit and push. It then
runs `asan`, `asan-notls`, and `tsan`. Exit status is non-zero if any variant fails to build or has a
failing test; a full run is "done" only when it prints `ALL GREEN`.

## Notes

- The gate runs four presets: `release`, `asan`, `asan-notls`, `tsan` (a `debug` `-O0` preset remains
  for local iteration but is not gated; the former `strict` preset was folded into the hardened warning
  set now shared by release/asan/asan-notls/tsan). The Windows cross build is no longer part of the routine.
- The codebase is `-Wconversion`-clean; the deliberate native-binding idiom (bound-method lambdas
  whose `vm`/`self` parameters shadow the enclosing `getAttr`/`setup` — same VM by design) is silenced
  with a scoped `#pragma GCC diagnostic ignored "-Wshadow"` in the stdlib glue + runtime type-methods,
  so `-Wshadow` stays active and meaningful in the evaluator/parser/lexer/GC core.
- Optional features behind flags (e.g. `-DKIRITO_ENABLE_TLS=ON`) are not part of the default sweep;
  build them explicitly when a change touches that code path.
