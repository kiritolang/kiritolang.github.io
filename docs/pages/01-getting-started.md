# Getting Started

## Installing (prebuilt)

If you just want to *use* Kirito, the one-line installers fetch the `ki` interpreter and the `kpm`
package manager, put launchers on your `PATH`, and create `~/.kirito/packages` (which `ki` searches
automatically):

```sh
# Linux / macOS  (installs to ~/.local/bin, no root)
curl -fsSL https://raw.githubusercontent.com/kiritolang/kiritolang.github.io/main/tools/scripts/install.sh | sh
```

```powershell
# Windows (PowerShell) — installs under %LOCALAPPDATA%\Programs\Kirito and updates your user PATH
irm https://raw.githubusercontent.com/kiritolang/kiritolang.github.io/main/tools/scripts/install.ps1 | iex
```

Prebuilt 64-bit binaries (`ki-linux-x64`, `ki-windows-x64.exe`) are also attached to each GitHub
Release for manual download. That's all you need to start — jump straight to your first script below.
Prefer to build from source? Skip to [Building from source](#building-from-source).

## Running

```
ki                      # REPL (interactive)
ki script.ki            # run a file
ki script.ki a b c      # run a file; a b c become arglist
ki --lib path/to/libs script.ki   # add an import search directory
ki -w script.ki         # disable static warnings
```

### The REPL

With no file, `ki` starts a Read-Eval-Print Loop. Type an expression to see its value; type a
statement to run it. Multi-line blocks are supported — when a line ends with `:` (opening an
indented suite), the prompt switches to `...` and keeps reading until you enter a blank line:

```
>>> var x = 41
>>> x + 1
42
>>> var f = Function():
...     return "hi"
...
>>> f()
hi
```

## Your first script

Create `hello.ki`:

```kirito
var io = import("io")
var name = io.input("What's your name? ")
io.print(f"Hello, {name}!")
```

Run it:

```
ki hello.ki
```

From here, the **[Course](course-01-hello.html)** walks you from your first line to a full program in
sixteen short lessons, and the **[Language Guide](language-guide.html)** is a one-page tour of the whole
language.

## Packages (`kpm`)

Kirito ships a package manager, **`kpm`**, that installs packages straight from GitHub (no central
index — you name an `owner/repo`) with semantic-version constraints, and can update both itself and
the `ki` interpreter. It has its own page: **[Packages & kpm](packages.html)** — installing,
versioning, and publishing.

## Building from source

### Requirements

- A C++20 compiler (GCC 13+ or Clang 18+).
- CMake 3.28+ and a generator (Ninja recommended).

The interpreter core is **header-only**; CMake builds only the `ki` executable and the tests.

### Building

```
cmake --preset debug          # or: release / asan / tsan
cmake --build build-debug
```

Presets (`CMakePresets.json`):

| Preset | What it is |
|--------|-----------|
| `debug` | `-O0` (fast compiles for the dev loop) with the hardened warning set (`-Werror -Wall -Wextra -Wconversion -Wpedantic -fstack-protector-all -Wshadow ...`) — the strictest warning gate (binary dir `build-debug`) |
| `release` | `-O2`, the looser warnings-as-errors set (no `-Wconversion`/`-Wshadow`); the build to benchmark and ship, and the gate for optimization-only warnings like `-Wmaybe-uninitialized` (`build-release`) |
| `asan` | AddressSanitizer + UBSan at `-O1` with the same hardened warnings (memory/UB checks) (`build-asan`) |
| `tsan` | ThreadSanitizer at `-O1` — the data-race + lock-order check for the multiprocessing dispatcher (`build-tsan`) |

Every preset shares **one precompiled header**: the umbrella `src/kirito.hpp` is compiled once and
reused (`REUSE_FROM`) by `ki` and all the test executables, so the header-only core is parsed a single
time per build rather than once per translation unit. A faster linker (mold, else lld) is auto-selected
when installed. The standalone binary is statically linked by default, so `build-debug/ki` (or the
release build's `build-release/ki`) is self-contained.

> Embedding Kirito in your own C++ program? The [C++ API](cpp-api.html#compiling-precompile-kirito-hpp)
> page shows how to share that same precompiled header (and why `-Winvalid-pch` matters) in your build.

### Tests

Kirito has an extensive CTest suite (unit tests, golden `.ki` scripts, error-message tests, an
adversarial/fuzz suite, and an embedding test). Run it with:

```
ctest --test-dir build-debug --output-on-failure
```

The `tools/scripts/post_work_check.sh` routine clean-builds the variants **sequentially** — `debug`,
then `release`, commit+push once both are green, then `asan` and `tsan` — running the whole suite for
each: the bar a change must clear before it's "done".
