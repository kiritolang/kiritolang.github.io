# Lesson 14 — A Tour of the Standard Library

You've met `io` and `math`. Kirito ships a sizeable standard library — each module is imported the
same way and loads once per program. This lesson is a guided tour of the modules you'll reach for
most. For the complete, signature-by-signature reference, see the **Standard Library** page; here
we focus on *when* and *why* to use each.

## Importing and exploring

Every module arrives via `import`. When you forget what a module offers, `inspect` prints its
contents live:

```kirito
var io = import("io")
var math = import("math")
io.print(type(math))                  # => Module
# io.print(inspect(math))             # prints math's full signature list
```

## `random` — randomness without global state

`random` gives you a generator object (no hidden global state, so two generators are independent and
a seeded one is reproducible):

```kirito
var io = import("io")
var random = import("random")

var rng = random.Random(42)           # seeded -> reproducible
io.print(rng.randint(1, 6))           # a dice roll in [1, 6]
io.print(rng.choice(["a", "b", "c"])) # a random element
var deck = [1, 2, 3, 4, 5]
rng.shuffle(deck)                      # shuffles in place
io.print(len(deck))                    # => 5  (same items, new order)
```

## `json` — data interchange

`json` converts between Kirito values and JSON text — the lingua franca for config files and web
APIs:

```kirito
var io = import("io")
var json = import("json")

var profile = {"name": "Ada", "langs": ["Kirito", "C++"], "active": True}
var text = json.dumps(profile)                 # Dict -> JSON String
io.print(text)                                  # => {"name": "Ada", ...}

var parsed = json.loads(text)                   # JSON String -> Dict
io.print(parsed["langs"][0])                    # => Kirito
io.print(json.dumps(profile, indent=2))         # pretty-printed with indentation
```

## `time` — clocks and calendars

```kirito
var io = import("io")
var time = import("time")

var start = time.perfcounterns()       # a high-precision monotonic counter
var total = 0
for i in range(100000):
    total = total + i
var elapsed = time.perfcounterns() - start
io.print(f"loop ran in {elapsed} ns")
```

`time` also offers wall-clock time and a `DateTime` type with field access and arithmetic.

## `collections` — specialized containers

`Counter` tallies occurrences (the word-frequency idiom from the Collections lesson, automated); `deque` is a
double-ended queue; `defaultdict` supplies missing values:

```kirito
var io = import("io")
var collections = import("collections")

var votes = collections.Counter(["yes", "no", "yes", "yes", "no"])
io.print(votes["yes"])                 # => 3   (index by item to get its count)
io.print(votes.mostcommon())           # => [['yes', 3], ['no', 2]]  (pairs, most frequent first)
```

## `itertools` and `functools` — building blocks for iteration and functions

```kirito
var io = import("io")
var itertools = import("itertools")
var functools = import("functools")

# All ways to pick 2 of 3 items.
io.print(itertools.combinations([1, 2, 3], 2))   # => [[1, 2], [1, 3], [2, 3]]

# reduce: fold a sequence down to a single value.
var product = functools.reduce(Function(a, b): return a * b, [1, 2, 3, 4], 1)
io.print(product)                                 # => 24
```

## `statistics` — quick numeric summaries

```kirito
var io = import("io")
var statistics = import("statistics")

var samples = [4, 8, 15, 16, 23, 42]
io.print(statistics.mean(samples))     # => the average
io.print(statistics.median(samples))   # => the middle value
```

## Writing your own module

A module is just a `.ki` file — its top-level `var`s become its public members. Save this as
`geometry.ki`:

<!--norun (illustrates a separate file)-->
```kirito
# geometry.ki
var pi = 3.14159265358979
var circle_area = Function(radius):
    return pi * radius * radius
var circle_circumference = Function(radius):
    return 2 * pi * radius
```

Then import it by name (Kirito searches the current directory, the script's directory, and any
`--lib` directories) and use it exactly like a built-in module:

<!--norun (depends on geometry.ki existing on the import path)-->
```kirito
var io = import("io")
var geometry = import("geometry")
io.print(geometry.circle_area(2))      # => 12.566...
```

This is the same mechanism the standard library uses — there's no difference, to your program,
between a module written in C++ and one written in Kirito.

## The rest of the library

What you've seen is a slice. The full set, all imported the same way, includes:

- **Text & data:** `string`, `textwrap`, `csv`, `json`, `xml`, `base64`, and `serialize`/`dump` (save
  and reload whole object graphs, including shared references and cycles).
- **Numbers & math:** `math`, `statistics`, `random`, and the heavier numeric libraries — `matrix`,
  `complex`, and `tensor` — covered in the bonus lessons.
- **Iteration & functions:** `itertools`, `functools`, `collections`, `heapq`, `bisect`, `enum`,
  `copy`.
- **System, time & I/O:** `sys` (environment, `exit`, platform), `time` (clocks and a `DateTime`
  type), `io` (files, in-memory buffers, the filesystem).
- **Binary data:** the [`Bytes`](types.html#bytes) type (a raw byte sequence — the binary counterpart
  to `String`, with `s.encode()`/`b.decode()` between them), `zlib` and `gzip` (compression), and
  `hash` (md5/sha digests plus the crc/adler checksums). These all speak `Bytes`, so binary downloads
  and files round-trip byte-exactly.
- **Text matching:** `regex` — a linear-time regular-expression engine (Bonus Lesson 1).
- **Networking:** `net` — TCP sockets and a full HTTP client.
- **Command-line & data analysis:** `arg` (Bonus Lesson 2) and `tabular` (Bonus Lesson 3).

For the complete, signature-by-signature listing of every module, see the **Standard Library**
reference page. When in doubt, `inspect(import("name"))` prints a module's whole surface live.

## Try it

Use `random.Random` to generate 1000 dice rolls (1–6), tally them with `collections.Counter`, and
print how many times each face came up. Then use `statistics.mean` on the rolls to confirm it's near
3.5.

## What you learned

- Every module imports the same way and `inspect` lists its contents.
- A working knowledge of `random`, `json`, `time`, `collections`, `itertools`/`functools`, and
  `statistics`.
- A module is just a `.ki` file whose top-level `var`s are its exports — yours look identical to the
  built-in ones.

Next: the **[Capstone](course-15-capstone.html)**, where we assemble everything into one program.
