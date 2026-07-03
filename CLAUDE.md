# CLAUDE.md - Claude Code Instructions

**Read this file in full at the start of every session before doing anything else.**
It is the source of truth for what Kirito is, how we build it, and how we work.

## Git

**ONLY commit and push to `claude-branch`.** That branch is Claude's own scratch branch and the
only place Claude's work lives while it is in flight; pushing to `main` (or any other branch) is
forbidden. The cycle is: base `claude-branch` off the current `main`, do the work, open a pull
request, wait for the human to merge; on the next task, restart `claude-branch` from `main` again.

Restart it with:

```sh
git fetch origin main
git checkout -B claude-branch origin/main
```

A PreToolUse hook (`.claude/hooks/enforce_claude_branch.py`, wired in `.claude/settings.json`)
enforces this — it blocks any `git commit` off `claude-branch` and any `git push` that touches
`main` or leaves `claude-branch`. If the hook fires, do not try to bypass it: switch to
`claude-branch` (recreating it from `origin/main` if needed) and retry.

Opening and updating a pull request from `claude-branch` is fine; no other GitHub write is.

## Versions

**A "released version" of Kirito is a git tag + published GitHub Release, nothing less.** The
`kVersion` constant in `src/kirito/version.hpp` (surfaced as `ki --version` / `sys.version`) is the
version the next release WILL carry, not the version that is out. So "bump the version to X" means
only "edit `kVersion` (+ any docs that quote it) to X" — it does NOT mean X is released, does NOT
imply a tag or a Release, and does NOT authorize either. A release happens only when the user
explicitly asks for one (build the binaries, push the tag, upload the GitHub Release). Until then
`kVersion` is just the label the in-progress work will ship under.

## What we are building

**Kirito** — a from-scratch, dynamically-typed, strongly-typed general-purpose scripting language.
Source files use the `.ki` extension. The language namespace is `Kirito`.

Main idea for the language: it should be a high-level language that's fast to develop in. We want just right level of abstraction to allow for that without demanind "boilerplate code" from the user. At the same time, Kirito is supposed to be a C++ framework - users should be able to easily implement and wire-in new objects / functions / modules in C++ Kirito's framework.

Furthemore, Kirito is expected to be capable of being extension language that can be integrated into bigger application in C++. It's therefore important that single "proccess" of Kirito is expected to be fully encapsulated in single object of KiritoVM class.

Implementation: **modern C++ (C++20)**. The execution engine is a **bytecode compiler + stack VM**
behind the AST boundary (it replaced the original tree-walking evaluator — there is no tree-walker).

Pipeline — keep these stages separate, each behind a clean interface:

```
.ki source -> Lexer -> [tokens] -> Parser -> [AST] -> Compiler -> [bytecode Proto] -> BytecodeVM -> result
```

The **AST is the stable boundary**. Treat it as a contract: the compiler depends only on the AST,
never on lexer/parser internals — it is a second AST visitor alongside the parser. The bytecode VM
reuses the entire value/object model, operator dispatch, call protocol, and GC; it owns only the
control structure (a flat instruction stream + an explicit operand stack instead of native recursion).

### Language shape (the target)

Kirito should support:

- `var` declarations; `#` line comments. **Significant indentation**: blocks are
  introduced by `:` + newline + indent (no braces).
- **Reference assignment semantics**: `A = B + C` allocates a new value, and `A`
  is bound to it. Assignment binds names to values; it does not deep-copy. `var` declares in the
  current scope; bare `=` rebinds the nearest existing binding (a `NameError` if undefined).
- **First-class functions**: `var main = Function():` followed by an indented block, called as `main()`.
  Parameters take **keyword arguments** (`f(b = 2, a = 1)`, any order) and **default values**
  (`Function(base, exp = 2):`) — keywords work **uniformly across every callable**: plain calls,
  **class instantiation** (forwarded to `_init_`: `Point(x = 1, y = 2)`), **instance/inherited/`_super_`
  method calls**, **built-in type methods** (`xs.sort(key = f, reverse = True)`, `s.split(sep = ",")`,
  `d.get(key = k, default = 0)`), **native-object methods** (matrix/regex/io/net/…), and signatured
  builtins/stdlib functions (the shared `makeMethod` helper gives any native member function keyword
  support). Parameters and the return value take optional **enforcing type
  annotations** (`Function(d : Dict) -> Float:`): unlike advisory type hints these are *checked at runtime* —
  the argument must be an instance of the named type (inheritance-aware: a subclass satisfies a base
  annotation) and the function must return that type, else a clear error. `Any` / no annotation
  accepts anything.
- **Control flow**: explicit `return` (functions default to `None`), `if`/`elif`/`else`, `while`,
  `for VAR in ITERABLE`, `break`, `continue`; logical keywords `and`/`or`/`not`; the **conditional
  expression** `THEN if COND else ORELSE` (lowest precedence, short-circuits, right-associative when
  chained). `return` outside a function and `break`/`continue` outside a loop are rejected at parse
  time.
- **Packing & unpacking**: a bare comma sequence packs into a List (`var t = 1, 2, 3`; `return a, b`
  returns `[a, b]`). The left side of `=`, `var`, and `for` unpacks any iterable — `var a, b = pair`,
  `a, b = b, a` (swap), `for k, v in d.items()` — with a single **starred** target absorbing the
  surplus (`var first, *rest = xs`, `var *init, last = xs`). Counts are checked (a clear error on
  mismatch); unpack targets may be names, indices, or members (`a[0], a[1] = x, y`).
