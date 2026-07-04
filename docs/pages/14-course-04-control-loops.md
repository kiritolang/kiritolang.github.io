# Lesson 4 — Control Flow and Loops

Programs make decisions and repeat work. This lesson covers comparisons and the logical operators,
the `if` family and `switch`, then the two kinds of loop — `while` and `for` — and the builtins that
make loops expressive.

## Comparisons produce Bools

```kirito
var io = import("io")
io.print(3 < 5)          # => True
io.print(3 == 3.0)       # => True   (Integer and Float compare by value)
io.print("a" < "b")      # => True   (strings compare lexicographically)
io.print(3 != 4)         # => True
io.print(5 >= 5)         # => True
```

## Combining conditions: `and`, `or`, `not`

These are spelled as words, and `and`/`or` **short-circuit** — they stop evaluating as soon as the
answer is known:

```kirito
var io = import("io")
var age = 25
var has_ticket = True
if age >= 18 and has_ticket:
    io.print("admitted")           # => admitted

io.print(not False)                 # => True

var boom = Function():
    return 1 // 0                   # would crash if ever called
io.print(True or boom())            # => True  (the right side is never evaluated)
```

Short-circuiting is not just an optimization — it lets you guard a risky operation with a cheap check
on its left: `if count > 0 and total / count > threshold:`. Above, `True or ...` already knows the
answer, so `boom()` — which would divide by zero — is never reached.

## `if` / `elif` / `else`

Indentation defines the block. Be consistent (pick spaces or tabs and stick with it — Kirito rejects
ambiguous mixing):

```kirito
var io = import("io")
var score = 72
if score >= 90:
    io.print("A")
elif score >= 80:
    io.print("B")
elif score >= 70:
    io.print("C")            # => C
else:
    io.print("try again")
```

## The conditional expression

When you just need to *choose between two values*, the conditional expression
`then if cond else orelse` is more concise than a four-line `if`/`else`. It's an **expression**, so it
produces a value you can assign, return, or pass along:

```kirito
var io = import("io")
var score = 72
var outcome = "pass" if score >= 60 else "fail"
io.print(outcome)                       # => pass

var x = 75
var band = "high" if x >= 90 else "mid" if x >= 50 else "low"   # chains right-associatively
io.print(band)                          # => mid
```

Only the chosen branch is evaluated. Reach for it for a quick two-way pick; for anything with side
effects or more than a couple of branches, a statement `if`/`elif`/`else` reads better.

## `switch` for multi-way branching

`switch` matches a subject against `case` labels. Exactly one arm runs — **there is no
fall-through** — and an optional `default` handles everything else:

```kirito
var io = import("io")
var describe_day = Function(day : String) -> String:
    switch day:
        case "Sat", "Sun":              # one arm, several labels
            return "weekend"
        case "Mon":
            return "back to work"
        default:
            return "a weekday"

io.print(describe_day("Sun"))           # => weekend
io.print(describe_day("Mon"))           # => back to work
io.print(describe_day("Wed"))           # => a weekday
```

In practice you'll use **constant scalars** for case labels (Integer, Float, String, Bool, None) —
matched by exact type *and* value, so `case 1` does not match `1.0`, and an all-literal switch compiles
to one O(1) dispatch. Labels may be any expression, though (`case some_var`, `case 3 + 4`); a
non-literal switch falls back to a comparison chain. `case` and `default` are "soft keywords":
they're only special inside a `switch`, so you can still use them as ordinary names elsewhere.

**Which to use?** Ranges (`>= 90`) need comparisons, so an `if`/`elif` chain fits. Matching a value
against a fixed set of constants is exactly what `switch` is for — it reads as a table and runs in
constant time regardless of how many arms it has.

## `while` — condition-driven loops

A `while` loop runs its body as long as the condition is truthy:

```kirito
var io = import("io")
var countdown = 3
while countdown > 0:
    io.print(countdown)
    countdown = countdown - 1
io.print("liftoff!")
# => 3 / 2 / 1 / liftoff!  (one per line)
```

The body must eventually make the condition false, or the loop runs forever — `countdown =
countdown - 1` is what guarantees progress here.

## `for ... in` — item-driven loops

A `for` loop walks any **iterable** — a range, a List, a String, a Set, a Dict — binding each item to
a name in turn:

```kirito
var io = import("io")
for letter in "hi!":
    io.print(letter)           # => h / i / !

for item in ["apple", "pear"]:
    io.print(item)             # => apple / pear
```

`range` is the workhorse of counting loops:

```kirito
var io = import("io")
for i in range(3):             # 0, 1, 2  — stop is exclusive
    io.print(i)

var total = 0
for n in range(1, 11):         # 1 through 10
    total = total + n
io.print(total)                # => 55

for even in range(0, 10, 2):   # start, stop, step
    io.print(even)             # => 0 2 4 6 8
```

## `break` and `continue`

`break` exits the loop immediately; `continue` skips to the next iteration:

```kirito
var io = import("io")
# Find the first multiple of 7 above 20.
var found = None
for n in range(21, 100):
    if n % 7 == 0:
        found = n
        break                  # stop as soon as we have it
io.print(found)                # => 21

# Print only the odd numbers.
for n in range(10):
    if n % 2 == 0:
        continue               # skip the evens
    io.print(n)                # => 1 3 5 7 9
```

## `enumerate` and `zip`

`enumerate` pairs each item with its position, so you don't manage a counter by hand; `zip` pairs
items from several iterables, stopping at the shortest:

```kirito
var io = import("io")
for pair in enumerate(["red", "green", "blue"]):
    io.print(f"{pair[0]}: {pair[1]}")
# => 0: red / 1: green / 2: blue

var names = ["Ada", "Alan", "Grace"]
var ages = [36, 41, 45]
for pair in zip(names, ages):
    io.print(f"{pair[0]} is {pair[1]}")
# => Ada is 36 / Alan is 41 / Grace is 45
```

(The next lesson's unpacking lets you write these as `for index, color in enumerate(...)` and
`for name, age in zip(...)`.)

## A worked example: the accumulator pattern

```kirito
var io = import("io")

var average = Function(numbers) -> Float:
    if len(numbers) == 0:
        return 0.0                       # avoid dividing by zero
    var running_total = 0
    for value in numbers:
        running_total = running_total + value
    return running_total / len(numbers)  # / always yields a Float

io.print(average([10, 20, 30, 40]))      # => 25.0
io.print(average([]))                     # => 0.0
```

**Walkthrough:** the classic *accumulator* pattern — initialize a total to zero, add each item as the
loop visits it, then use it after the loop. The empty-input guard up front keeps the final division
safe.

## Try it

1. Write `season(month)` taking an Integer 1–12 and returning `"winter"`, `"spring"`, `"summer"`, or
   `"autumn"`. Try it once with `if`/`elif` and once with a `switch` (grouping months per arm,
   `case 12, 1, 2:`).
2. Print a 9×9 multiplication table, neatly aligned — a `for` inside a `for`, building each row as an
   f-string with a width format spec like `{product:4d}`.

## What you learned

- Comparisons produce Bools; `and`/`or`/`not` are words and short-circuit.
- `if`/`elif`/`else` with indentation-defined blocks; the conditional expression for a concise value
  pick; `switch`/`case`/`default` with no fall-through, exact scalar matching, constant-time dispatch.
- `while` for condition-driven loops, `for ... in` for item-driven loops; `range`, `break`/`continue`.
- `enumerate` for index+item, `zip` to walk sequences in parallel, and the accumulator pattern.
