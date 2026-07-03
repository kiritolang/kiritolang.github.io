# Lesson 6 — Functions and Closures

Functions name a piece of work so you can reuse it, test it, and read your program at a higher level.
In Kirito a function is a **value** created with `Function(...)` and bound to a name like anything
else — which means you can also pass functions around and return them, the idea that powers closures.

## Defining and calling

```kirito
var io = import("io")

var greet = Function(name):
    return f"Hello, {name}!"

io.print(greet("Ada"))         # => Hello, Ada!
```

The parameters go in the parentheses; the indented block is the body; `return` hands back a value.

## The default return is `None`

A function with no `return` (or a bare `return`) yields `None`. Such functions are run for their
*effect*, not their value:

```kirito
var io = import("io")
var log = Function(message):
    io.print(f"[log] {message}")
    # no return -> yields None

var result = log("starting up")     # => [log] starting up
io.print(result)                     # => None
```

## Default parameter values and keyword arguments

A parameter can have a default, used when the caller omits it. Defaults are evaluated **at call
time, once per call** — so a mutable default like `xs = []` is a *fresh* list on every call (no
shared-default surprise). You can also pass arguments **by
name**, in any order — clearer at the call site and order-independent:

```kirito
var io = import("io")
var power = Function(base, exponent = 2):
    return base ** exponent

io.print(power(5))             # => 25   (exponent defaults to 2)
io.print(power(5, 3))          # => 125

var make_label = Function(text, width, fill = "-"):
    return text.center(width, fill)

io.print(make_label("hi", 8))                       # => ---hi---
io.print(make_label(width=8, text="hi", fill="*"))  # => ***hi***
```

Keyword arguments work for built-in and standard-library functions too — that's why you've been
writing `round(x, ndigits=2)` and `sorted(xs, reverse=True)`.

## Composing small functions

Break a problem into small, single-purpose functions and compose them. Each one is easy to read and
test on its own:

```kirito
var io = import("io")

var is_vowel = Function(ch : String) -> Bool:
    return ch.lower() in "aeiou"

var count_vowels = Function(text : String) -> Integer:
    var n = 0
    for ch in text:
        if is_vowel(ch):
            n = n + 1
    return n

io.print(count_vowels("Hello World"))    # => 3
```

`is_vowel` answers one tiny question; `count_vowels` uses it in a loop. Splitting the work this way
means you can test `is_vowel("e")` in isolation — the habit that keeps large programs manageable.

## `discard` and `todo`

If you call a function only for its side effect and ignore a non-`None` result, Kirito's analyzer
warns that you dropped a value. Mark it `discard` to say "I meant to". When you're sketching, `todo`
is a no-op that emits a reminder warning at its location:

```kirito
var io = import("io")
discard io.input("Press Enter to continue... ")   # we don't need the line back
```

<!--norun (todo emits a warning; shown for illustration)-->
```kirito
var parse_config = Function(path):
    todo "read and validate the config file"
```

## Functions are values

Because a function is an ordinary value, you can store it, pass it, return it, and keep a list of
them:

```kirito
var io = import("io")
var double = Function(x): return x * 2
var triple = Function(x): return x * 3

var operations = [double, triple]      # a List of functions
for op in operations:
    io.print(op(10))                    # => 20 / 30
```

A function that takes another function is "higher-order". The builtins `map`, `filter`, and
`sorted(key=...)` are the everyday examples:

```kirito
var io = import("io")
var numbers = [1, 2, 3, 4, 5, 6]
io.print(map(Function(n): return n * n, numbers))          # => [1, 4, 9, 16, 25, 36]
io.print(filter(Function(n): return n % 2 == 0, numbers))  # => [2, 4, 6]
io.print(sorted(["bb", "a", "ccc"], key=len))              # => ['a', 'bb', 'ccc']
```

## Closures: functions that remember

A function that *remembers* variables from where it was created is a **closure**. The key move is a
function that builds and returns another function:

```kirito
var io = import("io")

# make_adder returns a NEW function that adds `amount` to whatever it's given.
var make_adder = Function(amount):
    return Function(x): return x + amount     # the inner function "closes over" amount

var add_ten = make_adder(10)
var add_hundred = make_adder(100)
io.print(add_ten(5), add_hundred(5))          # => 15 105
```

`add_ten` and `add_hundred` are separate functions, each carrying its own captured `amount`.

Because a closure can capture a *mutable* binding, you can build little objects with private state
without a class:

```kirito
var io = import("io")

var make_counter = Function():
    var count = 0                 # private to each counter
    return Function():
        count = count + 1         # rebinds the captured `count`
        return count

var tick = make_counter()
io.print(tick(), tick(), tick())  # => 1 2 3
var other = make_counter()
io.print(other())                 # => 1   (independent state)
```

### A worked example: composing transformations

```kirito
var io = import("io")

# Build a pipeline that applies a list of single-argument functions in order.
var pipeline = Function(steps):
    return Function(value):
        var current = value
        for step in steps:
            current = step(current)
        return current

var clean = pipeline([
    Function(s): return s.strip(),
    Function(s): return s.lower(),
    Function(s): return s.replace(" ", "_"),
])

io.print(clean("  Hello World  "))     # => hello_world
```

**Walkthrough:** `pipeline` takes a list of step-functions and returns a closure that threads a value
through each step. This is composition as data — you can build, store, and reuse transformations as
values.

## Try it

1. Write `clamp(value, low, high)` returning `value` constrained to `[low, high]`, with `low`
   defaulting to `0` and `high` to `100`. Test `clamp(150)`, `clamp(-5)`, `clamp(42, low=10, high=50)`.
2. Write `make_multiplier(factor)` returning a function that multiplies its argument by `factor`.
   Build `[make_multiplier(2), make_multiplier(10)]` and apply each to `7`.

## What you learned

- `Function(params): ... return value` defines a function value; the default return is `None`.
- Default parameter values and keyword arguments (which work on builtins too); composing small,
  single-purpose functions; `discard` to drop a result, `todo` as a nagging placeholder.
- Functions are first-class values: store, pass, return, list them; the higher-order builtins `map`,
  `filter`, `sorted(key=...)`.
- A **closure** captures variables from its defining scope — even mutable ones — giving private,
  independent state and letting you compose behavior as data.
