# Lesson 9 — Classes II: Operators and Protocols

In Lesson 8 your classes had named methods. Now you'll make them feel like *built-in* types — usable
with `+`, `==`, `[]`, `len`, `in`, `for`, and even called like functions. You do this by defining
**special methods**: methods whose names have a single underscore on each side (`_add_`, `_str_`,
`_getitem_`, …). The evaluator calls them automatically when the corresponding operator or builtin is
used.

## Printing: `_str_`

`_str_` controls how your instance turns into text for `io.print`, `String(x)`, and f-strings:

```kirito
var io = import("io")

class Money:
    var _init_ = Function(self, cents):
        self.cents = cents
    var _str_ = Function(self) -> String:
        return f"${self.cents / 100.0}"

io.print(Money(1599))            # => $15.99   (io.print calls _str_)
io.print(f"price: {Money(500)}") # => price: $5.0
```

## Arithmetic: `_add_`, `_sub_`, `_mul_`, …

Define `_add_(self, other)` to make `+` work on your type. The left operand is `self`, the right is
`other`. Return the result (usually a new instance):

```kirito
var io = import("io")

class Vec:
    var _init_ = Function(self, x, y):
        self.x = x
        self.y = y
    var _add_ = Function(self, other):
        return Vec(self.x + other.x, self.y + other.y)
    var _mul_ = Function(self, scalar):
        return Vec(self.x * scalar, self.y * scalar)
    var _str_ = Function(self) -> String:
        return f"Vec({self.x}, {self.y})"

io.print(Vec(1, 2) + Vec(3, 4))  # => Vec(4, 6)
io.print(Vec(1, 2) * 3)          # => Vec(3, 6)
```

The full arithmetic set is `_add_`, `_sub_`, `_mul_`, `_div_`, `_floordiv_`, `_mod_`, `_pow_`.

## Comparison and equality: `_eq_`, `_lt_`, …

`_eq_` defines `==` (and drives `in` and `.index` on lists); `_lt_` defines `<` (and drives `sorted`,
`min`, `max`):

```kirito
var io = import("io")

class Version:
    var _init_ = Function(self, major, minor):
        self.major = major
        self.minor = minor
    var _eq_ = Function(self, other) -> Bool:
        return self.major == other.major and self.minor == other.minor
    var _lt_ = Function(self, other) -> Bool:
        if self.major != other.major:
            return self.major < other.major
        return self.minor < other.minor
    var _str_ = Function(self) -> String:
        return f"{self.major}.{self.minor}"

var versions = [Version(1, 4), Version(1, 2), Version(2, 0)]
io.print(sorted(versions))                 # => [1.2, 1.4, 2.0]   (sorted uses _lt_)
io.print(Version(1, 2) == Version(1, 2))   # => True             (uses _eq_)
```

The comparison family is `_eq_`, `_ne_`, `_lt_`, `_le_`, `_gt_`, `_ge_`. (`_ne_` defaults to "not
`_eq_`" if you don't define it.) Unary `_neg_` (`-x`) and `_not_` (`not x`) round out the operators.

## Container behavior: `_getitem_`, `_setitem_`, `_len_`, `_contains_`, `_iter_`

Make your type act like a collection:

```kirito
var io = import("io")

class Grid:
    var _init_ = Function(self, width, height):
        self.width = width
        self._cells = [0] * (width * height)    # flat storage
    var _getitem_ = Function(self, row, col):    # called for grid[row, col]
        return self._cells[row * self.width + col]
    var _setitem_ = Function(self, row, col, value):  # grid[row, col] = value
        self._cells[row * self.width + col] = value
    var _len_ = Function(self) -> Integer:       # len(grid)
        return len(self._cells)

var board = Grid(3, 3)
board[1, 1] = 5                  # calls _setitem_(self, 1, 1, 5)
io.print(board[1, 1])            # => 5    (calls _getitem_(self, 1, 1))
io.print(len(board))             # => 9    (calls _len_)
```

Note `_getitem_`/`_setitem_` are **variadic in the key** — `grid[1, 1]` passes two key arguments — so
you can build matrices and multi-dimensional containers naturally.

`_contains_(self, item)` powers `in`, and `_iter_(self)` powers `for` (return a List of the items to
yield):

```kirito
var io = import("io")

class Range3:                    # yields 0, 1, 2
    var _iter_ = Function(self):
        return [0, 1, 2]
    var _contains_ = Function(self, x) -> Bool:
        return x == 0 or x == 1 or x == 2

for n in Range3():
    io.print(n)                  # => 0 / 1 / 2
io.print(1 in Range3())          # => True
```

## Callable instances: `_call_`

`_call_` makes an instance usable like a function — perfect for configurable, stateful "function
objects":

```kirito
var io = import("io")

class Multiplier:
    var _init_ = Function(self, factor):
        self.factor = factor
    var _call_ = Function(self, x):
        return x * self.factor

var triple = Multiplier(3)
io.print(triple(10))             # => 30   (calls _call_(self, 10))
```

## Hashable instances: `_hash_` + `_eq_`

By default a user-class instance is **unhashable** — putting one in a `Set` or using it as a `Dict`
key throws `unhashable type '<class>'`. Define `_hash_(self) -> Integer` to opt in, and pair it
with `_eq_` so two instances that compare equal share a bucket:

```kirito
var hash = import("hash")

class Coord:
    var _init_ = Function(self, x, y):
        self.x = x
        self.y = y
    var _hash_ = Function(self) -> Integer:
        return hash.hash(self.x) * 31 + hash.hash(self.y)
    var _eq_ = Function(self, other) -> Bool:
        return isinstance(other, Coord) and self.x == other.x and self.y == other.y

var seen = {Coord(1, 2), Coord(1, 2), Coord(3, 4)}     # a Set with 2 elements
var byCoord = {Coord(0, 0): "origin"}
io.print(byCoord[Coord(0, 0)])                          # "origin" — fresh Coord finds the slot
```

`_hash_` **must return an Integer**, and without `_eq_` collections still key by identity — see
[Types → Hashability](types.html#hashability-set-dict-keys) for the full contract.

## Try it

Give the `Money` class above an `_add_` (add the cents), an `_eq_` (compare cents), and an `_lt_`
(compare cents). Then sort a list of `Money` amounts and print it. You'll have a type that prints
nicely, adds with `+`, and sorts — indistinguishable from a built-in at the call site.

## What you learned

- Special methods (single underscores) hook your class into operators and builtins.
- `_str_` for display; `_add_`/`_mul_`/… for arithmetic; `_eq_`/`_lt_`/… for comparison and sorting.
- `_getitem_`/`_setitem_` (variadic keys), `_len_`, `_contains_`, `_iter_` make a container.
- `_call_` makes instances callable; `_hash_` + `_eq_` makes them Dict/Set keys.
