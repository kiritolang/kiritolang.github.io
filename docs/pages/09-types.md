# Built-in Types

Kirito is **dynamically typed** (a name can refer to a value of any type) but **strongly typed**
(values are never silently coerced — `1 + "x"` is an error, not `"1x"`). `type(x)` returns a value's
type name as a String; `isinstance(x, T)` tests membership (inheritance-aware). `T` may be a built-in
type **constructor** (`isinstance(1, Integer)`, `isinstance("x", String)`) or the equivalent type-name
String (`isinstance(1, "Integer")`) — both work. The same goes for a typed `catch`: `catch String as
e` and `catch SomeClass as e` both match by type.

These are the built-in types. Collections (`List`, `Set`, `Dict`) hold values by reference, so two
names can share one collection and see each other's mutations.

| Type | Literal | Mutable | Hashable | Notes |
|------|---------|---------|----------|-------|
| `None` | `None` | — | yes | the absence of a value; falsy |
| `Bool` | `True` / `False` | — | yes | a subtype of nothing; falsy when `False` |
| `Integer` | `42`, `-7`, `0xFF`, `0b101` | — | yes | 64-bit, two's-complement wraparound |
| `Float` | `3.14`, `1e10`, `2e-3` | — | yes | IEEE-754 double |
| `String` | `"hi"`, `f"{x}"` | no | yes | Unicode, indexed by code point |
| `Bytes` | `Bytes([72, 105])` | no | yes | immutable raw bytes (0–255), the binary counterpart to `String` |
| `List` | `[1, 2, 3]` | yes | no | ordered sequence |
| `Set` | `{1, 2, 3}` | yes | no | insertion-ordered unique elements |
| `Dict` | `{"a": 1}` | yes | no | insertion-ordered key→value map (hashable keys) |

