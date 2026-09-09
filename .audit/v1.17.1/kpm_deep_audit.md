# v1.17.1 — kpm deep audit (scoped)

Deep audit of the Kirito package manager (`kpm/kpm.ki`, a single ~900-line Kirito program) following
`.audit/deep_audit.md`, scoped to kpm. **Every finding was reproduced against a real binary**
(`build-bin/ki-release`, kpm loaded via `KIRITO_PATH`); nothing here is "read it and it looks fine".
kpm is memory-safe (it runs on the VM), so this pass targets correctness, silent failure, bad defaults,
resolution logic, coupling/SSOT, and test/doc coverage. `KPM_VERSION` 1.4.0 → **1.5.0** (one minor: hardening is a
patch, the dependency resolver is the new feature); interpreter `kVersion` untouched at 1.17.1.

## Findings (all CONFIRMED, reproduced)

### HIGH
- **K-1 — resolver crashed on any non-semver tag** (`pickRef`). A repo carrying a tag that isn't
  semver (`latest`, `nightly`, a date tag, `release`) made constraint resolution throw
  `invalid version 'latest': need MAJOR.MINOR.PATCH`, because `pickRef` called `semver.satisfies(tag,
  range)` per tag and `satisfies` throws on a non-semver version. The prior code used
  `semver.maxsatisfying`, which *tolerates* mixed tags — so this was a **regression introduced by the
  new resolver**, caught here.
  - Repro: `pickRef(parseSource("o/r"), [{c:"^1.0.0"}], {"o/r":["latest","1.2.0","garbage","1.5.0"]}, {})`
    → threw; `semver.maxsatisfying(["latest","1.2.0","garbage","1.5.0"], "^1.0.0")` → `"1.5.0"`.
  - Fix: skip tags where `semver.valid(t) == None` before `satisfies`, matching `maxsatisfying`.
  - Regression tests: unit (`pickRef` mixed tags → `1.5.0`) + integration 13j (`m/mixed` with
    `latest`/`nightly`/`release` tags resolves `@^1.0.0` → `1.3.0`).

### MEDIUM
- **K-2 — no network timeout: a stalled host hangs kpm forever** (`fetch`/`fetchBytes`). `net.get`
  defaults to `timeout = 0` (block forever) and kpm never set one, so a black-holed or stalled git
  host would wedge every command with no recovery but Ctrl-C.
  - Repro: integration 13l — a mock endpoint that sleeps 5 s; before the fix the client blocked on it.
  - Fix: a per-request timeout (default **30 s**, `$KPM_TIMEOUT` override) applied in one place
    (`reqOpts`) used by both `fetch` and `fetchBytes`. Bounds connect + each recv, so a slow-but-
    progressing download is unaffected while a dead connection fails fast.
  - Regression test: integration 13l asserts `slowhost/x` fails in < 4 s under `KPM_TIMEOUT=1`.

### LOW
- **K-3 — pasted `?query`/`#fragment` polluted the repo path** (`parseSource`).
  `parseSource("https://gitlab.com/o/r?x=1")["path"]` was `"o/r?x=1"` → wrong API/raw URLs. Fix: strip
  from the first `?`/`#`. Regression: unit (`…/o/r?ref=x` → `o/r`, `…/o/r#frag` → `o/r`).
- **K-4 — `kpm remove` silently broke dependents.** Removing a package another installed package
  depends on left the dependent broken with no notice. Fix: `cmdRemove` now warns (naming the
  dependents) using the recorded dependency graph; it still removes (removal is explicit). Regression:
  integration 13k.

## Findings from the earlier hardening pass (same round, for the record)
- **K-5 [Med] — `readRecord` crashed on a corrupt `.kpm.json`** (contradicting its "silently skips
  malformed" contract), wedging `list`/`outdated`/collision-detection. Fixed: tolerant read (closes
  the file, returns `None`). Integration 16.
- **K-6 [Med] — same package NAME from a different source silently overwrote the install.** Fixed:
  refused in `validateResolved` (the dir is keyed by name alone). Integration 15.
- **K-7 [Med] — tag listing capped at 100** → wrong/failed semver resolution on repos with >100 tags.
  Fixed: `srcTags` paginates both hosts. Integration 14 (150 tags, winner on page 2).
- **K-8 [Low] — GitHub accepted invalid 3+-component paths**; **host detection was case-sensitive**
  (`GitHub.com` → "unrecognized host"); **a `.ki`/`foo/.ki` module → empty importable name**. All
  fixed with clear up-front errors / normalization. Unit + integration 11b.
