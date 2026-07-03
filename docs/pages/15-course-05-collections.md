# Lesson 5 — Collections: Lists, Sets, Dicts & Unpacking

Most programs revolve around collections of values. Kirito has three, each with a job: the **List**
(an ordered, mutable sequence), the **Set** (distinct values with instant membership), and the
**Dict** (a mapping from keys to values). This lesson covers all three, then **unpacking** — the
syntax that moves several values around at once.

## Lists: ordered sequences

A List holds values of any type, grows and shrinks on demand, and supports indexing and slicing.

```kirito
var io = import("io")
var primes = [2, 3, 5, 7, 11]
io.print(primes[0])       # => 2     (indices start at 0)
io.print(primes[-1])      # => 11    (negative counts from the end)
io.print(len(primes))     # => 5
io.print(primes[1:4])     # => [3, 5, 7]   (a slice — a new List)

var mixed = [1, "two", 3.0, [4, 5]]
io.print(mixed[3][0])     # => 4   (index into the nested list)
```

### Growing and shrinking

```kirito
var io = import("io")
var stack = []
stack.append(10)          # add to the end
stack.append(20)
var top = stack.pop()     # remove and return the last item
io.print(top, stack)      # => 20 [10]
stack.insert(0, 5)        # insert at an index
stack.remove(10)          # remove the first value equal to 10
io.print(stack)           # => [5]
```

### Searching, combining, and counting

`+` concatenates into a new list; `*` repeats; `.extend` appends another list in place:

```kirito
var io = import("io")
var letters = ["a", "b", "a", "c", "a"]
io.print("a" in letters)        # => True   (membership test)
io.print(letters.index("b"))    # => 1      (position of the first match)
io.print(letters.count("a"))    # => 3      (how many equal)

io.print([1, 2] + [3, 4])       # => [1, 2, 3, 4]
io.print([0] * 4)                # => [0, 0, 0, 0]   (handy for fixed-size buffers)
```

### Sorting

`.sort()` sorts **in place**; the builtin `sorted(...)` returns a **new** sorted list and leaves the
original alone. Both are *stable* and both accept `key` and `reverse`:

```kirito
var io = import("io")
var scores = [42, 17, 99, 8, 53]
io.print(sorted(scores))                 # => [8, 17, 42, 53, 99]   (original unchanged)
io.print(sorted(scores, reverse=True))   # => [99, 53, 42, 17, 8]

var words = ["banana", "fig", "cherry"]
io.print(sorted(words, key=len))         # => ['fig', 'banana', 'cherry']  (by length)
```

`key` is a function applied to each element to produce its sort key (computed once per element). You
can sort by several keys at once by returning a List as the key — it compares element by element.

### Lists are shared, not copied

Recall the binding model from Lesson 2: assigning a list to a second name shares it. When you want an
independent list, use `.copy()` (a shallow copy) or `copy.deepcopy` for nested structures:

```kirito
var io = import("io")
var original = [1, 2, 3]
var independent = original.copy()
independent.append(4)
io.print(original, independent)          # => [1, 2, 3] [1, 2, 3, 4]
```

## Sets: distinct values

A Set holds each value at most once and tests membership instantly:

```kirito
var io = import("io")
var seen = {3, 1, 4, 1, 5, 9, 2, 6, 5}   # duplicates collapse
io.print(len(seen))                       # => 7   (distinct values: 1 2 3 4 5 6 9)
io.print(3 in seen, 7 in seen)            # => True False
seen.add(7)
seen.discard(3)                            # remove if present (no error if absent)
io.print(7 in seen, 3 in seen)             # => True False
```

> The literal `{}` is an empty **Dict**, not a Set — use `Set()` for an empty set. A brace literal
> with values like `{1, 2}` is a Set.

Sets support the classic mathematical operations as methods (a common use is de-duplication:
`List(Set(items))` gives the distinct items):

```kirito
var io = import("io")
var a = {1, 2, 3, 4}
var b = {3, 4, 5, 6}
io.print(sorted(a.union(b)))            # => [1, 2, 3, 4, 5, 6]
io.print(sorted(a.intersection(b)))     # => [3, 4]
io.print(sorted(a.difference(b)))       # => [1, 2]
io.print(a.issubset({1, 2, 3, 4, 5}))   # => True
```

(We wrap results in `sorted(...)` only to get a stable order for printing — sets are unordered.)

## Dicts: keys to values

A Dict maps keys to values. Create one with `{key: value, ...}`, look up with `d[key]`, and assign
with `d[key] = value`:

```kirito
var io = import("io")
var ages = {"Ada": 36, "Alan": 41}
io.print(ages["Ada"])           # => 36
ages["Grace"] = 45              # add a new entry
ages["Ada"] = 37                # update an existing one
io.print("Alan" in ages)        # => True  (membership tests keys)
io.print(len(ages))             # => 3
```

Looking up a missing key is an error. When a key might be absent, use `.get`, which returns a default
instead of throwing:

```kirito
var io = import("io")
var ages = {"Ada": 36}
io.print(ages.get("Edsger"))             # => None  (absent -> None by default)
io.print(ages.get("Edsger", 0))          # => 0     (your own default)
```

`.keys()`, `.values()`, and `.items()` give the three views. A Dict does **not** promise any
particular iteration order, so sort the keys first if you need a deterministic one:

```kirito
var io = import("io")
var inventory = {"apples": 12, "pears": 7}
for name in sorted(inventory.keys()):
    io.print(name)                       # apples / pears
```

### A worked example: word frequency

```kirito
var io = import("io")

var word_counts = Function(text : String) -> Dict:
    var counts = {}
    for word in text.lower().split(" "):
        if len(word) == 0:
            continue
        # setdefault returns the current count (inserting 0 the first time we see a word).
        counts[word] = counts.setdefault(word, 0) + 1
    return counts

var tally = word_counts("the cat the dog the bird")
io.print(tally["the"], tally["cat"])     # => 3 1
```

**Walkthrough:** `setdefault(word, 0)` is the counting idiom — the first time a word appears it
inserts `0` and returns it; thereafter it returns the running count. The result is a Dict from each
word to how often it occurred. (The `collections` module's `Counter` automates exactly this.)

## Packing and unpacking

A bare comma **packs** values into a List, and the left-hand side of an assignment **unpacks** an
iterable into separate names:

```kirito
var io = import("io")
var point = 3, 4               # same as [3, 4]
io.print(point, type(point))   # => [3, 4] List

var first, second = [10, 20]   # unpack a list
io.print(first, second)        # => 10 20
var p, q, r = "xyz"            # any iterable unpacks
io.print(p, q, r)              # => x y z
```

The counts must match — too few or too many values is a clear error. Because the right side is fully
evaluated first, you can **swap** without a temporary, and one **starred** target absorbs the rest:

```kirito
var io = import("io")
var left = 1
var right = 2
left, right = right, left
io.print(left, right)          # => 2 1

var head, *tail = [1, 2, 3, 4]
io.print(head, tail)           # => 1 [2, 3, 4]
var lo, *mid, hi = [1, 2, 3, 4, 5]
io.print(lo, mid, hi)          # => 1 [2, 3, 4] 5
```

Unpacking shines in `for` loops and in returning several values from a function:

```kirito
var io = import("io")
var prices = {"apple": 1.20, "pear": 0.90}
for name, cost in prices.items():
    io.print(f"{name} costs {cost}")

# Return both the quotient and the remainder; the caller unpacks them.
var divide = Function(numerator : Integer, denominator : Integer):
    return numerator // denominator, numerator % denominator
var quotient, remainder = divide(17, 5)
io.print(f"17 = 5 * {quotient} + {remainder}")   # => 17 = 5 * 3 + 2
```

An unpack target can even be an index or a member, not just a bare name:

```kirito
var io = import("io")
var grid = [0, 0, 0]
grid[0], grid[2] = 9, 7
io.print(grid)                 # => [9, 0, 7]
```

## Try it

1. Write `running_max(numbers)` returning a new list where each element is the largest seen so far:
   `running_max([3, 1, 4, 1, 5, 9, 2])` gives `[3, 3, 4, 4, 5, 9, 9]`.
2. Given two lists of names, print the names in *both* (set intersection) and those in the first but
   not the second (difference).
3. Write `minmax(numbers)` returning the smallest and largest in one `return`, and call it with
   `var lo, hi = minmax([4, 9, 1, 7])`.

## What you learned

- **Lists:** create, index (incl. negative), slice; `append`/`pop`/`insert`/`remove`/`extend`,
  `in`/`index`/`count`, `+`/`*`, `sort` (in place) vs `sorted` (new) with `key`/`reverse`; lists are
  shared references, `.copy()` makes an independent one.
- **Sets:** distinct values, instant membership, set-algebra methods; `{}` is an empty Dict, `Set()`
  an empty Set.
- **Dicts:** `d[k]`, `d[k] = v`, `in`, `.get`, `.keys`/`.values`/`.items`, the `setdefault` idiom;
  iteration order is unspecified.
- **Unpacking:** a bare comma packs into a List; the left side of `=`/`var`/`for` unpacks any iterable
  (counts checked), with the swap idiom, starred targets, `for k, v in d.items()`, and multi-value
  returns.