Each type's constructor (e.g. `Integer(x)`, `List(iter)`) is a built-in function — see
[Built-in Functions](builtins.html#types-and-conversion).

> The examples below use `io.print`; assume `var io = import("io")` at the top of each snippet.
> When a collection is **printed or nested inside another container**, its String elements show in
> quoted *repr* form — `io.print(["a", "b"])` prints `['a', 'b']` and `io.print({"k": "v"})` prints
> `{'k': 'v'}` (so an empty String element `['']` is visibly distinct from `[]`). A *bare* String
> printed on its own is unquoted: `io.print("hi")` prints `hi`.

## None

The single value `None`, returned by functions that don't `return` anything. It is falsy, equal only
to itself, and stringifies as `"None"`.

## Bool

`True` and `False`. Produced by comparisons and the logical operators, and accepted anywhere a truth
value is needed. `Bool(x)` gives the truthiness of any value: `None`, `0`, `0.0`, `""`, and empty
collections are falsy; everything else is truthy. `Bool` is a **distinct type, not an Integer**: it
is not numeric (`True + 1` is a type error) and `True != 1`, in keeping with Kirito's strong typing.
Convert explicitly with `Integer(flag)` when you need to count or sum truth values.

## Integer

Signed 64-bit integers. Arithmetic wraps on overflow with well-defined two's-complement semantics —
never undefined behavior. Literals may be decimal (`42`, `-7`), hexadecimal (`0xFF`), octal (`0o17`),
or binary (`0b1010`); the base prefix is case-insensitive. The same width applies to *literals*: a
constant wider than 64 bits keeps only its low 64 bits (`0x1_0000_0000_0000_0000` → `0`, and a decimal
literal past `9223372036854775807` wraps into the negative range) — arbitrary-precision integers are a
future enrichment. Because there is no positive counterpart to the most-negative value, `abs` of it
returns the value unchanged (`abs(-9223372036854775808)` is still negative — the one input where `abs`
can't return a non-negative result).

The arithmetic operators are `+`, `-`, `*`, the three division forms below, and `**`
(exponentiation, right-associative: `2 ** 3 ** 2 == 512`).

- `/` always produces a `Float` — even when the operands divide evenly (`7 / 2 == 3.5`,
  `4 / 2 == 2.0`); use `//` when you want an Integer result.
- `//` (floor division), `%` (modulo), and `divmod(a, b)` floor toward negative infinity, so the
  remainder takes the sign of the divisor: `divmod(-7, 3) == [-3, 2]`.
- `**` raises to a power (`2 ** 10 == 1024`), right-associative. A negative base with a *fractional*
  exponent (`(-2) ** 0.5`) has no real result and **throws** a domain error — as does zero to a
  negative power — mirroring the `math` module's `pow`/`sqrt` policy of throwing rather than returning
  a silent `NaN`.
- `bin(n)` / `oct(n)` / `hex(n)` render an Integer in base 2 / 8 / 16 as a `String`
  (`hex(255) == "0xff"`).
- Kirito has no bitwise *operators*; the builtins `bitand` / `bitor` / `bitxor` / `bitnot` and
  `shl` / `shr` provide bitwise and/or/xor/not and left/right shifts on Integers
  (`bitand(0xFF, 0x0F) == 15`, `shl(1, 8) == 256`).
- `n.compare(other, rel_tol = 1e-9, abs_tol = 0.0) → Bool` — an approximate-equality test (the
  same relative/absolute tolerance shared with [Float](#float)); handy when comparing against a Float result.

```kirito
var n = 255
io.print(hex(n), bin(5))      # "0xff" "0b101"
io.print(n // 10, n % 10)     # 25 5
io.print(2 ** 10, 7 / 2)      # 1024 3.5
```

## Float

IEEE-754 doubles, including scientific-notation literals (`1.5e3`, `2e-3`). A decimal literal needs a
digit on **both** sides of the point (`0.5` and `1.0`, not `.5` or `1.`), and digit separators
(`1_000`) aren't supported. Mixing an Integer and a Float promotes to Float. `round(x[, ndigits])`, `abs(x)`, and the `math` module operate on Floats.
Float `nan`/`inf` arise from `math` (`math.inf`, `math.nan`).

### Float equality is exact — `.compare` for tolerance

`==` / `!=` on Floats (and on an Integer compared with a Float) are **exact IEEE-754 bit equality**,
the same comparison the ordering operators use. Values that merely *round* to the same decimal can
still differ:

```kirito
io.print(0.1 + 0.2 == 0.3)        # False     (the stored doubles differ)
io.print(0.3 == 0.3)              # True       (bit-identical)
```

`NaN` is never equal to anything, including itself (`nan == nan` is `False`); `inf == inf` and
`0.0 == -0.0` are `True`. Two Floats are the same `Dict`/`Set` key iff they are `==`, and a Float
equal to an integer value hashes like that integer (`1.0` and `1` share one key).

For *approximate* comparison, call `.compare(other, rel_tol = 1e-9, abs_tol = 0.0) → Bool` on either
number — `True` when `|x - other|` is within the relative tolerance (scaled to the larger magnitude)
**or** the absolute one. Defined on both `Integer` and `Float`:

```kirito
io.print((0.1 + 0.2).compare(0.3))            # True   (close enough)
io.print((1.0).compare(1.5))                  # False  (too far apart)
io.print((1.0).compare(1.5, abs_tol = 1.0))   # True   (widened absolute tolerance)
```

## String

Immutable Unicode text, indexed and sliced by **code point** (not byte). `+` concatenates, `*`
repeats, `len` counts code points, `in` tests substrings, and iteration yields characters. Strings
are hashable (usable as Dict keys / Set elements). f-strings (`f"{expr}"`) and the `format` builtin
apply the mini-format-spec (fill/align/sign/width/precision/type); the `.format()` method does only
positional `{}`/`{N}` substitution (no format spec — see its entry below).

```kirito
var s = "café"
io.print(len(s), s[3], s[::-1])     # 4 é éfac
io.print("ab" * 3)                  # "ababab"
io.print(", ".join(["a", "b", "c"])) # "a, b, c"
```

### String methods

| Method | Description |
|--------|-------------|
| `s.upper()` | Upper-case (Unicode-aware for ASCII/Latin-1/Latin-Extended-A). A strict 1:1 code-point map — no 1:many folding (`"ß".upper()` stays `"ß"`); out-of-range scripts pass through unchanged. |
| `s.lower()` | Lower-case (Unicode-aware for ASCII/Latin-1/Latin-Extended-A). 1:1, like `upper`. |
| `s.strip([chars])` | Trim whitespace (or any of `chars`) from both ends. |
| `s.lstrip([chars])` | Trim whitespace (or any of `chars`) from the left. |
| `s.rstrip([chars])` | Trim whitespace (or any of `chars`) from the right. |
| `s.split([sep][, maxsplit])` | Split into a List; whitespace runs by default. |
| `s.join(iter)` | Join an iterable of Strings with `s` as the separator. |
| `s.replace(old, new[, count])` | Replace occurrences of `old` with `new`. |
| `s.startswith(p[, start[, end]])` | Whether `s` begins with prefix `p` (within an optional code-point window). |
| `s.endswith(p[, start[, end]])` | Whether `s` ends with suffix `p` (within an optional code-point window). |
| `s.find(sub[, start[, end]])` | First index of `sub`, or `-1` if absent. |
| `s.rfind(sub[, start[, end]])` | Last index of `sub`, or `-1` if absent. |
| `s.index(sub[, start[, end]])` | First index of `sub`; throws if absent. |
| `s.rindex(sub[, start[, end]])` | Last index of `sub`; throws if absent. |
| `s.count(sub[, start[, end]])` | Number of non-overlapping occurrences. |
| `s.format(...)` | Substitute `{}` (sequential) and `{0}`/`{1}` (indexed) fields with the positional arguments. (Named `{x}` fields and `:format-spec` like `{:05d}` are **not** supported here — use an f-string or the `format()` builtin for those.) |
| `s.isdigit()` | Whether every character is a digit. **ASCII `0`–`9` only** (`²`, `½`, fullwidth/Arabic-Indic digits are not digits here). |
| `s.isalpha()` | Whether every character is a letter. ASCII letters, plus **any code point ≥ U+0080** counts as a letter. |
| `s.isalnum()` | Whether every character is a letter or digit (same ASCII-digit / ≥U+0080-letter rules as above). |
| `s.isspace()` | Whether every character is whitespace — the **six ASCII whitespace** characters only (space, `\t`, `\n`, `\r`, `\v`, `\f`); NBSP and other Unicode spaces do not count. |
| `s.islower()` | Whether the cased characters are all lower-case. |
| `s.isupper()` | Whether the cased characters are all upper-case. |
| `s.removeprefix(p)` | Drop prefix `p` if present. |
| `s.removesuffix(p)` | Drop suffix `p` if present. |
| `s.ljust(w[, fill])` | Left-justify to width `w`, padding with `fill`. |
| `s.rjust(w[, fill])` | Right-justify to width `w`, padding with `fill`. |
| `s.center(w[, fill])` | Center within width `w`, padding with `fill`. |
| `s.zfill(w)` | Left-pad with zeros to width `w` (sign-aware). |
| `s.partition(sep)` | Split once at the first `sep` into `[head, sep, tail]`. |
| `s.rpartition(sep)` | Split once at the last `sep` into `[head, sep, tail]`. |
| `s.levenshtein(other)` | Unicode (code-point) edit distance. `other` is a String (→ `Integer`) or a List of Strings (→ a List of distances, computed in one native call). Insert/delete/substitute each cost 1. The `string` module's `similarity`/`closest`/`fuzzymatch` build on this. |
| `s.encode([encoding])` | Encode to a [`Bytes`](#bytes). `encoding` is `utf-8` (default), `latin-1`, or `ascii`. |
| `s.apply(fn)` | A new String built by concatenating `fn(ch)` over each character. `fn` takes a String and returns a String of **any** length, so this is a flat-map: a 1-char return transforms, a longer return expands (`"ab".apply(Function(c): return c + c) == "aabb"`), and `""` drops the character. |

## Bytes

An **immutable sequence of raw bytes** (0–255) — the byte-exact counterpart to
the Unicode `String`. A `String` holds UTF-8 and indexes by *code point*, so it merges valid
multi-byte sequences and cannot address arbitrary binary; `Bytes` indexes by *byte*, which is what
binary data needs (network downloads, compressed streams, file contents). Iterating yields Integers,
`b[i]` is an Integer (0–255), and slicing returns a `Bytes`. `Bytes` is hashable (a Dict/Set key),
serializable, ordered lexicographically, and supports `+` (concatenate) and `*` (repeat).

```kirito
var b = Bytes([72, 105])          # from a List of byte values
b                                 # b'Hi'
b[0]                              # 72
b[0:1]                           # b'H'
"héllo".encode("utf-8")           # b'h\xc3\xa9llo'   (é is two UTF-8 bytes)
Bytes([0xc3, 0xa9]).decode("utf-8")   # "é"
```

Construct with `Bytes(x[, encoding])` — from an iterable of Integers (0..255) — a List, but also a
Set/`range`/Dict-keys — an Integer `n` (`n` zero bytes), a String (encoded; default `utf-8`), or
another Bytes (copied) — or `fromhex("48 69")`.

| Method / function | Meaning |
| --- | --- |
| `b.decode([encoding])` | Decode to a `String` (`utf-8` default, or `latin-1`/`ascii`). Throws on bytes that aren't valid for the encoding (malformed/overlong/surrogate UTF-8; a byte ≥ 0x80 for `ascii`). |
| `b.hex()` | Lowercase hex String (`b'Hi' → "4869"`). |
| `b.apply(fn)` | A new Bytes with `fn` applied to each byte (`fn` takes/returns an Integer 0–255). |
| `fromhex(s)` | Build a Bytes from a hex String. Whitespace is allowed *between* byte pairs (`fromhex("48 69")`), but not between the two nibbles of a single byte (`fromhex("4 8")` throws); an odd number of hex digits or a non-hex character throws. |
| `len(b)`, `b[i]`, `b[a:b:c]`, `x in b` | Byte length, byte at `i`, a Bytes slice, membership (Integer byte or Bytes subsequence). |

> **latin-1 is the lossless byte↔code-point bridge**: every byte 0–255 maps to exactly one code point
> and back, so `b.decode("latin-1").encode("latin-1") == b` for any Bytes — handy when you must move
> raw bytes through a String.

## List

An ordered, mutable sequence. Supports indexing (`xs[0]`, negatives count from the end), slicing
(`xs[1:3]`, `xs[::-1]`, `xs[::2]`), `+` concatenation, `*` repetition, `in`, iteration, and
lexicographic comparison (`<`/`<=`/`>`/`>=`, compared element-by-element, then by length when one is
a prefix of the other — which makes multi-key sorts easy via a list-returning `key`).

```kirito
var xs = [3, 1, 2]
xs.append(4)
xs.sort()                  # [1, 2, 3, 4], in place, stable
io.print(xs[0], xs[-1], xs[1:3])   # 1 4 [2, 3]
```

### List methods

| Method | Description |
|--------|-------------|
| `xs.append(x)` | Add `x` to the end. |
| `xs.pop([i])` | Remove and return the last element (or index `i`). |
| `xs.insert(i, x)` | Insert `x` before index `i`. |
| `xs.remove(x)` | Remove the first element equal to `x`; throws if absent. |
| `xs.index(x[, start[, end]])` | Index of the first element equal to `x` (throws if absent), within an optional window. |
| `xs.count(x)` | Number of elements equal to `x`. |
| `xs.reverse()` | Reverse in place. |
| `xs.sort([key][, reverse])` | Sort in place, **stable**; `key` precomputed once per element. |
| `xs.extend(iter)` | Append all elements of an iterable. |
| `xs.apply(fn)` | A new List with `fn` applied to each element (like `tensor.apply`). |
| `xs.copy()` | A shallow copy. |
| `xs.clear()` | Remove all elements. |

## Set

An **insertion-ordered** collection of unique, hashable values (iteration visits elements in insertion
order, stable across deletes). Supports `in`, `len`, iteration, and the usual
set algebra. Since Kirito has no `|`/`&`/`^` operators, the operator forms are `a - b` (difference)
and `a < b`/`a <= b`/`a > b`/`a >= b` (proper-/subset, proper-/superset); union, intersection, and
symmetric difference are the methods below (`==`/`!=` compare by membership).

```kirito
var a = {1, 2, 3}
var b = {3, 4}
io.print(a.union(b), a.intersection(b))   # a 4-element set and {3} (a Set is insertion-ordered:
                                          # union keeps a's elements first, then b's new ones)
```

### Set methods

| Method | Description |
|--------|-------------|
| `s.add(x)` | Insert `x`. |
| `s.discard(x)` | Remove `x` if present (no error if absent). |
| `s.remove(x)` | Remove `x` (throws if `x` is absent — unlike `discard`). |
| `s.pop()` | Remove and return an arbitrary element. |
| `s.contains(x)` | Membership test (also `x in s`). |
| `s.union(other)` | Elements in either set. |
| `s.intersection(other)` | Elements in both. |
| `s.difference(other)` | Elements in `s` but not `other`. |
| `s.symmetricdifference(other)` | Elements in exactly one set. |
| `s.issubset(other)` | Whether every element of `s` is in `other`. |
| `s.issuperset(other)` | Whether `s` contains every element of `other`. |
| `s.isdisjoint(other)` | True if the sets share no element. |
| `s.apply(fn)` | A new Set with `fn` applied to each element (collisions collapse). |
| `s.copy()` | A shallow copy of the set. |
| `s.clear()` | Remove all elements. |

The set-algebra **methods** (`union`/`intersection`/`difference`/`symmetricdifference`/`issubset`/
`issuperset`/`isdisjoint`) accept **any iterable** for `other` — another Set, a List, a Dict (its
keys), or a String (its characters) — e.g. `{1, 2}.union([2, 3])`. The set-algebra **operators**
(`-`, `<`, `<=`, `>`, `>=`), by contrast, require a Set on the right-hand side and throw otherwise.

## Dict

An **insertion-ordered** map from hashable keys to values — iteration (`keys()`/`values()`/`items()`,
`for k in d`, and `str`) visits keys in the order they were first inserted, and that order is stable
across deletes (updating an existing key keeps its position). Supports `d[key]` get/set, `in` (over
keys), `len`, and iteration. Multi-key indexing assignment (`m[i, j] = v`) is available to types that
define it.

```kirito
var d = {"a": 1, "b": 2}
d["c"] = 3
for k, v in d.items():
    io.print(k, v)
io.print(d.get("z", 0))    # 0 (default)
```

### Dict methods

| Method | Description |
|--------|-------------|
| `d.keys()` | List of keys (in unspecified order). |
| `d.values()` | List of values. |
| `d.items()` | List of `[key, value]` pairs. |
| `d.get(key[, default])` | Value for `key`, or `default` (or `None`) if missing. |
| `d.pop(key[, default])` | Remove and return `key`'s value. |
| `d.remove(key)` | Delete `key` (throws if absent; like `pop` but returns nothing). |
| `d.popitem()` | Remove and return the **last-inserted** `[key, value]` pair (LIFO, like Python's dict). |
| `d.setdefault(key[, default])` | Get `key`, inserting `default` first if absent. |
| `d.update(other)` | Merge another Dict (or `[key, value]` pairs) in. |
| `d.apply(fn)` | A new Dict with the same keys and `fn` applied to each value (like `tensor.apply`). |
| `d.copy()` | A shallow copy of the dict. |
| `d.clear()` | Remove all entries. |

## User-defined classes

A `class` defines a new type in the same value model as the built-ins — see the
[Language Guide](language-guide.html#classes) for syntax, inheritance, and `self._super_()`.
A class plugs into the evaluator's operator/protocol dispatch by defining **special methods** (the
single-underscore dunders below), so a user type can behave exactly like a built-in. `inspect(x)`
prints a class/instance/module's public API.

## Special methods (operator overloading)

Define any of these as methods on a class to control how instances respond to operators, built-ins,
and language constructs. Each takes `self` as its first parameter. A method that isn't defined means
the corresponding operator throws a clear "no operator" / "not supported" error (except `_ne_`, which
falls back to `not _eq_`). Names use **single** leading/trailing underscores (`_add_`, not `__add__`).

### Construction and display

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_init_(self, ...)` | `ClassName(...)` (instance creation) | nothing (initializes `self`) |
| `_str_(self)` | `String(x)`, `io.print(x)`, f-strings, nested in a collection's text | a `String` |

### Arithmetic operators

Each is invoked as `x OP y` → `x._op_(y)` (left operand's method); `self` is the left operand, the
argument is the right operand. Return the result value (any type).

| Method | Operator |
|--------|----------|
| `_add_(self, other)` | `x + y` |
| `_sub_(self, other)` | `x - y` |
| `_mul_(self, other)` | `x * y` |
| `_div_(self, other)` | `x / y` |
| `_floordiv_(self, other)` | `x // y` |
| `_mod_(self, other)` | `x % y` |
| `_pow_(self, other)` | `x ** y` |

### Comparison operators

Invoked as `x OP y` → `x._op_(y)`; return a `Bool` (or any truthy/falsy value).

| Method | Operator | Notes |
|--------|----------|-------|
| `_eq_(self, other)` | `x == y` | also drives `in`, `index`, `count`, `remove` on Lists |
| `_ne_(self, other)` | `x != y` | optional — defaults to `not self._eq_(other)` |
| `_lt_(self, other)` | `x < y` | also drives `sorted()`, `min()`, `max()` |
| `_le_(self, other)` | `x <= y` | |
| `_gt_(self, other)` | `x > y` | |
| `_ge_(self, other)` | `x >= y` | |

### Unary operators

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_neg_(self)` | `-x` | the negated value (returned **raw**, any type) |
| `_not_(self)` | `not x` | whatever you return, **raw and uncoerced** (any type) |

> **`_not_` and `_neg_` do not coerce their result.** Unlike `if`/`while`/`Bool(x)` (which route through
> `_bool_` and demand a `Bool`), the `not x` and `-x` operators hand back exactly what your method
> returns — a String, a List, anything. `not obj` is therefore **not** guaranteed to be a `Bool` when the
> class defines `_not_`. Return a `Bool` from `_not_` yourself if you want boolean semantics.

### Container / indexing protocol

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_getitem_(self, key)` | `x[key]` (variadic: `x[i, j]` passes `i, j`) | the element |
| `_setitem_(self, key, value)` | `x[key] = value` (variadic keys: `m[i, j] = v`) | nothing |
| `_len_(self)` | `len(x)` | an `Integer` |
| `_contains_(self, item)` | `item in x` / `item not in x` | a truth value |
| `_iter_(self)` | `for v in x:`, and any iteration (unpacking, `List(x)`, …) | an **iterator** (an object with `_next_`, commonly `self` or a fresh iterator instance — the lazy generator protocol below), OR any plain iterable to yield (a List/Set/String — the VM iterates whatever you return) |
| `_next_(self)` | each step of iterating a `_next_`-style iterator | the next value, or `throw StopIteration()` to end |

### Lazy generators (`_iter_` / `_next_`)

A class becomes a **lazy, pull-based generator** when `_iter_` returns an object whose `_next_(self)`
yields one value per call and raises `StopIteration` at the end. Iteration then **streams** — a `for`
loop, `sum`, `sorted`, `List(...)`, or unpacking pulls one value at a time and never materializes the
whole sequence, so an **infinite** generator with `break` (or `any`/`all` short-circuiting) is bounded:

```kirito
class Count:
    var _init_ = Function(self, start):
        self.n = start
    var _iter_ = Function(self):
        return self                 # self is the iterator
    var _next_ = Function(self):
        var v = self.n
        self.n = self.n + 1
        return v                    # never raises StopIteration -> infinite

for x in Count(0):
    if x >= 3:
        break
    io.print(x)                     # 0, 1, 2
```

`StopIteration` is a built-in exception class — `throw StopIteration()` inside `_next_` ends the
iteration, and you can `catch StopIteration as e:` or `isinstance(e, StopIteration)`. **Strict
(PEP-479):** only a `StopIteration` raised at `_next_`'s own frame ends iteration; one that leaks from
a deeper call inside `_next_` surfaces as an error, so a bug can't masquerade as "iteration finished".
Returning a plain List from `_iter_` (the older, eager style) still works. The built-in
`range`/`map`/`filter`/`zip`/`enumerate` are lazy on this same seam.

### Callable and context-manager protocol

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_call_(self, ...)` | `x(...)` (calling an instance; accepts keyword arguments) | the call result |
| `_enter_(self)` | entering a `with x as v:` block | the value bound to `v` |
| `_exit_(self)` | leaving a `with` block (normally or via an exception) | ignored |

### Truthiness

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_bool_(self)` | every conditional context — `if x:`, `while x:`, `x and y`, `x or y`, `not x`, `Bool(x)`, `filter`, `all`, `any`, the conditional expression `a if x else b` | a `Bool` |

Without `_bool_`, an instance is **always truthy** — the historical, additive-safe default. Define
`_bool_` when your class has a natural notion of "empty" or "off" — an empty container, a disabled
feature flag, a nothing-to-do work item — so plain `if x:` reads correctly. The return **must be a
Bool** (not an Integer or truthy Non-Bool); a wrong return type throws with a clear message. A
throwing `_bool_` propagates through the surrounding `and`/`or`/`if` as an ordinary exception.

<!--norun (illustrative fragment)-->
```kirito
class Bag:
    var _init_ = Function(self, items):
        self.items = items
    var _bool_ = Function(self) -> Bool:
        return len(self.items) > 0

if Bag([]):
    io.print("truthy")             # NOT printed — Bag([]) is falsy via _bool_
else:
    io.print("falsy")              # printed
```

Subclasses inherit `_bool_` from a base class and can override it. `_bool_` is independent of
`_hash_`/`_eq_`/`_len_` — `len(x) == 0` does not affect truthiness unless the class opts in via
`_bool_`.

### Hashability (Set/Dict keys)

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_hash_(self)` | put an instance in a `Set` or use it as a `Dict` key; also `hash.hash(x)` | an `Integer` |

Defining `_hash_` opts an instance into hashability — otherwise a user-class instance is
**unhashable** (using one as a `Set` element or `Dict` key throws `unhashable type '<class>'`). Pair
it with `_eq_` so equal-by-`_eq_` instances share a bucket (Dict/Set lookup goes value-based); if
`_hash_` is defined *without* `_eq_`, Dict/Set still key by **identity** — every fresh instance is
a distinct key, even if `_hash_` returns the same value. See
[stdlib `hash.hash`](stdlib.html#hash) for the same primitive as a callable.

<!--norun (fragment; Point is defined here)-->
```kirito
class Point:
    var _init_ = Function(self, x, y):
        self.x = x
        self.y = y
    var _hash_ = Function(self) -> Integer:
        return self.x * 73856093 + self.y * 19349663
    var _eq_ = Function(self, other) -> Bool:
        return isinstance(other, Point) and self.x == other.x and self.y == other.y

var seen = {Point(1, 2), Point(1, 2), Point(3, 4)}   # a Set of Points, size 2
```

`_hash_` **must return an Integer**; a wrong return type throws with a clear message. A `_hash_`
whose value depends on `id()` (the arena slot id) is **not serialization-stable** — a persisted-
then-loaded instance gets fresh arena slots for its attributes. For a hash that survives
`dump`/`serialize`, fold the *content* of the identifying attributes (`hash.crc32(self.email)`,
`hash.hash(self.email)`, or a mixing polynomial over the fields).

### Limitations of operator overloading

A few deliberate boundaries:

- **Left operand only — no reflected operators.** `x + y` consults `x`'s `_add_` (and `x < y` consults
  `x`'s `_lt_`), never a right-hand `_radd_`/reflected form. `3 * v` throws even if `v` defines `_mul_`;
  put the instance on the left. (`_eq_`/`_ne_` are the exception — equality is checked symmetrically.)
- **Only `_ne_` derives from `_eq_`.** The ordering operators do **not** derive from each other — define
  each of `_lt_`/`_le_`/`_gt_`/`_ge_` you need.
- **`_exit_` takes only `self`** (no exception type/value/traceback parameters), and its return value is
  **ignored** — it cannot suppress an exception propagating out of the `with` block.
- **Slice syntax does not reach `_getitem_`.** `x[a:b:c]` uses a separate native slice protocol that
  user classes can't intercept (there is no `_slice_`); only scalar/variadic keys (`x[i]`, `x[i, j]`)
  reach `_getitem_`/`_setitem_`. Expose a normal method (e.g. `x.slice(a, b)`) for range access.
- **`_len_` does not drive truthiness.** Kirito's built-in containers are falsy when empty (`if
  xs:` on a `List`, `Dict`, `Set` is the size test), but a user class opts into that behaviour by
  defining `_bool_` explicitly — `_len_` alone doesn't change truthiness. See
  [Truthiness](#truthiness) above.

### Serialization protocol

By default `serialize`/`dump` save an instance by its attributes. Define this pair to control it
(e.g. to drop a cache or wrap a resource); see the [stdlib reference](stdlib.html#serialize).

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_getstate_(self)` | `serialize`/`dump` serializing the instance | any serializable value capturing the state |
| `_setstate_(self, state)` | `serialize`/`dump` reconstructing the instance | ignored (restores `self` from `state`) |

### Inheritance

| Method | Invoked by | Returns |
|--------|-----------|---------|
| `_super_(self)` | `self._super_()` inside a method | a parent view of `self` whose lookup starts at the base of the current method's class |

`_super_` is provided automatically; defining it is possible but discouraged (it breaks every
`self._super_()` call in the class). See the [Language Guide](language-guide.html#super-the-parent-view).

### Worked example

```kirito
class Vec:
    var _init_ = Function(self, x, y):
        self.x = x
        self.y = y
    var _add_ = Function(self, o):
        return Vec(self.x + o.x, self.y + o.y)
    var _mul_ = Function(self, k):           # scalar multiply
        return Vec(self.x * k, self.y * k)
    var _eq_ = Function(self, o):
        return self.x == o.x and self.y == o.y
    var _lt_ = Function(self, o):            # order by magnitude², enables sorting
        return self.x ** 2 + self.y ** 2 < o.x ** 2 + o.y ** 2
    var _getitem_ = Function(self, i):
        if i == 0:
            return self.x
        return self.y
    var _len_ = Function(self):
        return 2
    var _str_ = Function(self):
        return f"Vec({self.x}, {self.y})"

var a = Vec(1, 2)
var b = Vec(3, 4)
io.print(a + b)                  # Vec(4, 6)   (via _add_ + _str_)
io.print(a * 3)                  # Vec(3, 6)
io.print(a == Vec(1, 2))         # True
io.print(b[0], len(b))           # 3 2
io.print(sorted([b, a])[0])      # Vec(1, 2)   (smaller magnitude first, via _lt_)
```

