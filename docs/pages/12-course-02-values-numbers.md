# Lesson 2 — Values, Variables & Numbers

Every program shuffles **values** around. Kirito has a small set of built-in value types and a clear,
predictable model for the **names** that refer to them. This lesson covers that model, then dives
into the type you'll use most: numbers.

## The built-in values

```kirito
var io = import("io")

var age = 30            # Integer  — whole numbers
var price = 9.99        # Float    — numbers with a fractional part
var name = "Ada"        # String   — text
var is_admin = True     # Bool     — True or False
var nothing = None      # None     — the "no value" value

io.print(type(age), type(price), type(name), type(is_admin), type(nothing))
# => Integer Float String Bool None
```

`type(x)` returns the name of a value's type as a String — useful when you're not sure what you're
holding.

## `var` declares, `=` updates

`var name = value` **declares** a new name in the current scope. Once declared, a plain `=` (no
`var`) **updates** the existing binding:

```kirito
var io = import("io")
var counter = 0         # declare
counter = counter + 1   # update — counter is now 1
counter = counter + 1   # update — counter is now 2
io.print(counter)       # => 2
```

Using `=` on a name that was never declared is an error (`name 'total' is not defined`), not a silent
new variable — this catches typos early:

<!--norun (demonstrates an intentional error)-->
```kirito
total = 5               # error: name 'total' is not defined  (you meant `var total = 5`)
```

## Names are bindings, not boxes

This is the one idea to internalize. Assignment **binds a name to a value**; it does not copy the
value. When two names are bound to the same mutable value, they see each other's changes:

```kirito
var io = import("io")
var first = [1, 2, 3]   # a List (collections come later)
var second = first      # second is bound to the SAME list, not a copy
second.append(4)
io.print(first)         # => [1, 2, 3, 4]   — first sees the change too
```

For immutable values (Integer, Float, String, Bool, None) this never surprises you, because you can
never change the value in place — you only ever rebind the name:

```kirito
var io = import("io")
var x = 10
var y = x
x = 99                  # rebinds x; y still refers to the original 10
io.print(x, y)          # => 99 10
```

When you genuinely want an independent copy of a mutable value, ask for one explicitly (the `copy`
module and `.copy()` methods, covered later).

## Truthiness

Every value is either "truthy" or "falsy" when used as a condition. The falsy values are `False`,
`None`, zero (`0`, `0.0`), the empty String `""`, and any empty collection. Everything else is
truthy:

```kirito
var io = import("io")
var name = ""
if name:
    io.print("got a name")
else:
    io.print("name is empty")     # => name is empty (an empty String is falsy)
```

`Bool(x)` makes this explicit, returning the truthiness of any value:

```kirito
var io = import("io")
io.print(Bool(0), Bool(42), Bool(""), Bool("hi"), Bool([]))
# => False True False True False
```

**`Bool` is its own type, not a number.** `True` is *not* `1`: `True == 1` is `False`,
and Bools are deliberately **not numeric** — `True + 1`, `abs(True)`, `sum([True, False])`, and
`sorted([True, False])` all throw a type error rather than treating `True`/`False` as `1`/`0`. When
you really want to count truth values, convert explicitly with `Integer(flag)` (`Integer(True) == 1`).

## Two number types

`Integer` is a 64-bit whole number; `Float` is a double-precision decimal. The literal you write
decides which you get — a trailing `.0` makes a Float:

```kirito
var io = import("io")
var whole = 42          # Integer
var decimal = 42.0      # Float
io.print(type(whole), type(decimal))   # => Integer Float
```

## The arithmetic operators

```kirito
var io = import("io")
io.print(7 + 3)         # => 10
io.print(7 - 3)         # => 4
io.print(7 * 3)         # => 21
io.print(7 / 3)         # => 2.33333333333333   division — ALWAYS a Float
io.print(7 // 3)        # => 2     floor division — rounds toward negative infinity
io.print(7 % 3)         # => 1     modulo (remainder)
io.print(2 ** 10)       # => 1024  exponentiation (right-associative)
```

Two rules are worth burning in:

1. **`/` always produces a Float**, even for evenly divisible integers: `6 / 2` is `3.0`, not `3`.
   Use `//` when you want an Integer result.
2. **`//` and `%` use floor semantics** — the quotient rounds toward negative infinity and the
   remainder takes the sign of the divisor:

```kirito
var io = import("io")
io.print(-7 // 3)       # => -3   (not -2: it rounds DOWN)
io.print(-7 % 3)        # => 2    (sign follows the divisor)
```

`**` is right-associative, so `2 ** 3 ** 2` is `2 ** (3 ** 2)` = `2 ** 9` = `512`.

## Number literals in any base

Integers can be written in decimal, hexadecimal, octal, or binary (the prefix is case-insensitive),
and floats allow scientific notation:

```kirito
var io = import("io")
io.print(255, 0xFF, 0o377, 0b11111111)   # => 255 255 255 255  (all the same value)
io.print(1e3, 1.5e3, 2e-3)               # => 1000.0 1500.0 0.002
```

