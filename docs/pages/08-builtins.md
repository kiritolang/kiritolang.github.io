# Built-in Functions

These names are available everywhere without an `import`. Each entry lists a signature
(`name(args) → ReturnType`), what it takes, and what it does.

**Keyword arguments everywhere.** Every fixed-arity builtin declares a **signature**, so it accepts
its parameters by name and in any order, with defaults — e.g. `Integer(x = "42")`,
`round(3.14159, ndigits = 2)`, `sorted(xs, reverse = True)`, `pow(2, 10, mod = 1000)`. `inspect(fn)`
prints those parameter names, types, and return type live. Genuinely **variadic** builtins
(`min`, `max`, `zip`, `range`) take a positional list and advertise themselves as `name(...)`
under `inspect`; `min`/`max` additionally accept the keyword options `key=` and `default=`.

A leading `*args` below denotes a variadic positional list; `[arg]` denotes an optional argument.

## Types and conversion

The type constructors double as converters and are keyword-callable by their
parameter name.

- `type(x) → String` — the type name of `x` (e.g. `"Integer"`, `"List"`, a user class's name).
- `Integer(x) → Integer` — convert to a 64-bit integer. Accepts `Bool` (`True`→`1`), `Float`
  (truncates toward zero; rejects NaN/∞/out-of-range), or a `String` in decimal or `0x`/`0o`/`0b`
  form (the base prefix is case-insensitive — `0X`/`0O`/`0B` also work; surrounding whitespace and a
  single leading sign allowed). Throws on anything else or an unparseable String — including a doubled
  sign or a space after the sign/prefix (`"--5"`, `"+ 5"`, `"0x-5"`).
- `Float(x) → Float` — convert to a double. Accepts `Integer`, `Bool`, or a numeric `String`
  (decimal/scientific notation, plus the special values `"inf"`/`"infinity"`/`"nan"`, case-insensitive
  and sign-prefixable). Throws if a String doesn't parse; a C99 hex-float literal (`"0x1p4"`) is
  rejected too.
- `String(x) → String` — the `str()` form of any value (the same text `io.print` would emit).
- `Bool(x) → Bool` — the truthiness of `x` (empty collections/`0`/`""`/`None` are `False`).
- `List([iterable]) → List` — an empty list, or a new list built from any iterable. `List(iterable = xs)`.
- `Set([iterable]) → Set` — an empty set, or a set of the distinct elements of an iterable.
- `Dict([iterable]) → Dict` — an empty dict, or a dict from an iterable of `[key, value]` pairs.
- `Bytes(x[, encoding]) → Bytes` — build a [`Bytes`](types.html#bytes) from a `List` of Integers (0–255),
  an Integer `n` (`n` zero bytes), a `String` (encoded; default `utf-8`, also `latin-1`/`ascii`), or
  another `Bytes`. No argument (or `None`) gives empty `Bytes`.
- `fromhex(s) → Bytes` — parse a hex `String` (spaces allowed) into `Bytes` (the inverse of the
  `b.hex()` method). E.g. `fromhex("48 69").decode() == "Hi"`.
- `isinstance(value, type) → Bool` — whether `value` is an instance of `type`. `type` may be a
  **built-in type constructor** (`isinstance(1, Integer)`, `isinstance("x", String)`), a user class
  value, or the equivalent type-name `String` (`isinstance(x, "Integer")`) — all three forms work.
  Inheritance-aware: a subclass instance satisfies a base type. (A typed `catch` accepts the same
  forms: `catch String as e`, `catch SomeClass as e`.) Note `Bool` is its **own** type, not a subtype
  of `Integer`: `isinstance(True, Integer)` is `False` (and `isinstance(1, Bool)` is `False`).
  The type-name `String` form also accepts the **pseudo-types** `"Number"` (Integer or Float),
  `"Any"` (always `True`), `"Function"`, `"Module"`, `"None"`, and a **native object's type name**
  (e.g. `isinstance(m, "Matrix")`).
- `hasattr(obj, name: String) → Bool` — whether `obj.name` resolves to an attribute or method. It is
  **existence**, not value: an attribute set to `None` still exists, so `hasattr` returns `True` for it
  (this is the whole point — it tells a present-but-`None` attribute apart from a missing one). Works
  uniformly on every value: instances (own + inherited attributes and methods, including private
  `_members` and shared class attributes), the built-in types and their methods (`hasattr("x", "upper")`,
  `hasattr([1], "append")`, `hasattr(5, "compare")`), native and `.ki` **modules** (`hasattr(math,
  "sqrt")`), and native objects. `hasattr(obj, name)` is exactly `True` iff `obj.name` would evaluate
  without throwing — so on a **class value** (which exposes no member access) it is always `False`, and
  on a plain function it is `False`. A non-String `name` throws.
- `id(x) → Integer` — a stable identity number for `x`, unique among live objects and equal for two
  references to the same object (`id(a) == id(b)` iff `a` and `b` are the same object). Used to tell
  shared references apart from equal-but-distinct values.

## Sequences and iteration

- `len(x) → Integer` — number of elements of a collection, or code points of a String.
- `range(stop) → List` / `range(start, stop[, step]) → List` — integers from `start` (default `0`)
  up to but excluding `stop`, stepping by `step` (default `1`, may be negative). Materializes a List;
  a step of `0` throws, and an over-large result throws rather than exhausting memory. (`stop` may also
  be given by the keyword `end`.)
- `enumerate(iterable[, start]) → List` — a list of `[index, value]` pairs, indices starting at
  `0` (or at `start`, e.g. `enumerate(xs, start = 1)`).
- `zip(*iterables) → List` — a list of `[a, b, …]` tuples drawn position-wise from the inputs,
  truncated to the shortest. Variadic.
- `map(function, iterable) → List` — apply `function` to every element, collecting the results.
- `filter(function, iterable) → List` — keep the elements for which `function(x)` is truthy.
- `reversed(iterable) → List` — the elements in reverse order.
- `sorted(iterable[, key][, reverse]) → List` — a new **stable**-sorted list. `key` is an optional
  function mapping each element to its comparison key (computed once per element); `reverse = True`
  sorts descending. `sorted(xs, key = len, reverse = True)`.
- `sum(iterable, start = 0) → Number` — the sum of a sequence of numbers, added onto `start`; an
  `Integer` if every element (and `start`) is an Integer, otherwise a `Float`. `sum([])` is `start`
  (`0` by default).
- `min(*args[, key][, default]) → value` / `max(*args[, key][, default]) → value` — the smallest /
  largest of a single iterable (`min(xs)`) or of several positional values (`min(a, b, c)`). `key` is
  an optional function producing the comparison key; `default` is returned when the (single) iterable
  is empty — without it, an empty sequence throws. `max(words, key = len)`.
- `all(iterable) → Bool` — `True` if every element is truthy (vacuously `True` when empty).
- `any(iterable) → Bool` — `True` if at least one element is truthy.

## Numbers

- `abs(x) → Number` — absolute value of an Integer or Float (Integer `abs` wraps two's-complement at
  `INT64_MIN`, consistent with Kirito's fixed-width integers).
- `round(x[, ndigits]) → Number` — with `ndigits` omitted (or `None`), round to the nearest
  `Integer`; with `ndigits` given, round to that many decimal places, yielding a `Float`.
  `round(pi, ndigits = 2)`. Ties round **half away from zero** (`round(0.5) == 1`, `round(-1.5) == -2`),
  not round-half-to-even. A `NaN`/infinite `x` throws in the `→ Integer` form (no `ndigits`) but passes
  through unchanged when `ndigits` is given (`round(nan, 2)` is `nan`).
- `divmod(a, b) → List` — `[a // b, a % b]` in one step, using floor semantics.
- `pow(base, exp[, mod]) → Number` — exponentiation; the 3-argument form is modular,
  `(base ** exp) % mod`, computed efficiently over non-negative Integers (`mod` must be positive and
  `exp` non-negative; both throw otherwise). `pow(2, 10, mod = 1000)`.
- `bin(n) → String` — the base-2 text of an Integer with a `0b` prefix (sign-aware).
- `oct(n) → String` — the base-8 text of an Integer with a `0o` prefix (sign-aware).
- `hex(n) → String` — the base-16 text of an Integer with a `0x` prefix (sign-aware).
- `bitand(a, b) → Integer` — bitwise AND of two Integers (Kirito has no `&` operator).
- `bitor(a, b) → Integer` — bitwise OR of two Integers (Kirito has no `|` operator).
- `bitxor(a, b) → Integer` — bitwise XOR of two Integers (Kirito has no `^` operator).
- `bitnot(a) → Integer` — bitwise NOT (`~a`, i.e. `-a - 1`).
- `shl(a, n) → Integer` — shift `a` left by `n ≥ 0` bits.
- `shr(a, n) → Integer` — shift `a` right by `n ≥ 0` bits (arithmetic, sign-preserving).
- `ord(char) → Integer` — the Unicode code point of a single-character String.
- `chr(codepoint) → String` — the single-character String for a code point.
- `format(value[, spec]) → String` — format a value with a mini-format-spec (see below).

> Float `==` is exact IEEE-754 bit equality; use `.compare` for tolerance — see
> [Float](types.html#float).

## Formatting with `format`

`format(value, spec)` applies a spec of the form `[[fill]align][sign][#][0][width][,][.precision][type]`:

```kirito
format(42, "05d")        # "00042"
format(255, "x")         # "ff"        (also X, b, o, d)
format(255, "#x")        # "0xff"      (# adds the base prefix)
format(3.14159, ".2f")   # "3.14"      (also e, g, %)
format(1234567, ",")     # "1,234,567" (thousands separator)
format(0.25, ".1%")      # "25.0%"
format("hi", "^6")       # "  hi  "    (< left, > right, ^ center)
format("x", "*^7")       # "***x***"   (custom fill)
format(-42, "+06d")      # "-00042"    (sign + zero-pad)
```

Numeric-only flags on a **String** presentation are rejected rather than silently ignored:
`format("x", "+5")`, `format("x", "#6")`, `format("x", "=6")`, and `format("x", ",")` all throw
(the sign, `#` alternate form, `=` sign-aware alignment, and `,` grouping are meaningless on a
String). Fill/align/width/precision still work on Strings.

## Introspection and modules

- `import(name) → Module` — load and return the named module (cached per VM). `import(name = "io")`.
  A circular import (a module that imports itself directly or through a chain `a → b → a`) is
  detected and throws `circular import detected: ...` naming the cycle, rather than recursing until
  the stack overflows.
- `inspect(x) → String` — a human-readable description of the public methods/attributes (with
  signatures, type annotations, and defaults) of a class, instance, module, function, or **native
  object** (e.g. a `Random`, `Matrix`, `BytesIO`, `DateTime`, regex `Pattern`/`Match`, `Socket`,
  `Response`, …) — every native type declares its members for introspection. Native
  functions/modules that declare a signature are shown the same way.

> `print` and `input` are **not** builtins — they live in the `io` module:
> `import("io").print(...)`. See the [standard library](stdlib.html#io) reference.

## Notes

- `divmod`/`//`/`%` use floor semantics — the quotient rounds toward negative infinity and the
  remainder takes the sign of the divisor: `divmod(-7, 3) == [-3, 2]`.
- `range` materializes a List, so very large ranges allocate.
- `min`/`max` throw on an empty sequence unless `default` is given; `sum([])` is `0`.
- Passing a non-iterable where an iterable is expected throws a clean `is not iterable` error.
- An unknown keyword, a duplicated argument, a missing required argument, or too many positionals all
  throw a clear, catchable error naming the function and argument.
