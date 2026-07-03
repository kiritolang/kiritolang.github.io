# Packages & kpm

`kpm` is Kirito's package manager. A **package** is a bundle of `.ki` modules published in a git
repository — there is **no central index**, you install by naming a repository. **GitHub is the
default**, but any **GitLab** repository (gitlab.com or a self-hosted instance) works too. `kpm` is
itself written in Kirito (`kpm/kpm.ki`, using only the `net`, `json`, `io`, `sys`, and `semver`
modules) and ships with the interpreter; the installer drops a `kpm` launcher on your `PATH`.

## Commands

```sh
kpm install owner/repo            # install from GitHub (the default — owner/repo)
kpm install owner/repo@main       # pin a literal git ref (branch / tag / commit)
kpm install owner/repo@^1.2.0     # pin a semver constraint -> highest matching tag
kpm install gitlab.com/owner/repo # install from GitLab.com (also: gitlab:owner/repo)
kpm install https://gitlab.com/owner/repo@v2.0.0      # any full http(s) URL; host auto-detected
kpm install gitlab+https://git.acme.com/group/repo    # self-hosted GitLab (github+<url> for GHE)
kpm list                          # list installed packages
kpm update owner ... | --all      # re-resolve + reinstall (honours each recorded constraint)
kpm outdated                      # show which installed packages have a newer version
kpm remove name ...               # uninstall
kpm where                         # print the packages directory
kpm version                       # print the kpm + ki versions
kpm update-kpm  [--force]         # update kpm itself from GitHub
kpm update-ki   [--force]         # update the ki interpreter binary itself
kpm help
```

`install`/`add`/`i`, `remove`/`rm`/`uninstall`, `list`/`ls`, and `update`/`upgrade` are aliases.

## Where packages install