## Overflow wraps, it doesn't crash

Integer arithmetic is fixed-width 64-bit with **well-defined two's-complement wraparound** — it
never triggers undefined behavior. You rarely hit this, but it's defined when you do:

```kirito
var io = import("io")
var biggest = 9223372036854775807        # the largest Integer
io.print(biggest + 1)                     # => -9223372036854775808  (wraps around)
```

## Float equality is exact — use `.compare` for tolerance

`==` on Floats is **exact IEEE-754 bit equality**, just like `<` and `>`. Because the values you write
in decimal can't always be represented exactly in binary, sums that *look* equal often aren't:

```kirito
var io = import("io")
io.print(0.1 + 0.2)               # => 0.3      (display is rounded to be readable)
io.print(0.1 + 0.2 == 0.3)        # => False    (but the stored bits differ!)
```

`0.1 + 0.2` is stored as `0.30000000000000004`, a different double from `0.3` — this is how binary
floating point works everywhere. When you want to compare Floats *up to a small tolerance*, call
`.compare` on either number:

```kirito
var io = import("io")
io.print((0.1 + 0.2).compare(0.3))                 # => True   (close enough)
io.print((1.0).compare(1.5))                       # => False  (too far apart)
io.print((1.0).compare(1.5, abs_tol = 1.0))        # => True   (widened tolerance)
```

`x.compare(other, rel_tol = 1e-9, abs_tol = 0.0)` is `True` when `x` and `other` are within either a
relative tolerance (`rel_tol`, scaled to the larger magnitude) or an absolute one (`abs_tol`, useful
near zero); it also works between an Integer and a Float.

## Rounding, absolute value, and the `math` module

```kirito
var io = import("io")
io.print(abs(-5), abs(-2.5))             # => 5 2.5
io.print(round(3.14159))                  # => 3      (to the nearest Integer)
io.print(round(3.14159, ndigits=2))       # => 3.14   (to 2 decimal places -> Float)
io.print(divmod(17, 5))                   # => [3, 2] (quotient and remainder at once)
```

Square roots, logs, trig, and constants live in the `math` module:

```kirito
var io = import("io")
var math = import("math")
io.print(math.sqrt(144))                  # => 12.0
io.print(math.pi)                          # => 3.14159265358979
io.print(math.floor(3.7), math.ceil(3.2)) # => 3 4
io.print(math.gcd(24, 36))                 # => 12
io.print(math.factorial(5))                # => 120
```

## Bitwise work is done by named functions

Kirito deliberately has **no** `&`, `|`, `^`, `~`, `<<`, `>>` operators — those symbols are easy to
misread. Bitwise operations are explicit, named builtins on Integers instead:

```kirito
var io = import("io")
io.print(bitand(0b1100, 0b1010))          # => 8   (0b1000)
io.print(bitor(0b1100, 0b1010))           # => 14  (0b1110)
io.print(bitxor(0b1100, 0b1010))          # => 6   (0b0110)
io.print(shl(1, 4))                        # => 16  (1 shifted left 4 bits)
io.print(bin(shl(1, 4)))                   # => 0b10000  (bin() shows the binary form)
```

## Converting between types

The type names double as converters. Each takes a value and returns the converted form:

```kirito
var io = import("io")
io.print(Integer("42") + 1)     # => 43   (String -> Integer)
io.print(Float(7))              # => 7.0  (Integer -> Float)
io.print(String(3.5) + "!")     # => 3.5! (anything -> String)
io.print(Integer(3.9))          # => 3    (Float -> Integer, truncates toward zero)
```

A conversion that can't succeed throws a clear error rather than guessing:

<!--norun (intentional conversion error)-->
```kirito
Integer("not a number")         # error: cannot convert String to Integer
```

## Try it

1. Declare `temp_celsius = 100`, convert it to Fahrenheit (`c * 9 / 5 + 32`), and print
   `100C is 212.0F`. Notice which operations give an Integer and which give a Float.
2. Given `total_seconds = 3725`, print `1h 2m 5s` using `//` and `%` to peel off hours, then minutes,
   then seconds.

## What you learned

- The built-in values: `Integer`, `Float`, `String`, `Bool`, `None`; `type(x)` names a value's type.
- `var` declares; `=` updates an existing name (and errors if it doesn't exist).
- Assignment binds names to values — mutable values are shared, not copied; truthiness rules.
- `Integer` vs `Float`, why `/` is always a Float while `//` floors, floor `//`/`%` semantics,
  right-associative `**`; hex/octal/binary/scientific literals; defined integer overflow.
- Float `==` is **exact** (so `0.1 + 0.2 == 0.3` is `False`); use `x.compare(other, rel_tol, abs_tol)`
  for tolerant comparison.
- `abs`/`round`/`divmod`, the `math` module, named bitwise builtins, and the type-name converters.