- **Numerics**: separate `Integer` (int64) and `Float` (double); integer literals may be decimal,
  hex (`0xFF`), octal (`0o17`), or binary (`0b1010`) (case-insensitive prefix, full-width
  two's-complement), and float literals allow scientific notation (`1e10`, `1.5e3`, `2e-3`).
  **True division** — `/` always yields `Float`, `//` is
  floor division, `%` modulo, `**` right-assoc exponentiation. Integer arithmetic is fixed-width
  int64 with **well-defined two's-complement wraparound** on overflow (no UB); arbitrary-precision
  integers are a future enrichment. **Float `==`/`!=` is EXACT IEEE-754**: `0.1 + 0.2
  == 0.3` is `False`, `NaN != NaN`, `inf == inf`, `0.0 == -0.0` — so equality agrees with `<`/`>`
  (trichotomy) and with hashing (distinct-but-close floats are distinct Set/Dict keys). For
  *approximate* comparison every Integer/Float has **`.compare(other, rel_tol = 1e-9, abs_tol = 0.0)
  -> Bool`** (close when `|a - b| <= max(rel_tol * max(|a|, |b|), abs_tol)`). **The boundary rule is: ONLY `==`/`!=` are exact — every native
  numeric type (`Complex`, real `Matrix`, `Tensor`, `ComplexMatrix`/complex `Matrix`) compares
  bit-exactly with `==` and carries the same `.compare(other, rel_tol, abs_tol)` method. METHODS, by
  contrast, MAY (and should) be tolerant — `.compare` and predicates like `complex.is_zero` use a
  rel/abs epsilon; tolerance lives in methods, never in `==`.** Resource guards: huge string/list repetition, padding, and
  `range` are bounded (throw instead of OOMing); deeply nested source/data structures throw instead
  of overflowing the native stack.
- **Modules** via `import("io")`; first stdlib module is `io` (`io.input`, `io.print`).
- Built-in types, dynamically typed: `None`, `Bool`, `Integer`, `Float`, `String`,
  and collections `List`, `Set`, `Dict` (plus an internal `Array` — same value model as `List`, no
  literal/constructor exposed to Kirito). Values are hashable where it makes
  sense. **Stringification has a str-vs-repr distinction**: a bare `print(s)`/`String(s)` shows a
  String's raw text, but a String *nested in a container* prints in **repr form** (quoted + escaped):
  `print(["a", "b"])` → `['a', 'b']`, `print({"k": "v"})` → `{'k': 'v'}`, so `[""]` (→ `['']`) is
  distinguishable from `[]`. Numeric/math depth was a *later* enrichment (the general scripting core
  came first) and is now delivered: the native `matrix` and `complex` modules (complex numbers +
  real/complex matrices and vectors).
- **`Bytes`** — an immutable sequence of raw bytes (0–255): the byte-exact
  counterpart to the Unicode (code-point) `String`. `b[i]` is an Integer byte, slicing yields Bytes,
  iteration yields Integers; `+` concatenates, `*` repeats, lexicographic ordering, hashable. Convert
  with `s.encode([enc])` (String → Bytes) and `b.decode([enc])` (Bytes → String); encodings `utf-8`
  (default), `latin-1` (lossless byte↔code-point), `ascii`. `b.hex()` / `fromhex(s)`; `Bytes(x[, enc])`
  builds from a List of Integers, an Integer n (n zero bytes), a String, or a Bytes. Serializable.
  This is the right type for binary I/O — `net.get(url).content`, `io.open(path, "rb")`, gzip/zlib —
  because a String, being UTF-8, merges valid multi-byte sequences and so cannot address arbitrary
  bytes.
- **Classes**: user-defined types — `class` with methods and instance
  attributes, instantiated by calling the class. A class is just another first-class value, in the
  same value/object model as built-ins, so a C++-defined type and a Kirito `class` look alike to
  the VM. Special methods use dunder names with **single** underscores:
  `_init_`, `_str_`, `_add_`/`_sub_`/`_mul_`/`_div_`/`_floordiv_`/`_mod_`/`_pow_`,
  `_eq_`/`_ne_`/`_lt_`/`_le_`/`_gt_`/`_ge_`, `_neg_`/`_not_`, `_call_`, `_getitem_`/`_setitem_`
  (variadic keys: `m[i, j] = v`), `_len_`, `_contains_`, `_iter_`, `_enter_`/`_exit_`. Members whose
  name has a **single leading underscore and no trailing underscore** (e.g. `_count`) are **private**
  — accessible only from within a method of the same class **or a subclass** (privacy is per class
  *chain*, not per defining class — there is no name mangling). Non-function class-body
  `var`s are shared class attributes: instances read them through the class and copy-on-write on
  assignment. Parameter **defaults are evaluated at call time**, once per call in the call scope (so
  a mutable default like `xs = []` is a fresh list each call — not a shared-once footgun).
  `self._super_()` returns a *parent view*
  of self (method lookup starts at the base of the currently-running method's class) — for extending
  inherited methods/constructors; it climbs one level per call (so multi-level chains compose),
  throws if the class has no base, and is overridable (but shouldn't be).

Build the smallest thing that runs end-to-end first (lex+parse+eval an integer
literal, then arithmetic, then `var`, then functions, then `io`), and grow outward.
Every step must keep `main.ki`-style programs as the north star.

**Status:** the language is broadly implemented and tested end-to-end (`src/kirito/*.hpp`, the
`ki` runner, an extensive CTest suite incl. golden `.ki` scripts, an embedding integration test,
a stability fuzzer, and a benchmark). Working today:
- Arithmetic (true division), `var`/reference-assignment, comparisons, `in`/`not in`.
- Indentation blocks with `if/elif/else`/`while`/`for`/`break`/`continue`/`pass`/`todo`, `and`/`or`/`not`.
  Tabs and spaces both work but ambiguous mixing is rejected (measured with tab=8
  and tab=1, both must agree). Line endings are universal: the lexer normalizes CRLF and lone CR to
  LF up front, so Windows/WSL-copied (`\r\n`) sources lex identically to Unix `\n` (a stray `\r` on a
  blank line no longer corrupts the indent/dedent stream).
- `switch SUBJECT:` with `case V[, V2...]:` arms and an optional `default:` — **no fallthrough**
  (exactly one arm runs). Case labels are constant scalars (`Integer`/`Float`/`String`/`Bool`/`None`),
  matched exactly by type+value (so `case 1` ≠ `case 1.0`, and float labels match by exact value, not a
  rounded string); compiled into an exact-match comparison chain. Non-scalar subjects only reach `default`; duplicate case
  values, a second `default`, and an empty body are rejected. `case`/`default` are **soft keywords**
  (lexed as identifiers, recognized only inside a switch body) so they stay usable as ordinary names
  like a `default` parameter; only `switch` itself is reserved.
- First-class functions with closures and `return`; `assert`. Recursion is bounded by a call-depth
  guard (configurable) that throws a catchable error instead of overflowing the native stack.
- `List`/`Set`/`Dict` with literals, indexing, slicing, iteration, `in`, and methods (append/pop/
  reverse/insert/remove/index/extend/copy/clear/count; keys/values/items/get/pop/update/setdefault/
  popitem/clear; add/discard/contains/union/intersection/difference/symmetricdifference/issubset/
  issuperset/isdisjoint/pop/clear/...); `len`. **Set algebra also via operators** (Kirito has no
  `|`/`&`/`^` tokens): `-` is difference and `<`/`<=`/`>`/`>=` are proper-/subset and proper-/superset.
  Every container — `List`/`Set`/`Dict` and the
  sequences `String`/`Bytes` — has **`apply(fn)`** (like `tensor.apply`): a new container of the same
  type with `fn` mapped over the elements (over a Dict's *values*, keeping keys; over a String's
  characters; over a Bytes' bytes). Built-in containers also describe their methods under `inspect`. Lists support lexicographic ordering (`<`/`<=`/`>`/`>=`,
  element-by-element then by length) and `+` concatenation (and `*` Integer repetition,
  guarded against huge counts), enabling multi-key sorts via
  a list-returning `key`. Ordered collections have an efficient in-place
  `sort([key][, reverse])` that is **stable** by default (so is the `sorted()` builtin); keys are
  precomputed once per element.
- **Unicode** `String` (code-point indexing/slicing/iteration), `*` repetition, and methods
  (upper/lower [Unicode-aware]/strip/split/join/replace/startswith/endswith/find/rfind/index/count/
  is{digit,alpha,alnum,space,lower,upper}/removeprefix/removesuffix/ljust/rjust/center/zfill/
  — search/replace methods honor optional args (strip(chars), split(sep, maxsplit),
  replace(old, new, count), find/index/rfind/rindex/count/startswith/endswith with code-point
  [start[, end]]) — and the format mini-spec's `#` alternate form adds the 0b/0o/0x base prefix —
  partition/rpartition, and `levenshtein` (the Unicode/code-point edit distance to a String, or to
  each String in a List — computed in C++; the `string` module's `similarity`/`closest`/`fuzzymatch`
  build fuzzy matching on it) and `.format()`.
- **User-defined `class`es** with methods, attributes, inheritance, operator methods
  (`_add_`/`_str_`/`_getitem_`/...), and private `_members`.
- **Exceptions**: `try`/`catch [Type as e]`/`finally`/`throw` (typed matching via the class chain).
  Indentation-based blocks, but **C++-style keyword names** (`catch`/`throw`, not `except`/`raise`).
  A bare `catch` also catches **any `std::exception`** crossing the native boundary (surfaced as a
  catchable String), so a C++ module that throws can't escape a Kirito `try`.
- **Context managers**: `with ... as ...` (enter/exit protocol).
- **Garbage collection**: precise mark-sweep with rooted intermediates (AddressSanitizer-clean).
- **String literals** in every spelling: **single- or double-quoted** (`'x'` / `"x"` — pick one to
  embed the other quote unescaped), each **triplable** (`'''…'''` / `"""…"""`) for **multiline**
  strings that span newlines and hold lone quotes, with two combinable prefixes — `r` for **raw**
  (backslashes literal: `r"\n"` is two chars) and `f` for **f-strings** (`rf`/`fr` combine). Cooked
  escapes: `\n \t \r \0 \\ \" \'` and `\xHH`. A single-line form can't cross a newline; an
  unterminated string, a bad escape, or a raw string ending in a lone backslash is a clear lex error.
  All of this is one unified lexer routine (`Lexer::stringLiteral`).
- **f-strings** `f"{expr}"` (with optional `:format-spec` — `f"{x:05d}"`, `f"{pi:.2f}"` — and
  surrounding whitespace allowed inside the braces) in any quote style/flavour (`f'…'`, `f"""…"""`,
  raw `rf"…"`); because `'…'` strings exist, an f-string can hold a single-quoted key:
  `f"{d['k']}"`. Inline anonymous functions `Function(x): return x*x`.
- **Static warnings + `discard`**: a non-fatal analysis pass (`analyzer.hpp`) run before execution
  flags: function-local variables assigned-but-never-used; bare expression statements whose
  non-`None` value is dropped; a `var` re-declared in the same block; unreachable code after a
  return/throw/break/continue; self-assignment (`x = x`); and duplicate parameter names. `discard
  EXPR` evaluates and intentionally drops a value (suppressing the unused-result case). `todo
  [message]` is a no-op statement (like `pass`) that *deliberately* emits a `todo: ...` warning at
  its location reminding you to implement something (an optional trailing string is the reminder).
  Warnings print `file:line:col: warning: ...` to stderr; the `ki` flag `-w`/`--no-warn` disables
  them. Module-level names (exports) and class members are never flagged.
- **Builtins**: `range`, `sum`, `min`, `max`, `abs`, `round`, `sorted`, `enumerate`, `zip`, `map`,
  `filter`, `len`, `type`, `id`, `import`, `inspect`, `all`, `any`, `reversed`, `divmod`, `isinstance`
  (the type argument may be a user class, a **built-in type constructor** — `isinstance(1, Integer)` —
  or a type-name String; typed `catch` likewise matches built-in types, e.g. `catch String as e`),
  `hasattr(obj, name)` (does `obj.name` resolve? — **existence**, so an attribute that is `None` still
  counts as present; privacy-agnostic; `True` iff `obj.name` would evaluate, so uniformly `False` on a
  class value and a plain function; a non-String name throws),
  `ord`, `chr`, `bin`, `oct`, `hex`, `pow` (2- and 3-arg modular), `bitand`/`bitor`/`bitxor`/`bitnot`
  and `shl`/`shr` (bitwise ops + shifts on Integers — Kirito has no `&`/`|`/`^`/`~`/`<<`/`>>`
  operators), `format` (mini-format-spec:
  fill/align/sign/width/`,`/precision/type), and the
  `Integer`/`Float`/`String`/`Bool`/`List`/`Set`/`Dict` constructors/converters. `inspect(x)` returns
  a String describing the public methods/attributes (with signatures + annotations) of a class,
  instance, module, function, or **native object** (Random/Matrix/BytesIO/DateTime/regex/Socket/… —
  each `NativeClass` declares its members via `Object::inspectMembers()`) — **including native
  functions/modules that declare a signature**.
- **Native functions can declare a signature** (`NativeFunction` second ctor / `ModuleBuilder::fn`
  overload, taking `std::vector<NativeParam>` + a return-type string). A signatured native function
  then accepts **keyword arguments** and **defaults** (the VM binds them into the positional
  `span` the impl expects via `NativeFunction::bindArgs` — strictly by name, so out-of-order keywords
  bind correctly) and is fully described by `inspect`. Errors
  carry the module/chunk filename (`KiritoError::file`), so a parse error in an imported module
  reports that module's path, not the entry script's. **Every fixed-arity native** — builtins
  (incl. the `Integer`/`Float`/`String`/`Bool`/`List`/`Set`/`Dict` constructors) and all stdlib
  modules (io/math/random/matrix/json/serialize/dump/net/sys/time/zlib/hash) — declares a signature,
  so it accepts keyword args and `inspect` shows its full signature. Genuinely **variadic** natives
  (`min`/`max`/`zip`/`range`/`sum`/`io.print`) take a positional list and show `...` under `inspect`;
  a variadic native that also wants named options is registered as a **keyword-aware variadic**
  (`NativeFnKw` / `ModuleBuilder::kwfn` / `NativeFunction(name, NativeFnKw)`, dispatched via
  `callKw`) — e.g. `min`/`max` accept `key=`/`default=`, and `io.print` etc. accept `stream=`.
- **Standard library** (each a one-liner `vm.install<T>()`; a third party adds their own the same
  way — `#include` a header, register on the VM, no global state):
  - `io` — print/eprint/write/input/read acting on **rebindable, interchangeable streams**: the
    module-level `stdout`/`stderr`/`stdin` (with originals kept as `__stdout__`/`__stderr__`/
    `__stdin__`) can be reassigned to a file, a `BytesIO`, another std stream, or any object exposing
    `write`/`readline`/`read` (duck-typed) — so I/O redirection is just an assignment. Each of
    print/eprint/write/input/read also takes an optional **`stream=`** keyword to direct that one
    call to a specific stream without rebinding the std slots (variadic-yet-keyword-aware natives via
    `ModuleBuilder::kwfn` / `NativeFunction(NativeFnKw)`). A common
    stream protocol (`IoStream`: streamWrite/streamRead/streamReadLine/streamFlush) is implemented by
    `File`, `BytesIO`, and the std streams. `open` files & streams (read([n])/readline/readlines/
    write/writelines/seek/tell/flush, iterable line-by-line, usable as a `with` context manager); a
    **binary mode** (`"rb"`/`"wb"`/`"ab"`/`"r+b"`) makes read/readline/iteration yield `Bytes` and
    write accept `Bytes` (the stream is always byte-exact internally),
    `BytesIO` (an in-memory byte buffer with a read/write cursor; note its reads return **String** —
    Kirito Strings are byte-transparent). Module members are rebindable (`ModuleValue::setAttr`).
    `io` is **only** I/O now: streams, `open`, `print`/`eprint`/`input`/`read`/`write`, `BytesIO`.
    Everything that interprets, queries, mutates, or lists the filesystem by path lives in the `path`
    module (below), the single home for path operations — so callers never have to remember whether a
    helper is in `io` or `sys`.
  - `path` — Kirito's **os.path + os filesystem** surface: the sole home for ALL path/filesystem
    operations. Pure path-string manipulation (`join`/`dirname`/`basename`/`splitext`), read-only
    queries (`exists`/`isfile`/`isdir`/`getsize`/`listdir`/`walk`/`getcwd`), and mutation
    (`mkdir`/`remove`/`rmtree`/`rename`/`chmod`). Path strings use `/` on every platform (identical
    cross-platform; the split helpers still accept `\`). `join(*parts)` is variadic with os.path.join
    semantics (absolute `/`-component resets; needs ≥1 part — **throws on zero args**; a leading `\`
    is not absolute). Queries `exists`/`isfile`/`isdir`/`listdir`/`walk` are tolerant (missing →
    `False`/`[]`); `getsize` **throws** on a missing/non-regular path. Mutation is **strict by
    default** (throw, not silent no-op) with opt-in leniency: `mkdir(exist_ok=False)` throws if the
    dir exists; `remove`/`rmtree(missing_ok=False)` throw if the target is absent; `rmtree` is the
    recursive `rm -rf`. `mkdir`/`remove`/`rmtree` return a Bool (True=did it, False=lenient no-op),
    `rename` returns None, `chmod` is lenient (Bool). `path` also owns the filesystem *locations*
    `gettempdir()` (system temp dir) and `executable` (absolute path of the running `ki` binary) —
    moved here from `sys`, since they name a place on disk.
  - `math` — constants and the usual functions (trig/hyperbolic, exp/log, gamma/erf/erfc, floor/ceil/
    trunc, gcd/lcm, factorial, isnan/isinf, prod/comb/perm, ...). **Domain errors RAISE** a clear `math
    domain error` rather than returning silent `NaN`/`inf` rubbish (`sqrt(-1)`, `log(0)`, `asin(2)`,
    `acosh(0)`, `atanh(1)`, `gamma(0)`, `pow(-2, 0.5)`, `fmod(x, 0)`, …); a `NaN` argument passes
    through and genuine overflow-to-`inf` is not a domain error. The same policy holds across the
    numeric stack — the **`complex`** analytic set throws on the same out-of-domain inputs
    (`log`/`log10` of `0`, `atanh(±1)`, zero to a negative/complex power) and **`tensor`** element-wise
    math throws on an out-of-domain element (consistent with the tensor engine's div-by-zero guard).
  - `random` — object-based RNG (`Random(seed, generator = "xoshiro")`, no global state); dispatches
    a single distribution surface (random/uniform/randint/randrange/choice/choices/shuffle/sample/
    gauss/expovariate) through a `std::variant` over two engines — the vendored `fum::xoshiro256`
    (xoshiro256++, ~1.5–1.75× faster than MT on raw `next()` and every `<random>` distribution, the
    DEFAULT) and `std::mt19937_64` (`generator = "mersenne_twister"`). `.generator` exposes the
    active engine's name; serialize/dump tag the state with the kind so a checkpoint round-trips
    onto the same engine (`choices(population, k=1)` samples WITH replacement into a List; `choice`
    is its k=1 case unwrapped; `sample` is without replacement).
  - `tensor` — dense **N-dimensional** arrays in C++ (`tensor.hpp`, a generic `Tensor<T>` engine;
    CPU-only, GPU-ready single-buffer design). dtype **Float** (default) or **Complex**
    (the engine is generic in T). `Tensor(nested[, dtype][, requiresgrad])`/`zeros`/`ones`/`full`/`eye`/`arange`;
    `t[i,j,...]` (full index → scalar, partial → sub-tensor; negative indices count from the end) +
    assignment; **NumPy-style indexing**
    (`t[a:b:c]` axis-0 slice, `t[mask]` boolean, `t[[i,j]]` fancy, plus grad-aware `t.slice`/`t.take`);
    +,-,*,/ **element-wise** with NumPy **broadcasting** (mixed Float/Complex promotes) and scalar ops
    (`/`, `//`, `%` all **throw on a zero divisor**, like scalar arithmetic);
    element-wise comparisons (`eq/ne/lt/le/gt/ge` + the `< <= > >=` operators → 0/1 mask) and logic
    (`logicaland/or/xor/not`) — note the **whole-tensor `==` operator** returns a single Bool (same
    shape + every element **bit-exact**, NaN never equal — use `.compare(other, rel_tol, abs_tol)` for
    a tolerant whole-tensor check), distinct from the elementwise `.eq()` mask;
    `%`,`//`,`**` operators; `matmul` (2-D + batched), `dot`,
    `tensordot(a,b,axes)`/`contract` (general axis contraction), `transpose`/`permute`/`reshape`/
    `flatten`, `apply` (element-wise map), `astype`, `item()` (a one-element tensor → a Float/Complex
    scalar), `tolist()` (→ a nested Kirito List); reductions `sum`/`mean`/`prod`/`min`/`max`/
    `argmin`/`argmax`/`std`/`var`/`all`/`any`/`ptp`/`median`/`cumsum`/`cumprod` (whole or per-axis);
    selection `where`/`clip`/`maximum`/`minimum`; structural `squeeze`/`expanddims`/`swapaxes`/`flip`/
    `broadcastto`/`repeat`/`tile`/`concatenate`/`stack`/`split`; creation `linspace`/`zeroslike`/
    `oneslike`/`fulllike`/`identity`/`diag`/`tril`/`triu`; linear algebra `det`/`inv`/`solve`/`trace`/
    `norm`/`outer`/`inner`/`kron`/`cross`/`einsum`; sorting `sort`/`argsort`/`unique`/`nonzero`/
    `searchsorted`; complex helpers `real`/`imag`/`conj`/`angle`; plus a differentiable element-wise
    math set (exp/log/sqrt/cbrt/square/pow/reciprocal/abs/sign/floor/ceil/round/trunc/sin/cos/tan/
    asin/acos/atan/sinh/cosh/tanh/asinh/acosh/atanh/relu/sigmoid/softplus/erf). **Reverse-mode autograd** (Float-only, opt-in): tensors don't track gradients by
    default; mark a leaf via the `requiresgrad=True` constructor kwarg or `t.requiresgrad(True)`, and
    the differentiable ops (+,-,*,/,**, matmul, tensordot, sum/mean, transpose/reshape/permute/flatten,
    neg, where/clip/maximum/minimum, concatenate/stack/split, squeeze/expanddims/swapaxes/flip/
    broadcastto, cumsum, grad-aware slice/take, and the math set) record a computational graph;
    `t.backward([seed])` accumulates `t.grad`,
    `t.zerograd()` clears it, `t.detach()` stops gradient flow, and `with tensor.nograd():` (a context
    manager) disables tracking for a block. The graph records *operations* (not data location), so it
    carries forward to a future GPU backend. The grad-mode flag is VM-scoped (a hidden `_grad` member
    of the module, hidden from `inspect`). Tensor **arithmetic is pure** — every op returns a new
    tensor and never mutates its operands (the only in-place op is element assignment `t[i,j]=v`), so a
    gradient-descent step **rebinds** the parameter (`w = w - w.grad*lr`, re-marked `requiresgrad(True)`)
    rather than mutating in place — the functional update style of JAX/Optax, not PyTorch. The
    `matrix` and `complex` matrix types are **built on this engine** (a 2-D tensor is a matrix).
  - `matrix` — dense real matrices (a 2-D `Tensor<double>`) of arbitrary shape (no concurrency): +,-,* (matrix/scalar),
    `m[i, j]` element access/assignment, transpose, determinant, inverse, trace, apply, factories
    (zeros/ones/identity); square-only ops (determinant/inverse/trace) throw on non-square. **Vectors**
    (a Matrix with one dimension = 1): `vector(list)` factory, `dot` (scalar product; `*` stays
    matrix multiply), `cross` (3-vectors), `norm` (Euclidean 2-norm).
  - `complex` — complex numbers and complex matrices (a 2-D `Tensor<cdouble>`), all in C++ (`std::complex<double>`). `Complex(re
    [, im])`/`of(re, im)`/`real(re)`/`polar(r, θ)`; constants `i`/`zero`/`one` (genuinely
    Complex-typed, so they live here — real-axis constants like π/e/τ do not: use `math.pi`/`math.e`/
    `math.tau`, the single source of truth, and lift to the real axis via `complex.real(math.pi)` if a
    Complex value is needed);
    operators `+ - * / **` and unary `-` (Complex-on-the-left; reals coerce to the real axis; complex
    numbers are unordered so `<`/`>` throw); `.re`/`.im`, `conjugate`/`modulus`/`argument`/`norm2`/
    `is_zero`; the analytic math set (`exp`/`log`/`log10`/`sqrt`/`cbrt`/`pow` + trig/inverse-trig/
    hyperbolic/inverse-hyperbolic) as module functions over a Complex-or-number; and a complex
    `Matrix` (arbitrary shape; nested-list ctor; `m[i, j]`; +,-,* matrix/scalar;
    `transpose`/`conjugate`/`hermitian`, **`determinant` via Gaussian elimination** and **`inverse`
    via fast O(n³) Gauss-Jordan**, `trace`, factories zeros/ones/identity/`vector`, and complex
    **vector** ops `dot` [Hermitian inner product], `cross`, `norm`; `*` is matrix multiply).
    Supersedes the old
    pure-Kirito `complex.ki`/`cmatrix.ki` prototypes; the `linsolve` solver and the complex examples
    build on it.
  - `json` — parse/loads (objects → Dict; decodes \u escapes + surrogate pairs) and stringify/dumps
    (optional indent for pretty-printing).
  - `serialize` — text graph dumps/loads/save/load preserving shared references and cycles.
  - `dump` — compact BINARY serialization preserving references and cycles; `dumps(value)` returns
    the blob as **`Bytes`** and `loads(bytes)` reconstructs it (save/load persist to a file).
    `serialize` (text) and `dump` (binary) are two formats of
    the same feature: they share one graph walk + reconstruction core (`serde::flatten`/`rebuild` in
    `stdlib_serde.hpp`) and supply only their byte codec — unlike `json`, which is flat data
    interchange with no reference/cycle preservation. Both handle the built-in value types
    (None/Bool/Integer/Float/String/List/Dict/Set) **and user `class` instances** — serialized by
    attributes (reconstructed by looking the class up by name; a name→class registry is kept on the
    VM, set when a class is defined) or via the **`_getstate_`/`_setstate_`** protocol when the class
    defines it (a native C++ type opts in the same way + `vm.registerDeserializer(name, factory)`).
    The native **value** types opt in and round-trip through both formats: **Matrix/Vector**
    (`matrix`), **Complex/ComplexMatrix** (`complex`), **DateTime** (`time`), **Random** (`random` —
    restores the generator's exact stream, a reproducible checkpoint), and gradient-free **Tensor**
    (`tensor`). Resource-like natives that wrap live state (`Socket`/`Session`, open files/`BytesIO`/
    streams, compiled regex `Pattern`/`Match`) are intentionally **not** serializable and throw a
    clear, catchable error.
  - `net` — TCP sockets (connect/bind/listen/accept/send/recv/recvall/settimeout; `recv`/`recvall`
    return **`Bytes`** so binary streams stay byte-exact — `.decode()` for text — and `send` accepts a
    String or Bytes) **and** a
    full-fledged HTTP/1.1 client (requests-style): `request(method, url[, opts])` plus
    `get/post/put/delete/patch/head/options` returning a rich
    `Response` (`status`/`statuscode`/`reason`/`ok`/`url`/`text` [decoded String]/`content` [raw
    `Bytes`, for binary downloads]/`headers`/`cookies`, `json()`,
    `raiseforstatus()`, case-insensitive `header()`, and `["status"]`/`["body"]` indexing). Request
    `opts`: `headers`, `params`, `data` (string or form-Dict), `json`, `files` (multipart upload),
    `auth` (`[user, pass]` Basic), `timeout`, `allowredirects`/`maxredirects`, `verify` (TLS cert
    verification, on by default), `cookies`. Follows redirects, decodes chunked transfer-encoding,
    decompresses gzip/deflate, and parses/sends cookies. A `Session()` keeps a cookie jar + default
    headers across calls. HTTPS via `-DKIRITO_ENABLE_TLS=ON` (links OpenSSL; verifies the peer cert
    by default — trust roots come from the OS: OpenSSL's default paths/`SSL_CERT_FILE` on Unix and the
    **Windows system "ROOT" store** via CryptoAPI, since OpenSSL ships no default CA bundle there; a
    verify failure reports the actual reason). URL helpers: quote/unquote/urlencode/parseqs/urlsplit.
  - `sys` — environment (getenv/setenv/unsetenv/environ), `platform`, `arch` (x64/arm64/x86), `version`
    (the interpreter's semver string, == `ki --version`), `traceback`, `exit`, and **external-process
    execution** (the running binary's own path is `path.executable`, a filesystem location):
    `createprocess(args, cwd, input, timeout)` runs a program by argv (no shell) and `shell(command,
    cwd, input, timeout)` runs it through `/bin/sh -c` (POSIX) / `cmd.exe /c` (Windows) — both block,
    capture, and return `{code, stdout, stderr}` (stdout/stderr drained on their own threads so a
    chatty child can't deadlock; positive `timeout` kills+throws). This is for EXTERNAL programs
    (ffmpeg, git, …), distinct from `parallel`'s worker-VM model. The platform split (fork+execvp+pipe
    on POSIX, CreateProcessW+CreatePipe on Windows, incl. the Windows argv-quoting) lives in
    `proc_compat.hpp`, mirroring `net_compat.hpp`; the Kirito API is identical on every platform.
  - `time` — high-precision clocks (time/timens/monotonic/perfcounterns), sleep, and calendar
    time (`now`/`datetime`/`make`/`strptime`; `DateTime` with fields, iso/format,
    add/sub/diff arithmetic). `DateTime` has **value equality + hashing** by instant (epoch), so two
    DateTimes for the same instant compare equal and can be Dict/Set keys, and it is serializable.
  - `zlib` — compress/decompress (standard zlib streams, RFC 1950), raw deflate/inflate — a
    self-contained DEFLATE/INFLATE, no external dependency, interoperable with real zlib (the
    checksums live in `hash`). Every function accepts a `String` **or** a `Bytes` and returns the same
    type as its input, so binary streams (downloads, files) stay byte-correct via `Bytes`.
  - `gzip` — the gzip **container** (RFC 1952, `.gz` files / HTTP `Content-Encoding: gzip`): its own
    module, distinct from the bare zlib stream. `compress`/`decompress` (aliases `gzip`/`gunzip`) —
    `gzip(1)`-compatible, header flags + CRC-32 verified; String-or-Bytes in, same type out. Pair with
    `net.get(url).content` (raw `Bytes`) to fetch and unpack a `.gz` (`gzip.decompress(resp.content)`).
  - `hash` — md5/sha1/sha256 hex digests, plus the non-cryptographic checksums adler32/crc32 (Integer)
    and crc64 (CRC-64/XZ, as a signed Integer). Self-contained, standard-conformant; every function
    takes a `String` **or** a `Bytes` (so binary data hashes correctly).
  - `regex` — a from-scratch, **linear-time** regular-expression library (`regex_engine.hpp`: a
    recursive-descent parser → bytecode compiler → Thompson-NFA Pike VM with capture tracking; NO
    `std::regex`, NO backtracking, so `(a+)+b`-style patterns can't blow up). A high-level API:
    `compile`/`match`/`search`/`fullmatch`/`findall`/`finditer`/`sub` (string or callable repl)/
    `split`/`escape`, `IGNORECASE`/`MULTILINE`/`DOTALL` flags, capturing/non-capturing/named groups,
    greedy+lazy quantifiers, classes/anchors/boundaries, on Unicode code points. Backreferences and
    lookaround are deliberately rejected (they'd break the linear-time guarantee, à la RE2).
  - `parallel` — **multiprocessing** (`stdlib_parallel.hpp` + `dispatcher.hpp`): true parallelism by
    running many fully-isolated `KiritoVM`s, one per OS thread, that **share nothing** and communicate
    only by passing serialized values (the `dump` codec) through thread-safe primitives owned by the
    `KiritoDispatcher`. `spawn(fn, *args, **kwargs)` runs `fn` in a fresh worker VM (resolved by the
    function's source span, re-read from its `.ki` file — so `fn` must be file-defined; locals captured
    from an enclosing function do NOT cross — and args/result must be serializable) and returns a `Task`
    (`join`/`done`); `Queue` (put/get/putnowait/getnowait/qsize/empty/full/close — the central transfer
    primitive, cross-VM by identity) plus the coordination primitives `Lock`/`Event`/`Semaphore`/
    `Barrier` (all cross-VM by identity, all with timeouts, all woken by shutdown) and `cpucount`. Only
    the `ki` CLI installs it (every VM is built through a dispatcher via `KiritoDispatcher::configureVM`);
    a bare embedded `KiritoVM` has no `parallel`. **Deadlock-safe by construction:** every blocking op is
    a `Waitable` with an `aborted_` flag and `shutdown()` aborts them all before joining threads, so a
    worker blocked anywhere always unwinds (throwing a catchable "operation aborted"). The `net` module
    gains `socket.detach()` / `net.fromfd(fd)` to hand an accepted connection to a worker VM (same OS
    process). The example servers (`sqldb`, `webserver`) are built on this.
  - **Kirito-authored, frozen-source modules** (registered via `vm.registerSourceModule(name, src)`;
    bodies live in `stdlib_kimodules.hpp`, compiled once per VM on first import): `itertools`
    (chain/repeat/cycle/islice/accumulate/product/permutations/combinations/count/takewhile/
    dropwhile/filterfalse/compress/starmap/pairwise/ziplongest/groupby), `functools`
    (reduce/partial/cache), `collections` (deque/Counter/defaultdict), `statistics`
    (mean/median/mode/variance/stdev/multimode/quantiles/...), `string` (constants + capwords + levenshtein-based `similarity`/`closest`/`fuzzymatch`),
    `textwrap` (wrap/fill/indent/dedent), `base64` (+urlsafe), `csv` (low-level parse/format),
    `tabular` (a dataframe-style, pandas-like data-analysis library: labelled 1-D `Series` + 2-D `DataFrame`,
    `readcsv`/`tocsv` with type inference, column/`loc`/`iloc`/boolean-mask selection, element-wise
    arithmetic & comparisons, aggregations [sum/mean/min/max/std/median/...], `groupby`+`agg`,
    `sortvalues`, `merge` [inner/left/right/outer], `concat`, `describe`, `dropna`/`fillna`,
    `valuecounts`/`unique`/`apply`; numeric-only reductions treat Bool as 0/1), `xml` (an
    ElementTree-style XML parser/serializer: `parse`/`fromstring`/`tostring` + an `Element` tree with
    `tag`/`attrib`/`text`/`tail`/`children` and `find`/`findall`/`findtext`/`get`/`itertext`; handles
    attributes, entities [named + numeric], comments, CDATA, the `<?xml?>` declaration; lenient), `heapq`
    (+nlargest/heapreplace/merge), `bisect`, `copy` (copy/deepcopy), `enum`, `tee` (a `Tee`
    fan-out stream that clones writes to extra streams — e.g. stdout to a log file — plus
    `tee_stdout`/`tee_stderr` context managers that hook the std streams), `arg` (an argparse-style
    `Parser`: positional/option/flag declarations, then `parse(arglist)` -> Dict; type-converts
    options to their default's type, `-h`/`--help` prints usage and returns None), `semver` (semantic
    versioning à la semver.org + node-semver: parse/valid/clean, compare/eq/lt/gt/diff/inc, and the
    range grammar satisfies/validrange/maxsatisfying/minsatisfying/sort/rsort — `^`/`~`/comparators/
    x-ranges/hyphen ranges/AND/OR, prerelease precedence + gating; this is what `kpm` resolves
    `repo@<constraint>` with).
- **Modules** can also be `.ki` files found on the import path (`--lib <dir>`, the cwd, the
  script's directory, the `KIRITO_PATH` env var [PATH-style], and the per-user package dir
  `~/.kirito/packages` + each package sub-dir), lexed+parsed+evaluated once per VM and cached by
  resolved path. **Circular imports are detected and rejected**: a module's members are published to
  the cache only after its body finishes, so a re-entrant import of an in-progress module (a self
  import, or a chain `a → b → a`) throws a clear `circular import detected: a -> b -> a` error naming
  the cycle (tracked by both module name and resolved path) instead of recursing until the native
  stack or the call-depth guard blows; the in-progress set is unwound on failure so a later import on
  the same VM still works, and re-importing an already-finished module (a non-cyclic diamond) is fine.
  The env/package paths live in the CLI only (`src/kirito/cli_paths.hpp`,
  unit-tested), not the embeddable VM core. The `ki` CLI
  is interactive: REPL with no file (multi-line blocks via a `...` continuation prompt until a blank
  line), runs a file otherwise. Every file scope is pre-bound with **`arglist`** (the command-line
  arguments as a List — populated only in a directly-run file; **empty** in an imported module) and
  **`argmain`** (a Bool: True iff the file is run directly, False when imported — the
  run-directly-vs-imported flag). Small-integer interning, flat-vector scopes, a
  no-temporaries fast call path, and other non-invasive perf wins. **Every `Object` is allocated
  through a thread-local small-object pool** (`pool.hpp`, a segregated free-list keyed by size class up
  to 224 B — covering Int/Float/Bool/Str/List/Set/Dict/Instance/Class/Function/EnvValue): profiling a
  tight arithmetic loop put ~25% of run time in `malloc`/`free` from per-operation value boxing, and
  recycling those fixed-size blocks cut the instruction count ~25% (sum_loop) with no semantic change.
  It is thread-local (safe: one OS thread per VM, share-nothing multiprocessing) and **bypassed under
  asan/tsan** so the sanitizers still instrument every allocation. (A runtime *inline-cache* prototype
  for name lookup was rejected — it measured break-even because the flat-vector scopes already make name
  lookup cheap and the cache itself allocated; v1.9's slot-addressed locals instead resolve slots at
  COMPILE time with zero added allocation — see the bytecode-VM section below.)
- **Sample projects** in `examples/` — single-file pure-Kirito demos:
  `complex_linsolve.ki` (complex linear-system solver on `complex.ComplexMatrix` + a Gauss-Jordan
  solver in `examples/lib/linsolve.ki`), `gen_systems.ki` / `solve_systems.ki` (a producer-consumer
  pair that writes/reads system files under `/tmp`), `rpn_calculator.ki`,
  `wordcount.ki`, `stats.ki`, `trie.ki`, `todo.ki`, `domain_suffix_bench.ki` (whitelist/blacklist
  benchmark), `rule34_download.ki` (network image downloader — the only network-dependent example),
  and three `tabular`-library data-analysis demos — `tabular_iris.ki` on the bundled
  `examples/data/iris.csv`, `tabular_sales.ki`, `tabular_survey.ki`.
  Two subdirectories bundle related material:
  `examples/deep_learning/` is a tiny **PyTorch-like nn library** in pure Kirito (`lib/nn.ki`,
  atop the native `tensor` autograd) plus ten worked projects that train real models on real,
  downloaded datasets (MLP on iris, conv on digits, diabetes regression, digits autoencoder,
  breast-cancer binary, wine MLP, digits softmax, PCA on digits, k-means on iris, denoising AE);
  `examples/http_client/` is a `net` HTTP-client app + companion server + Python test harness.
  `examples/big_projects/` holds larger, multi-file programs — each also a stress test for the
  interpreter. Every project ships a self-test entry point that either matches a
  `test_<proj>.expected` byte for byte, self-asserts and prints an `ALL TESTS PASSED` /
  `N passed, 0 failed` line, or drives a live server through a Python harness:
  - `sqldb` — a networked SQL database, **concurrent** via the `parallel` actor model: a single
    DB-owner VM serializes access while a pool of connection workers handles socket I/O;
    `test_client.py` runs functional + adversarial suites and `test_concurrent.py` asserts
    consistency under K simultaneous clients.
  - `webserver` — an HTTP/1.1 server + Sinatra/Flask-style routing framework (`:name` params,
    middleware, JSON, static files), **concurrent** via a stateless `parallel` worker pool;
    `test_client.py` + `test_concurrent.py` fire parallel requests and an adversarial burst.
  - `kgrad` — a pure-Kirito tensor/autodiff/deep-learning library: strided views, reverse-mode
    autograd with a computational graph, SGD/Adam, Linear/Conv2d/BatchNorm/activations as
    PyTorch-style Modules, MSE/BCE/CE/NLL losses, Dataset/DataLoader, PCA, weight serialization,
    and a backend abstraction ready for a future GPU device — trains an MLP that solves XOR; conv
    backward is gradient-checked.
  - `imaging` — a Pillow/PIL-style image library in pure Kirito, built on the `tensor` backend:
    `Image` stores pixels as an `(H, W, C)` Float tensor, with PNG (zlib + all five scanline filters
    incl. Paeth) / PPM / PGM / BMP codecs, `convert`/`crop`/`resize` (nearest+bilinear) /
    `transpose`/`rotate`/`paste`/`point`/`split`/`merge`/`blend`, `imageops`
    (invert/grayscale/posterize/solarize/autocontrast/equalize/expand/colorize/…),
    `imagefilter` (convolution kernels + Gaussian/Box/rank filters, vectorised as edge-pad +
    shifted-window accumulate), and `imagedraw` (line/rectangle/ellipse/polygon); **plus video** —
    a baseline-JPEG decoder (`jpeg.ki`: Huffman + a tensor 8×8 IDCT + YCbCr→RGB, so `.jpg` opens and
    MJPEG decodes), a GIF decoder (`gif.ki`: LZW + palette + animation), and an OpenCV-
    `VideoCapture`-style `video.ki` reading MJPEG / animated-GIF / Y4M / image-sequence files and
    network MJPEG-over-HTTP streams (`for frame in cap`); true H.264/HEVC/RTSP is out of reach in
    pure Kirito (no codec, no subprocess) and documented as needing an external transcode to MJPEG.
    Cross-validated pixel-for-pixel against Pillow by `compare_pillow.py` /
    `compare_video_pillow.py`; its self-tests run in CTest as `script_imaging` and `script_video`.
  - `selfhost` — a **Kirito interpreter written in Kirito** (`lib/interp.ki` + `lib/lexer.ki` +
    `lib/parser.ki`, with pure-Kirito reimplementations of the stdlib under `lib/stdmods/`); `run.ki`
    is the CLI front-end, `run_tests.ki` re-runs the main project's `tools/tests/scripts/*.ki`
    golden suite through it (with a documented EXCLUDE / native-audit-prefix set for tests that
    depend on native-only surface — `parallel`, native `net.Socket`, `serialize`'s KSER1 format, the
    exhaustive `audit_/deep_/r4_/r5_/r6_/r10_/r11_` regression families, and `class_hash`).
  - `cronki` — a cron-like scheduler + one-shot batch runner in pure Kirito; parses the full
    crontab micro-language and appends every result to a JSON-lines history log.
  - `feedreader` — an RSS 2.0 + Atom feed reader with a persistent store, unread/mark-read, and
    search.
  - `kirdown` — a self-contained CommonMark-subset Markdown → HTML converter.
  - `ledger` — plain-text double-entry accounting inspired by `hledger` / `beancount` (balance,
    income, register, top, stats, csv reports).
  - `snip` — a CLI-first code-snippet manager using the `dump` binary codec for its on-disk format.
  - `sqldb_kwargs` / `webserver_kwargs` — copies of the two servers refactored so *every* call site
    passes arguments by name (built-in methods, native methods, stdlib functions included); an
    end-to-end test that keyword arguments work uniformly across every callable.

  All examples and big projects are covered by two release-binary smoke-test scripts —
  `tools/scripts/test_examples.sh` (skips the network-only `rule34_download.ki`) and
  `tools/scripts/test_big_projects.sh` (self-host is opt-in via `--selfhost` — it is slow) — both
  take `--ki PATH`; the nightly workflow runs them against the freshly-built `dist/ki-linux-x64`.

Tested under four CMake presets: **`debug`** (g++ `-O2` with the hardened warning set `-Werror
-Wall -Wextra -Wformat=2 -Wconversion -Wpointer-arith -Wpedantic -fstack-protector-all -Wreorder
-Wunused -Wshadow` — the strictest compile gate), **`release`** (the same minus `-Wconversion`/
`-Wshadow`; the build to ship/benchmark), **`asan`** (AddressSanitizer/UBSan, hardened warnings), and
**`tsan`** (ThreadSanitizer — the data-race + lock-order-inversion gate for the `parallel` dispatcher,
the only concurrent code).
The codebase is `-Wconversion`-clean; the deliberate native-binding idiom where bound-method lambdas
take `vm`/`self` parameters shadowing the enclosing `getAttr`/`setup` (same VM by design) is silenced
with a scoped `#pragma GCC diagnostic ignored "-Wshadow"` in the stdlib glue + runtime type-methods,
so `-Wshadow` stays active in the compiler/VM/parser/lexer/GC core. An 11k-input fuzzer guards
stability. Tests include an **error-message suite** (`tools/tests/errors/*.ki` + `.experr`: programs that
must fail, with the required diagnostic text) and an **adversarial suite**
(`tools/tests/unit/test_adversarial.cpp`: overflow, recursion, cyclic structures, Unicode, slicing edge
cases). The **post-work routine** (`tools/scripts/post_work_check.sh`, documented in
`.claude/POST_WORK_CHECKLIST.md`) runs the variants **sequentially** — `debug`, then `release`,
**commit+push once both are green**, then `asan` and `tsan` (fix and re-push any failure) — each a clean
build of the whole auto-discovered CTest suite. Run it before calling a change done.

**Docs:** an expandable HTML site (`docs/`) — hand-authored Markdown in `docs/pages/` rendered by
the dependency-free `docs/build_docs.py` into `docs/site/` (intro, build, embedding, extending,
language guide, a built-in **types + special-methods/operator-overloading** reference, builtins
reference, a **comprehensive per-function stdlib reference** with signatures/inputs/outputs, recipes,
a **Packages & kpm** page (installing/versioning/publishing packages — the page lives right before
the course), and a course — a **core** path of 16 lessons (Lesson 0 editor setup → Lesson 15 capstone; the basic
types/control/collections/functions material is consolidated into dense lessons) followed by 5
**bonus lessons** for specialized libraries: regex, command-line programs, tabular data, linear
algebra, and tensors+autograd). `build_docs.py` auto-anchors every documented
symbol and turns later `inline code` mentions into clickable cross-links — but only for
*unambiguous* names: a name defined in more than one place, or an instance-method name reused across
modules (`sum`/`split`/`mean`/…, detected from `receiver.name` forms in the reference pages), keeps
its anchor but is never auto-linked, so a prose mention never points at the wrong module. It also
renders Markdown indented code fences, multi-line list items, and strips `<!--comment-->` directives.
Documentation is authored in those `.md` files, NOT scraped from code comments.

Not yet done (future enrichment): comprehensions, variadic params,
generators, arbitrary-precision integers, full-Unicode case folding (current `upper`/`lower` cover
ASCII + Latin-1 + Latin Extended-A). The **bytecode compiler + stack VM** is done and is the **sole
engine** — the tree-walker is gone (`bytecode.hpp` / `compiler.hpp` / `bytecode_vm.hpp`; the
`Compiler` is a second AST visitor that lowers each body to a `Proto`, executed by the `BytecodeVM`
with an explicit GC-rooted operand stack instead of native recursion; operator/call/member semantics
are shared free functions in `runtime.hpp`). A **compile-time, scope-aware name-resolution pass**
(`resolver.hpp`, run before execution in `evalIn`/the module loaders) now throws `name 'X' is not
defined` for any reference bound to no parameter, no `var`/`for`/`class`/`catch`/`with` name in an
enclosing lexical scope, no run-scope/REPL binding, and no builtin — resolution is by scope
*membership*, so recursion/mutual-recursion/forward-references resolve, and an undefined name is a
compile error (not catchable at run time). **Slot-addressed locals are implemented** (v1.9): the
compiler assigns each function's non-captured body locals (`var`/`for`/`with`/`catch`/unpack targets +
the hidden `with` manager) a frame slot, lowered to `LoadLocal`/`StoreLocal`/`AssignLocal` — a direct
index into the call frame's operand stack (`stack_[3 + slot]`, zero added allocation) instead of a name
lookup. Captured locals (referenced by a nested function/class, found via a free-variable analysis in
`locals.hpp` shared with the resolver) and **parameters** stay name-based in the scope's `vars_`, where
closures and the call binder resolve them by name; an unwritten slot transparently falls back to a name
lookup, so semantics (closures, read-before-assign, the `var`-shadowing rule) are byte-for-byte
unchanged. Module and class bodies are never slotted (a class body harvests its methods via
`scope.locals()`; module scopes are dynamic). Two companion v1.9 wins ride along: a **numeric binary
fast path** (Integer/Float arithmetic in `applyBinaryOp` skips the virtual dispatch, delegating straight
to the shared `numericBinary` with identical wraparound/true-division/exact-compare semantics) and
**constant deduplication** (repeated scalar literals share one `consts` slot, floats keyed on exact
bits). Measured ~10% on function-local arithmetic loops; no regression on call-heavy/module-level code.

## Architecture (as built)

- **Header-only core.** The whole interpreter lives in `src/kirito/*.hpp`, surfaced through one
  umbrella header: `#include "kirito.hpp"` embeds Kirito in any C++ program (Lua-style), **no `main`**.
  The standalone interpreter's `main()` lives only in `main.cpp`. Use **`#ifndef` include guards**
  (e.g. `KIRITO_OBJECT_HPP`), **never `#pragma once`**; everything `inline`/templated, no mutable
  globals — all state is VM-scoped.
- **One `KiritoVM` = one fully-encapsulated process**, composing its owned sub-objects: an
  `ObjectArena`, the global `Environment`, and the `ModuleRegistry`. No global/static mutable state,
  so multiple VMs coexist and the whole context is serializable later. Because the arena is
  **unsynchronized**, exactly one OS thread may ever touch a given VM — so concurrency is
  **multiprocessing**: a `KiritoDispatcher` (`dispatcher.hpp`) owns the main VM plus worker VMs (one per
  thread) and the cross-VM primitives, and the `parallel` module exposes it. Workers share nothing;
  they exchange only serialized blobs through thread-safe Queues/primitives owned by the dispatcher.
- **VM-owned value graph + handles.** Every value is an `Object` owned by an arena slot
  (`unique_ptr`); everything else holds lightweight `Handle`s (slot+generation). Reference-assignment
  = two bindings sharing one handle. No `shared_ptr`, no per-value refcount. Mark-sweep GC is
  designed-for (`Object::children()`) but deferred — early on, values accumulate until the VM dies.
- **Unified object protocol.** Built-ins, C++-authored types, and Kirito `class`es all derive
  from one `Object` base exposing the same slots (`truthy/str/equals/hash`, and operation slots
  `binary/unary/call/getAttr/setAttr/getItem/setItem/iterate/length`). The VM dispatches
  through the protocol — it can't tell built-ins from user types.
- **Execution engine: bytecode.** The `Compiler` (`compiler.hpp`, a second AST visitor) lowers each
  body — a function body, the top-level program, a class body, or a parameter default — to a flat
  `Proto` (`bytecode.hpp`: an `Op` stream + constant/name/func/call/unpack/class side-tables), cached
  per body on the VM and compiled lazily (a nested function literal compiles on first call). The
  `BytecodeVM` (`bytecode_vm.hpp`) executes a `Proto` with an explicit operand stack (a region of the
  VM's GC roots) instead of native recursion. Control flow is jumps; exceptions use a runtime block
  stack; `finally`/`with`-exit on `return`/`break`/`continue` is compiled inline. Operator/call/member
  semantics live in shared free functions (`applyBinaryOp`/`applyCall`/`evalMemberGet`/… in
  `runtime.hpp`). A genuine program error the compiler finds (deep nest, invalid assignment target,
  positional-after-keyword) is thrown as a `KiritoError`, like a parser diagnostic.
- **Layered scoping**: global (built-ins) → module (per `.ki` file) → local (per function call);
  closures capture their lexical scope by handle. Only functions/modules/class-bodies introduce scopes.
- **Extending in C++**: subclass `NativeModule` (override `setup`) or `NativeClass` (override only
  the slots you need) and register with one call — indistinguishable from a built-in to the VM.
  Prefer the built-in types via the ergonomic `value.hpp` API (`Value` facade + `Args` + `List`/
  `Dict`/`Set` builders + `val(...)`; builders root intermediates for the GC). Returning built-in
  values is the default; defining a new `NativeClass` is the fallback (only for genuinely new
  behaviour). `value.hpp` is included by `native.hpp`, so every module gets it.

## Build & test

Toolchain present: `g++ 13`, `clang++ 18`, `cmake 3.28`, `ninja`, `ctest`.

- Build is **thin CMake** (out-of-source, e.g. `build/`), C++20: the header-only core is an
  `INTERFACE` target; CMake builds only `ki` (from `main.cpp`) and the test executables.
- **Cross-platform** (Linux + Windows minimum): the only platform-specific code is sockets,
  isolated behind `net_compat.hpp` (BSD sockets vs Winsock); everything else is `std::filesystem`/
  STL. CMake links `ws2_32` on Windows automatically.
- **Static linking** by default (self-contained binaries): full `-static` on GCC/Clang, static CRT
  on MSVC; TLS builds fall back to a static C++ runtime since OpenSSL is usually shared-only.
- **Install + packages**: `tools/scripts/install.sh` (Linux/macOS) and `tools/scripts/install.ps1` (Windows)
  are one-line installers that download the release binary (or build from source), place `ki`/`kpm`
  launchers on PATH, and create `~/.kirito/packages`. **`kpm/kpm.ki`** is the package manager,
  written in Kirito (uses `net`/`json`/`io`/`sys`/`semver`): `kpm install <repo>[@ref]` fetches a
  package's `kirito.json` manifest + modules into `~/.kirito/packages/<name>/`; also
  `remove`/`list`/`update`/`outdated`/`where`/`version`. **Multi-host (1.3):** `<repo>` defaults to
  GitHub (`owner/repo`) but a host-aware adapter also installs from **GitLab** and self-hosted
  instances — `gitlab.com/o/r`, `gitlab:o/r`, a full `https://…` URL (host auto-detected), or a
  `gitlab+`/`github+` prefix to force the host TYPE (self-hosted GitLab / GitHub Enterprise); GitHub
  endpoints are env-overridable (`$KPM_GITHUB_API`/`$KPM_GITHUB_RAW`). Hardening: manifest validation,
  a path-traversal guard on module paths + package names (a manifest can never write outside the
  package dir), and version-conflict warnings across the dependency graph. (`tools/tests/kpm_integration.py`
  drives the real kpm under `ki` against a localhost GitHub/GitLab mock — install/deps/semver/update/
  remove/conflict + every misconfig failure mode.) Dependencies are other repos (each optionally
  `@<constraint>`). An `@ref` is either
  a **literal git ref** (`@main`, a sha) or a **semver constraint** (`@^1.2.0`, `@1.x`, `@">=1 <2"`)
  resolved against the repo's tags by the `semver` module (`validrange` tells them apart;
  `maxsatisfying` picks the highest matching tag); the chosen constraint is recorded in `.kpm.json`
  so `update`/`outdated` re-resolve it. **Self-maintenance**: `kpm update-kpm` refreshes `kpm.ki`
  from GitHub (path via `$KPM_SELF`, set by the launcher), and `kpm update-ki` downloads the latest
  release binary for `sys.platform`/`sys.arch`, `io.chmod`s it executable, and atomically swaps it in
  over the running interpreter (`path.executable` / `$KPM_KI_PATH`; Windows moves the old exe aside
  first) — both version-check against `sys.version` and no-op when current (`--force` overrides).
  `$GITHUB_TOKEN`/`$KPM_GITHUB_TOKEN` (GitHub bearer) and `$GITLAB_TOKEN`/`$KPM_GITLAB_TOKEN` (GitLab
  `PRIVATE-TOKEN`) lift each host's rate limit + reach private repos. The standalone `ki` also gains
  `-v`/`--version`.
- Tests run under **CTest**. **Every language feature gets a test.** Prefer many
  small, focused tests (one behavior each) over large ones. A feature isn't done
  until it has a test and the suite is green.
- **Release-binary smoke tests** live alongside the C++ suite: `tools/scripts/test_examples.sh`
  runs every runnable `examples/*.ki` against a chosen `ki` binary (with the right `--lib` flags
  for `complex_linsolve.ki` / `solve_systems.ki`; skips the network-only `rule34_download.ki`), and
  `tools/scripts/test_big_projects.sh` covers all three big-project testing conventions in one place
  — golden-`.expected` (cronki, feedreader, kirdown, ledger, snip), self-asserting Kirito assertions
  (imaging + imaging/video, kgrad + kgrad/extra), and the Python harnesses
  `test_client.py` + `test_concurrent.py` for sqldb, sqldb_kwargs, webserver, webserver_kwargs. Both
  take `--ki PATH`; the big-projects script keeps the slow self-host suite opt-in behind
  `--selfhost`.
- **Nightly CI** (`.github/workflows/nightly.yml`) runs at 03:00 UTC on `main` with a
  fresh-commits gate — a small "gate" job compares `HEAD` against the last completed Nightly run and
  skips the heavy job when they match; a `workflow_dispatch` always runs. The heavy job runs
  `post_work_check.sh` (debug + release + asan + tsan), `build_all.sh` (Linux native + Windows
  cross-compile via mingw-w64) and `test_release.sh`, then chains `test_examples.sh` +
  `test_big_projects.sh` against the shipped `dist/ki-linux-x64`, and uploads the release binaries
  as an artifact. All compilation happens on Linux.
- Before claiming something works, actually build and run it; report real output.

## Code style & working directives

- **Terse but self-explanatory.** Names carry the meaning. Comment the *why*, never
  the *what*; if code needs a comment to say what it does, rewrite the code.
- Small, single-responsibility units. Use abstraction only responsibly, where 
  you expect extension will be required later.
- **Keep code DRY**. If you do something three Times or more, it should probably be a 
  separate function.
- Match the style of surrounding code: naming, structure, idiom.
- **Kirito's public surface uses lowercase, no-underscore names** — every Kirito-visible function and
  method (builtins, stdlib module functions, type methods) is all lowercase with no underscores or
  camelCase (`gettempdir`, `splitext`, `startswith`, `symmetricdifference`, `httpget`, `timens`).
  This is the language convention; keep new names consistent with it. (C++ identifiers still follow
  ordinary C++ style; this rule is about names exposed to Kirito code.)
- Clear diagnostics: lexer/parser/runtime errors should carry line and column and a
  message a user can act on. Errors are part of the language, not an afterthought.
- Prefer the standard library and plain data structures over cleverness.
- C++: use STL and modern standard (C++20). Never expose raw pointers. Everywhere 
  it is possible favor references, if reference can't be used, use smart pointers.
  Favor std::unique_ptr over std::shared_ptr.
- In general, objects shouldn't share attributes. If B belongs to A, then A "owns" B
  and this gets messy when C that's not part of A has reference to B. Variables in
  kirito code can get and use references like this, but it must not be ingrained in 
  language internals, we want clean separation so in the end it's easy to save&load 
  context. 

## Keep this file current

When a decision changes the language design, architecture, or workflow, update this
file in the same change. It must always describe Kirito as it actually is.