Packages install under `~/.kirito/packages/<name>/` and are importable directly — `import("name")` —
because `ki` automatically searches that directory, every package sub-directory, and any directory in
the `KIRITO_PATH` environment variable (PATH-style, `:`-separated on Unix, `;` on Windows), in
addition to the current directory, `--lib` directories, and the running script's own folder. `kpm
where` prints the directory.

Each install also writes a small `.kpm.json` record inside the package directory (the source repo,
the resolved git ref, the recorded version constraint, and the installed module list) so `update` and
`outdated` know how to re-resolve the package later.

## The manifest (`kirito.json`)

A package repository carries a `kirito.json` manifest **at its root**:

```json
{
  "name": "mypkg",
  "version": "1.2.0",
  "modules": ["mypkg.ki", "extra/util.ki"],
  "dependencies": ["someone/dep@^1.0.0", "other/thing"]
}
```

| Field | Meaning |
|---|---|
| `name` | The import name. Installs to `~/.kirito/packages/<name>/`, imported as `import("name")`. |
| `version` | The package's semantic version. Recorded in `.kpm.json` and shown by `kpm list`. A constraint resolves against the repo's **git tags**, not this field — see [Versioning](#versioning). |
| `modules` | Repo-relative `.ki` paths to fetch and install. Sub-paths (`extra/util.ki`) are recreated under the package directory. Each module's **importable name** is the path with `.ki` stripped and `/` → `.`, so `mypkg.ki` → `import("mypkg")` and `extra/util.ki` → `import("extra.util")`. See [Module-name collisions](#module-name-collisions). |
| `dependencies` | Other packages, each an `owner/repo` optionally with an `@constraint`. Installed first (recursively); cycles and duplicates are guarded. |

### Module-name collisions

`kpm` computes each module's **importable name** (the path with `.ki` stripped and `/` → `.`) and
refuses to install a package that would collide with:

- **another already-installed package** — the new install writes nothing, the existing one is left
  intact, and the error names both packages.
- **another package in the same install run** — a dependency (or a sibling top-level spec) that
  would install the same importable name is caught before any files land.
- **itself** — a single manifest that lists two paths mapping to the same importable name
  (e.g. `foo.bar.ki` and `foo/bar.ki`, both `import("foo.bar")`) is rejected.

Reinstalling the same package on top of itself is not a self-collision. To swap one owner for
another, `kpm remove <old>` first.

## Versioning

A package's **versions are its git tags**. When you install or update with a version constraint, `kpm`
lists the repository's tags and picks the **highest** one that satisfies the constraint, using the
[`semver`](stdlib.html#semver) module (semver.org precedence + the node-semver range grammar).

The `@ref` after `owner/repo` is either a **literal git ref** (a branch, tag, or commit sha —
`@main`, `@v2.0.0`, `@8f3a1c2` — used as-is) or a **semantic-version constraint** resolved against
the repo's tags to the highest match:

| Constraint | Means | Picks from tags `1.0.0 1.2.0 1.2.5 1.3.0 2.0.0` |
|---|---|---|
| `@1.2.3` | exactly `1.2.3` | `1.2.3` (or none) |
| `@^1.2.0` | `>=1.2.0 <2.0.0` (compatible) | `1.3.0` |
| `@~1.2.0` | `>=1.2.0 <1.3.0` (patch-level) | `1.2.5` |
| `@1.2.x` | any `1.2.*` | `1.2.5` |
| `@1.x` / `@1` | any `1.*` | `1.3.0` |
| `@*` or no `@` | the highest tag | `2.0.0` |
| `@">=1.0.0 <2.0.0"` | an explicit range (quote it — it has a space) | `1.3.0` |

`kpm` tells the two apart with `semver.validrange`: a parseable range is a constraint, anything else a
literal ref. (`@v1.2.3` works either way — it is both a real tag and an exact version.) The chosen
constraint is recorded, so `kpm update` / `kpm outdated` **re-resolve** it against tags published since.

**Prereleases** (`v1.2.0-rc.1`) are excluded from constraint matches unless your constraint pins the
same `major.minor.patch` with its own prerelease (the node-semver default) — so a plain `^1.0.0` never
silently pulls an `-rc`/`-beta` build.

## Publishing a package

There is nothing to register — publishing is just pushing to GitHub and tagging a release:

1. **Lay out the modules** — put your `.ki` files in a GitHub repo, at the root or in sub-directories.
2. **Add a `kirito.json` at the repo root** with `name`, `version`, the `modules` to install, and any `dependencies` (see the manifest table above).
3. **Commit and push** to GitHub.
4. **Tag the release** with the version — a package's versions are its git tags, and this is what `kpm`'s constraints resolve against:

```sh
git tag v1.2.0          # match your kirito.json "version" (a leading v is fine)
git push --tags
```

Users then install the latest tag with `kpm install you/yourrepo`, or pin a constraint with
`kpm install you/yourrepo@^1.2.0`.

To cut a new release, bump `version` in `kirito.json` **and** push a new tag, keeping the two in step:
the tag is what gets resolved and installed; the manifest `version` is what `kpm list` reports for the
installed copy. Follow [semver.org](https://semver.org) — bump **patch** for fixes, **minor** for
back-compatible features, **major** for breaking changes — so your users' `^`/`~` constraints behave
as expected. (`semver.inc` and `semver.diff` help compute the next version.) A package with no tags is
still installable from a branch (`@main`), but then version constraints have nothing to resolve against.

## Updating installed packages

- `kpm update <name>...` or `kpm update --all` re-reads each package's recorded constraint and
  reinstalls the highest tag that now satisfies it (a constraint-less install just refetches its ref).
- `kpm outdated` reports, per package, the installed version versus the newest available under its
  recorded constraint.

## Updating kpm and Kirito itself

`kpm` can maintain both itself and the interpreter:

- `kpm update-kpm [--force]` refreshes `kpm.ki` from GitHub (the launcher points it at the installed
  copy via `$KPM_SELF`).
- `kpm update-ki [--force]` downloads the latest release binary for your platform
  (`sys.platform`/`sys.arch`), makes it executable, and atomically swaps it in over the running
  interpreter (located via `path.executable`, overridable with `$KPM_KI_PATH`; on Windows the old
  binary is moved aside first, since a running `.exe` can't be overwritten in place).

Both compare versions first (against `sys.version` and the latest GitHub release) and **no-op when
already current** — pass `--force` to reinstall anyway.

## Repository sources (GitHub, GitLab, self-hosted)

A package source is written as one of:

| Form | Host |
|------|------|
| `owner/repo` | GitHub.com (the default) |
| `github.com/owner/repo` | GitHub.com, explicit |
| `gitlab.com/owner/repo` or `gitlab:owner/repo` | GitLab.com |
| `https://gitlab.com/owner/repo` (any full `http(s)://` URL) | host auto-detected from the domain |
| `gitlab+https://git.example.com/group/repo` | a **self-hosted** GitLab (force the host *type*) |
| `github+https://ghe.example.com/owner/repo` | a self-hosted **GitHub Enterprise** |

A trailing `.git` is ignored, a GitLab **nested group path** (`group/subgroup/repo`) is supported, and
the same forms work for a `dependencies` entry. Dependencies on GitLab are pinned and re-resolved just
like GitHub ones (the install record stores a re-parseable source). If a host's *type* can't be
inferred from its domain, prefix it with `gitlab+`/`github+` — `kpm` tells you so rather than guessing.

## Authentication & rate limits

`kpm` uses each host's REST API (to list tags / releases) and raw-file endpoints (to download
modules). Set **`$GITHUB_TOKEN`** (or `$KPM_GITHUB_TOKEN`) for GitHub — sent as a bearer token — and
**`$GITLAB_TOKEN`** (or `$KPM_GITLAB_TOKEN`) for GitLab — sent as a `PRIVATE-TOKEN` header. A token
lifts that host's anonymous rate limit and lets you install from **private** repositories. For GitHub
Enterprise (or testing), `$KPM_GITHUB_API` / `$KPM_GITHUB_RAW` override the GitHub endpoints.