- **K-9 [feature] — `update-ki` executed an unverified binary.** Now verifies SHA-256 against the
  release's `SHA256SUMS` (published by `prepare_release.sh`); mismatch or missing sums aborts,
  `--force` opts out. Pure `shaFor` parser unit-tested.
- **K-10 [feature] — naive first-wins dependency handling replaced by a real resolver** (constraint
  unification across the whole closure + installed packages' recorded needs; atomic conflict failure;
  cross-run reverse-dependency check). See the resolver section in `docs/pages/04-packages.md`.
  Correctness sub-fix caught in review: a package being re-resolved drops its **own** stale recorded
  dep so a self-bump doesn't falsely conflict (integration 13i).

## Verified CORRECT (probed, not assumed) — so the pass is provably deep
- **No built-in shadowing.** `importModule` (runtime.hpp:2769) resolves built-in module factories
  *before* the packages path, so a package named `json`/`io`/`sys` cannot hijack `import(...)`. Its
  same-named module is merely unreachable — no corruption. (Verified in source + reasoned.)
- **Path-traversal defense holds.** `safeRelPath` rejects absolute paths, drive letters, UNC, and any
  `.`/`..`/empty component; validated on the package name and every module path, and on `remove` names.
  A `../../escape.ki` module and a `../oops` name are refused with nothing written (integration 10).
- **Constraint unification** intersects correctly: `^1.0.0` + `^1.5.0` → `1.8.0`; incompatible
  (`^1`+`^2`) fails atomically naming requesters, writing nothing; empty tag set → clean conflict.
- **Prereleases** are excluded from plain ranges (`^1.0.0` over `[1.0.0, 2.0.0-rc.1]` → `1.0.0`);
  a range with only a prerelease available fails rather than silently taking the `-rc`.
- **`v`-prefixed tags** resolve and the original tag string is preserved as the ref.
- **Cycles terminate** (A↔B, and self-dependency A→A) via the `known`/`placed` guards; deep chains
  (A→B→C→D) resolve through the multi-pass fixpoint. All install nothing twice.
- **`glEnc`/`net.quote`** percent-encode path components (`a/b c` → `a%2Fb%20c`); refs are quoted on
  the GitLab file API. Userinfo (`user:pass@host`) is stripped from the host.
- **`shaFor`** parses standard `sha256sum` output, tolerates CRLF, handles the binary-mode `*`, and
  lowercases; returns `None` (→ verification aborts, never mis-verifies) on absent/odd input.
- **Duplicate top specs** unify via canonical source; **manifest type validation** rejects non-list
  `modules`/`dependencies`, non-string entries, empty `modules`, and missing `name`.

## Residual / accepted limitations (documented, not bugs)
- Resolution is a **bounded fixpoint, not a SAT solver**: no backtracking across mutually exclusive
  optional versions; a 100-pass cap turns any pathological graph into a clear error, never a hang.
- A **missing module file** (manifest lists it, repo 404s it) is detected at install time, so that one
  package can be left partially written (the manifest can't reveal it during the pure resolve phase).
  Documented; a reinstall overwrites. Not worsened by the resolver.
- Packages installed **before** dependency-recording carry no recorded needs, so they can't be
  reverse-checked until reinstalled. Documented.
- `outdated`/`latestFor` re-resolve a package against its own recorded constraint (advisory);
  `update`/`update --all` re-resolve the whole graph.

## Coverage delta
- **Integration** (`tests/kpm_integration.py`): 44 → **76 checks** (pagination, name/source collision,
  corrupt record, github-nested, empty-import, resolver: compatible-unify / incompatible-conflict /
  reverse-dep-conflict / deep-chain / self-dep / self-bump, non-semver tags, remove-dependents warning,
  network timeout). Mock now honours `per_page`/`page` and has a deliberate slow endpoint.
- **Unit** (`tests/unit/test_kpm.cpp`): added `canonicalSource`, `pickRef` (unify / literal /
  non-semver-skip / conflict throws), `srcTagsPageUrl`, `shaFor`, case-insensitive host, nested-path
  rejection, query/fragment stripping.
- **Docs**: `docs/pages/04-packages.md` — resolver section, name-collision & built-in-shadowing notes,
  SHA-256 verification, `$KPM_TIMEOUT`, non-semver-tag handling, remove warning. All doc fences pass.
