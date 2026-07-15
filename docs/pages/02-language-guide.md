# Language Guide

A reference for Kirito's syntax and semantics.

## Comments and layout

`#` starts a line comment. Blocks use **significant indentation**: a `:` then a newline
then an indented suite. Tabs and spaces both work, but mixing them ambiguously is an error.

```kirito
var x = 5
if x > 0:
    io.print("positive")    # indented block
```

## Variables and assignment

`var` declares a name in the current scope; bare `=` rebinds the nearest existing binding (the error
`name 'X' is not defined` if it doesn't exist). Assignment binds a **name to a value** — it does not
copy.

```kirito
var a = 1
a = a + 1          # rebinds a
var b = a          # b and a are independent bindings to the same value
```

Reference semantics: `A = B` makes `A` refer to the same value as `B`; mutating a shared mutable
value (List/Dict/Set/instance) is visible through every name bound to it.

**Scoping is strict and lexical.** A name declared anywhere in a scope (with `var`, or as a
`for`/`with`/`catch`/parameter/`class` binding) is *that scope's* binding throughout — so reading or
rebinding it **before its `var` runs** is a `name 'X' is not defined` error, exactly like Python's
`UnboundLocalError`. Kirito never silently falls back to an outer variable of the same name; if you
want the outer value, use a different local name or read it before shadowing:

<!--norun (illustrative — demonstrates the error)-->
```kirito
var x = 1
var f = Function():
    var y = x        # ERROR: x names THIS function's local (declared below), read before its var ran
    var x = 2
    return y
```

## Types

Dynamically typed, strongly typed. Built-in types:

| Type | Examples |
|------|----------|
| `None` | `None` |
| `Bool` | `True`, `False` |
| `Integer` | `42`, `-7` (64-bit, two's-complement wraparound on overflow) |
| `Float` | `3.14`, `1.0` |
| `String` | `"hi"`, Unicode, code-point indexed |
| `Bytes` | raw bytes 0–255, byte indexed — `"hi".encode()`, `Bytes([104, 105])`, `fromhex("6869")` |
| `List` | `[1, 2, 3]` (ordered, mutable) |
| `Set` | `{1, 2, 3}` (unique, unordered) |
| `Dict` | `{"a": 1}` (key→value) |

`type(x)` returns the type name as a String. Constructors double as converters: `Integer("42")`,
`Float(3)`, `String(x)`, `Bool(x)`, and `List(iter)`, `Set(iter)`, `Dict(pairs)`.

A `String` is Unicode **code points** (stored UTF-8); a `Bytes` is raw **bytes** — the byte-exact
counterpart for binary data (files, network, `.gz`). Convert with `s.encode([enc])` and
`b.decode([enc])`; `b[i]` is an Integer, slicing yields `Bytes`, iteration yields Integers. See the
[Files & I/O lesson](course-13-files-io.html#text-vs-bytes-bytes-encode-decode) for when to reach for it.

Beyond these, Kirito has a native **numeric stack** — `complex` (complex numbers), `matrix` (real
matrices/vectors), and `tensor` (N-dimensional arrays with autograd) — covered in the
[bonus lessons](bonus-04-linear-algebra.html).

## Numbers and arithmetic

Division semantics: `/` always yields a `Float` (`7 / 2 == 3.5`); `//` is floor division (rounding
toward negative infinity); `%` modulo; `**` right-associative exponentiation. `%` is paired with `//`
(`a % b == a - (a // b) * b`), so its result takes the sign of the **divisor**, not the dividend.

```kirito
7 / 2       # 3.5  (Float)
7 // 2      # 3
-7 // 2     # -4   (floors toward negative infinity)
-7 % 3      # 2    (sign follows the divisor)
7 % -3      # -2
2 ** 10     # 1024
```

Integer overflow wraps (well-defined), it does not trap.
Integer literals may be written in decimal, hex (`0xFF`), octal (`0o17`), or binary (`0b1010`). A
leading zero does **not** mean octal — `007 == 7` (octal needs the `0o` prefix). A float literal needs
digits on **both** sides of the dot: `3.0` and `0.5` are floats, but `3.` and `.5` are not (and do
not parse as numbers).

There are no bitwise *operators*; use the builtins `bitand`, `bitor`, `bitxor`, `bitnot` and the
shifts `shl`, `shr` for bit manipulation on Integers:

```kirito
bitand(0xF0, 0x3C)   # 48   (0x30)
bitor(0b1000, 0b0001)  # 9
bitxor(5, 1)         # 4
bitnot(0)            # -1
shl(1, 8)            # 256
shr(-8, 1)           # -4   (arithmetic, sign-preserving)
```

## Comparisons

`<`, `<=`, `>`, `>=`, `==`, `!=` compare two values and yield a `Bool`; `in` and `not in`
(membership) parse at this same precedence. On Floats, `==` is **exact IEEE-754 bit equality** —
`0.1 + 0.2 == 0.3` is `False`. Use `.compare(other, rel_tol, abs_tol)` for tolerant comparison; see
[Float](types.html#float) for the full contract.

Comparisons **do not chain**: `1 < 2 < 3` is *not* `1 < 2 and 2 < 3`. It evaluates left-to-right as
`(1 < 2) < 3` → `True < 3`, which throws (a `Bool` has no ordering against an `Integer`). Write the
conjunction explicitly:

```kirito
1 < 2 and 2 < 3     # True   (what you meant)
```

## Strings

Unicode strings, indexed and sliced by **code point** (not byte). Concatenate with `+`, repeat with
`*`. f-strings interpolate expressions:

```kirito
var s = "chrząszcz"
len(s)              # 9 code points
s[0]                # "c"
s[::-1]             # reversed
"ab" * 3            # "ababab"
var n = 5
f"n squared is {n * n}"
```

### String literals

A string can be written with **single or double quotes** — pick whichever lets you embed the other
quote without escaping — and either spelling can be **tripled** (`'''`/`"""`) to make a **multiline**
string that runs across newlines and may contain lone quotes. Two **prefixes** modify how the body is
read: `r` makes a **raw** string (backslashes are literal — `\n` is a backslash then an `n`, not a
newline), and `f` makes an **f-string** (interpolates `{expr}`). They combine as `rf`/`fr`.

```kirito
'single quotes'                  # same as "double quotes"
"it's got an apostrophe"         # ' is just a character inside "..."
'she said "hi"'                  # and " is just a character inside '...'
"""
a multiline string
spanning several lines
"""
r"C:\Users\name\file"            # raw: no escape processing
var name = "Kirito"
f'hi {name}'                     # f-strings work with any quote style...
f"""report for {name}:
  line 2
"""                              # ...including triple-quoted, multiline
rf"raw\path\{name}"              # raw f-string: backslashes literal, {expr} still interpolated
```

Because `'...'` exists, an f-string can hold a single-quoted string key inside its braces:
`f"{d['key']}"`. Escapes in cooked (non-raw) strings: `\n \t \r \0 \\ \" \'` and `\xHH` (the code
point U+00HH from two hex digits — e.g. `"\xff"` is one code point, U+00FF, not a raw byte; use
[`Bytes`](types.html#bytes) for raw bytes). A single-line string can't span a newline (use a triple-quoted form); an
unterminated string, an unknown escape, or a raw string ending in a lone
backslash is a clear lex error. An f-string decodes escapes through the **same** decoder as a plain
string, so `f"\q"` is the same lex error as `"\q"` (and `f"\xHH"` matches `"\xHH"`).

Methods: `upper`, `lower` (Unicode-aware), `strip`/`lstrip`/`rstrip`, `split`, `join`, `replace`,
`startswith`, `endswith`, `find`/`rfind`, `index`/`rindex`, `count`, `format`, the `is...` predicates
(`isdigit`/`isalpha`/`isalnum`/`isspace`/`islower`/`isupper`), `removeprefix`/`removesuffix`,
`ljust`/`rjust`/`center`/`zfill`, `partition`/`rpartition`, and `levenshtein` — the Unicode
(code-point) edit distance to another String, or to **each** String in a List (`"kitten".levenshtein(
["sitting", "kit"])` → `[3, 3]`); the [`string`](stdlib.html#string) module builds fuzzy matching
on it.

## Collections

```kirito
var xs = [3, 1, 2]
xs.append(4)
xs.sort()                  # in-place, stable
xs[0]                      # 1
xs[1:3]                    # [2, 3]  (slicing)
len(xs)

var d = {"a": 1, "b": 2}
d["c"] = 3
d.get("z", 0)              # default if missing
for k, v in d.items():
    io.print(f"{k}={v}")

var s = {1, 2, 3}
s.add(4)
2 in s                     # True
```

Slicing supports negative indices and steps: `xs[3:-1]`, `xs[::-1]`, `xs[::2]`.

## Control flow

<!--norun (syntax skeleton with ... placeholders)-->
```kirito
if cond:
    ...
elif other:
    ...
else:
    ...

while cond:
    ...
    if done:
        break
    continue

for x in iterable:
    ...
```

Logical operators `and`/`or`/`not` short-circuit and yield an operand. `break`/`continue` outside a
loop and `return` outside a function are rejected at parse time.

**`for` iterates a snapshot.** A `for` loop captures the iterable's elements (or a dict's keys) once,
at loop start, then walks that fixed snapshot. So mutating the collection inside the loop is safe and
well-defined: appending never causes an infinite loop (the new items aren't visited this pass),
removing every element still visits all the originals (no skipping), and adding a dict key mid-loop
does **not** throw. The structural change still
applies to the collection — you just won't see it until the next loop. In-place mutation of a
nested object you still hold a reference to *is* visible, because it's the same object.

### Conditional expression

`then if cond else orelse` is an expression that yields `then` when `cond` is truthy and `orelse`
otherwise. Like `and`/`or`, it short-circuits — only the selected branch is evaluated:

<!--norun (syntax fragment, placeholder names)-->
```kirito
var label = "even" if n % 2 == 0 else "odd"
var clamped = lo if x < lo else hi if x > hi else x   # right-associative chaining
io.print("big" if x > 100 else "small")
```

It has the lowest precedence of any operator (so `a or b if c else d` parses as `(a or b) if c else
d`), and it is an expression — usable anywhere a value is, including inside lists, call arguments, and
`return`. There is no statement form; for multi-line branching use `if`/`elif`/`else`.

### `switch`

`switch` dispatches on a value against constant `case` labels — **no fallthrough**, so exactly one
arm runs:

<!--norun (syntax fragment, placeholder names)-->
```kirito
switch command:
    case "start":
        run()
    case "stop", "halt":     # multiple values per arm
        shutdown()
    case 1, 2, 3:            # cases may mix types
        handle_number()
    default:                 # optional; runs when nothing matched
        unknown()
```

Case labels are **expressions**, matched against the subject **exactly by type and value** — `case 1`
and `case 1.0` are distinct, so a constant `case 3 + 4` or even `case some_var` is allowed. In practice
you'll use constant scalars (`Integer`/`Float`/`String`/`Bool`/`None`); a label whose **runtime value**
is non-scalar throws when that arm is reached, and a non-scalar *subject* (e.g. a list) only ever
reaches `default`. The labels are tested in turn as an exact-match comparison chain. A second `default`
and an empty arm body are rejected at **parse time**; a **non-scalar label value** and a **duplicate case
value** are reported only when the `switch` is actually reached at **run time** (so a never-executed
`switch` with a non-scalar or duplicate label still loads). `break`/`continue`/`return` inside an arm
propagate to the enclosing loop/function as usual; a `switch` with no matching case and no `default` is
a no-op.

### Functions

First-class values created with `Function`. Parameters take **keyword arguments**, **default
values**, and **enforced type annotations**; the return type can be annotated too.

```kirito
var power = Function(base, exp = 2) -> Integer:
    return base ** exp

power(5)               # 25  (default exp)
power(exp = 3, base = 2)   # 8  (keyword args, any order)

var typed = Function(d : Dict) -> Float:
    return Float(len(d))
# typed([1, 2])  -> error: argument 'd' must be Dict, got List
```

A non-`None` return annotation also checks the **implicit** `None` a function returns by falling off
the end (or hitting a `return`-less branch): such a function annotated `-> Integer` throws `function
must return Integer, got None`. (`-> None` accepts an explicit `return`, `return None`, or fall-off.)

Annotations are checked at runtime (inheritance-aware for classes). `Any` or no annotation accepts
anything. Inline form: `Function(x): return x * x`.

> The **inline** body's `return` takes a single expression — an inline function does not comma-pack.
> A trailing comma in a **delimited** position belongs to the enclosing brackets: it's the next call
> argument in `map(Function(x): return x, xs)` or the next element in `[Function(): return 9, 1]`. In a
> bare packing position, though — `var f = Function(): return a, b` — the comma reads ambiguously and
> is **rejected** with a clear error (rather than silently binding `f` to `[<function>, b]`). To return
> several values from an inline function, wrap them: `Function(): return [a, b]`; or use an indented
> block body, where `return a, b` packs into a list as usual.

## Packing and unpacking

A bare comma sequence packs into a List; the left side of `=`, `var`, and `for` unpacks any iterable,
with one optional starred target absorbing the surplus. The starred target can sit **anywhere** —
at the start, middle, or end of the target list.

<!--norun (syntax skeleton with ... placeholders)-->
```kirito
var t = 1, 2, 3            # t == [1, 2, 3]
var a, b = 1, 2            # a == 1, b == 2
a, b = b, a               # swap
var first, *rest = [1, 2, 3, 4]   # first == 1, rest == [2, 3, 4]
var *init, last = [1, 2, 3, 4]    # init == [1, 2, 3], last == 4
var head, *mid, tail = [1, 2, 3, 4, 5]   # head == 1, mid == [2, 3, 4], tail == 5
for k, v in d.items():
    ...
```

## Classes

```kirito
class Animal:
    var _init_ = Function(self, name):
        self.name = name
        self._id = 0          # `_id` is private (only methods of this class chain can touch it)
    var speak = Function(self) -> String:
        return "..."

class Dog(Animal):            # inheritance
    var speak = Function(self) -> String:
        return f"{self.name} says woof"
```

Special methods use names wrapped in a **single** leading and trailing underscore: `_init_`, `_str_`,
`_add_`/`_sub_`/`_mul_`/..., `_eq_`/`_lt_`/..., `_neg_`, `_not_`, `_bool_` (opt-in truthiness —
`if x:` etc. dispatch through it), `_call_`, `_getitem_`/`_setitem_`, `_len_`,
`_contains_`, `_iter_`, `_enter_`/`_exit_`, and `_hash_` (opt-in hashability — see
[Types → Hashability](types.html#hashability-set-dict-keys)).

### `_super_()` — the parent view

`self._super_()` returns a **parent view of `self`**: the same instance, but method/attribute lookup
begins at the *base of the class whose method is currently running*. Use it to extend (rather than
replace) an inherited method, including the constructor:

<!--norun (continues the Animal class above)-->
```kirito
class Dog(Animal):
    var _init_ = Function(self, name, breed):
        self._super_()._init_(name)        # run Animal's constructor
        self.breed = breed
    var describe = Function(self) -> String:
        return self._super_().describe() + " (" + self.breed + ")"
```

Because resolution starts at the *current method's* class base, each `_super_()` climbs exactly one
level, so multi-level chains (`Puppy → Dog → Animal`) compose correctly.

`_super_()` is only meaningful when the class inherits — calling it from a class with no base throws
`_super_() called in 'X', which does not inherit from any class`.

It is technically a special method, so a class *can* define its own `_super_` and override the
default behaviour — **don't.** Overriding it discards the parent view and breaks every `_super_()`
call in that class's methods; there is no good reason to do it.

## Exceptions

The keywords `throw`/`try`/`catch`/`finally` with indented blocks. `throw` throws; `try`/`catch [Type as e]`/`finally` handles.

```kirito
class ValueError:
    var _init_ = Function(self, msg):
        self.msg = msg

try:
    throw ValueError("bad")
catch ValueError as e:
    io.print(e.msg)
finally:
    io.print("always runs")
```

Typed `catch` matches via the class chain (a subclass matches its base). A bare `catch:` catches
everything.

### `assert`

`assert CONDITION[, message]` throws a catchable error when `CONDITION` is falsy, and does nothing
when it is truthy — a compact way to state an invariant at the point it must hold. The optional
`message` (any expression) becomes the error text; without one it reads `"assertion failed"`. The
error is an ordinary exception, so a surrounding `try`/`catch` can intercept it.

```kirito
var withdraw = Function(balance, amount):
    assert amount > 0, "amount must be positive"
    assert amount <= balance, "insufficient funds"
    return balance - amount
```

## Context managers

<!--norun (reads a file that need not exist)-->
```kirito
with io.open("file.txt", "r") as f:
    for line in f:
        io.print(line)
# f is closed automatically (enter/exit protocol)
```

## Modules

`import("name")` returns a module value; access members with `.`. Modules are resolved from the
bundled stdlib, then from `.ki` files on the import path. Each module loads once per VM.

```kirito
var math = import("math")
math.sqrt(2)

var path = import("path")             # os.path + filesystem ops: join/dirname, exists/listdir, mkdir/remove
path.join("dir", "file.ki")
```

The bundled stdlib is broad — `io`/`path` (I/O and filesystem), `math`/`random`/`statistics`,
`json`/`serialize`/`dump` (data), `net`/`gzip`/`hash`/`regex`, `time`, `parallel`, and Python-flavored
`itertools`/`functools`/`collections`/`tabular`, among others. The [stdlib reference](stdlib.html)
documents every module; the [course](course-14-stdlib-tour.html) tours the highlights.

A module's members only become visible once its body has finished running, so a **circular import**
— a module that imports itself, directly or through a chain (`a` imports `b` imports `a`) — cannot
be satisfied. Kirito detects the loop and throws a clear `circular import detected: a -> b -> a`
error naming the chain, instead of looping until the stack is exhausted. Re-importing a module that
has already finished loading is fine (a diamond `a` → {`b`, `c`} → `d` loads `d` once and shares it).

### `arglist` and `argmain`

Every file runs with two names already bound in its scope:

- **`arglist`** — a List of the command-line arguments the program was launched with (`arglist[0]`
  is the first; the program name is not included). Only the file **run directly** receives the
  arguments; a file loaded via `import` gets a fresh **empty** `arglist`.
- **`argmain`** — a Bool that is `True` when this file is the one being **run directly**, and `False`
  when it was loaded by another file via `import`. Guard a file's "run me" code with `if argmain:`
  so it stays dormant when
  the file is imported as a library.

```kirito
var io = import("io")
var greet = Function(name): return f"Hello, {name}!"
if argmain:
    io.print(greet(arglist[0] if len(arglist) > 0 else "world"))
```

For parsing flags/options rather than positional `arglist`, use the `arg` module (see the standard
library reference).

## The `discard` statement and warnings

A bare expression whose non-`None` value is dropped triggers a warning (it's probably a mistake).
Use `discard EXPR` to say "I'm intentionally ignoring this result":

<!--norun (syntax fragment, placeholder names)-->
```kirito
discard validate(x)    # called for its side effect / exception; result ignored on purpose
```

A non-fatal analysis pass flags several more likely mistakes: a **local variable assigned but never
used**; a **`var` re-declared** in the same block; **unreachable code** after a `return`/`throw`/
`break`/`continue`; **self-assignment** (`x = x`); and **duplicate parameter names**. (`todo` also
deliberately emits a reminder warning at its location.) Warnings carry `file:line:col`, go to stderr,
and never stop execution; `-w` / `--no-warn` disables them.

## `pass` and `todo`

`pass` is the do-nothing statement — use it where the grammar needs a body but you have nothing to do:

<!--norun (syntax fragment, placeholder name)-->
```kirito
if condition:
    pass        # intentionally empty
```

`todo` is also a no-op at runtime, but it *deliberately* emits a warning at its own location to
remind you to come back and implement something. An optional trailing string is the reminder:

```kirito
var parse = Function(src):
    todo "handle escape sequences"
    return src
```

Running this prints `file.ki:2:5: warning: todo: handle escape sequences` to stderr (a bare `todo`
warns `todo: not yet implemented`), while the program keeps running. Like all warnings, `todo`
reminders are silenced with `-w`.
