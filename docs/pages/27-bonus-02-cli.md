# Bonus Lesson 2 — Command-Line Programs

Most useful programs take **input from the command line**: a filename to process, a `--count`, a
`--verbose` flag. This lesson shows how a Kirito program sees its arguments (`arglist`/`argmain`) and
how the `arg` module turns a raw list of strings into clean, typed options — without the tedious
hand-parsing.

## `arglist` and `argmain`

Every file scope is pre-bound with two names, so you never import anything to get the basics:

- **`arglist`** — the command-line arguments as a `List` of Strings. For `ki greet.ki Ada --loud`,
  `arglist` is `["Ada", "--loud"]` (the interpreter and script name are stripped). It is **empty in
  an imported module** — arguments belong to the program you *run*, not to its libraries.
- **`argmain`** — a `Bool` that is `True` when this file was run directly and `False` when it was
  imported — so a file can guard its "run me" code with `if argmain:`.

The standard idiom is to guard your program's entry point so a file can be **both** a runnable script
and an importable library:

```kirito
var io = import("io")

var run = Function(args):
    io.print(f"got {len(args)} argument(s)")
    for a in args:
        io.print(f"  - {a}")

# Only execute when run directly; stays silent (and side-effect-free) when imported.
if argmain:
    run(arglist)
```

Run directly with `ki demo.ki one two` it prints the count and each argument; `import("demo")` from
another file runs nothing and exposes `run` for reuse.

## The `arg` module

You *can* pick apart `arglist` yourself, but every option, flag, default, and typo check becomes
fiddly index juggling. The `arg` module handles all of that.

`import("arg")` gives you a `Parser`. You **declare** the arguments, then **`parse`** a list of
strings yourself — typically `arglist`. The declaration methods chain (each returns the parser):

- `arg.Parser(description = "")` — create a parser.
- `p.positional(name, help = "")` — a required positional argument, consumed in order.
- `p.option(name, default = None, help = "")` — a `--name VALUE` option. **The value is converted to
  the type of `default`** — an Integer default parses an Integer, a Float default a Float, otherwise
  it stays a String.
- `p.flag(name, help = "")` — a boolean `--name` flag (default `False`, `True` when present).
- `p.parse(args)` — parse into a `Dict` keyed by name. Returns **`None`** if `-h`/`--help` was given
  (after printing the help), so the program can stop.

Here is the hand-rolled parser above, rewritten — declarations replace all the index juggling:

```kirito
var io = import("io")
var arg = import("arg")

var p = arg.Parser("greet someone")
p.positional("name")
p.option("count", 1)           # Integer, because the default is 1
p.flag("loud")

# In a real program this is `p.parse(arglist)`; here we pass an explicit list to keep the demo
# deterministic.
var opts = p.parse(["Ada", "--count", "3", "--loud"])

var name = opts["name"]
var greeting = f"Hello, {name}!"
if opts["loud"]:
    greeting = greeting.upper()
var n = 0
while n < opts["count"]:
    io.print(greeting)
    n = n + 1
```

This prints `HELLO, ADA!` three times. `parse` returns a Dict; you read results by name
(`opts["name"]`, `opts["count"]`, `opts["loud"]`).

## Typed options

The **default's type** decides how a value is parsed — you declare it once and every value comes back
in the right type:

```kirito
var io = import("io")
var arg = import("arg")

var p = arg.Parser()
p.option("width", 80)          # Integer default  -> Integer values
p.option("ratio", 1.0)         # Float default    -> Float values
p.option("title", "untitled")  # String default   -> stays a String

var opts = p.parse(["--width", "120", "--ratio", "0.75", "--title", "report"])
var width = opts["width"]
var ratio = opts["ratio"]
var title = opts["title"]
io.print(f"{width} ({type(width)})")
io.print(f"{ratio} ({type(ratio)})")
io.print(f"{title} ({type(title)})")
```

prints:

```text
120 (Integer)
0.75 (Float)
report (String)
```

If a value can't be converted (`--width abc`), `parse` throws a clear, catchable error rather than
handing you a bad value.

## Flags, short forms, `=`, and leftovers

`parse` understands the common command-line spellings, and collects any **extra** positionals under
the `"rest"` key:

```kirito
var io = import("io")
var arg = import("arg")

var p = arg.Parser()
p.positional("command")
p.option("level", 0)
p.flag("verbose")

# --name=value, a short -v for the flag (matched by first letter), and two leftover positionals.
var opts = p.parse(["build", "--level=2", "-v", "extra1", "extra2"])
var command = opts["command"]
var level = opts["level"]
var verbose = opts["verbose"]
var rest = opts["rest"]
io.print(f"command = {command}")
io.print(f"level   = {level}")
io.print(f"verbose = {verbose}")
io.print(f"rest    = {rest}")
```

prints:

```text
command = build
level   = 2
verbose = True
rest    = ['extra1', 'extra2']
```

So `--level 2`, `--level=2`, a short `-l 2`, and a short `-v` flag all work; anything not claimed by a
declared positional lands in `rest`.

## Automatic `--help`

You get a usage screen for free. When `-h` or `--help` appears anywhere, `parse` **prints the help
and returns `None`** — your program checks for that and stops:

```kirito
var io = import("io")
var arg = import("arg")

var p = arg.Parser("greet someone, maybe loudly")
p.positional("name", "who to greet")
p.option("count", 1, "how many times")
p.flag("loud", "shout it")

var opts = p.parse(["--help"])
if opts == None:
    io.print("(parse returned None — program would stop here)")
```

prints the generated help, then the sentinel line:

```text
usage: <name> [--count VALUE] [--loud]

greet someone, maybe loudly

arguments:
  <name>  who to greet
  --count VALUE  (default 1)  how many times
  --loud  shout it
  -h, --help  show this help and stop
(parse returned None — program would stop here)
```

## Handling bad input

Unknown options, a missing required positional, and unconvertible values all throw — wrap `parse` in
a `try` to report them nicely instead of crashing:

```kirito
var io = import("io")
var arg = import("arg")

var p = arg.Parser()
p.positional("name")
p.option("count", 1)

# Three things go wrong, each on its own parse; a bare `catch` also catches the thrown String.
for bad in [[], ["Ada", "--count", "ten"], ["Ada", "--oops"]]:
    try:
        discard p.parse(bad)
        io.print("ok")
    catch as e:
        io.print(f"error: {e}")
```

prints:

```text
error: missing required argument: <name>
error: option --count expects an integer, got 'ten'
error: unknown option: --oops
```

## Putting it together — a tiny CLI

Here is a complete, idiomatic command-line program: it declares its interface, parses `arglist`,
respects `--help`, and does real work. We drive it with an explicit list so the lesson runs, but in a
real file the one line that changes is `p.parse(arglist)`:

```kirito
var io = import("io")
var arg = import("arg")

# A word-counting tool: `wc.ki <text> [--by-char] [--upper]`
var wordcount = Function(args):
    var p = arg.Parser("count words (or characters) in a phrase")
    p.positional("text", "the phrase to measure")
    p.flag("by-char", "count characters instead of words")
    p.flag("upper", "report the phrase upper-cased")
    var opts = p.parse(args)
    if opts == None:
        return                       # --help was shown
    var text = opts["text"]
    if opts["upper"]:
        text = text.upper()
    var amount = len(text) if opts["by-char"] else len(text.split())
    var unit = "characters" if opts["by-char"] else "words"
    io.print(f"{text}: {amount} {unit}")

# In a real script: `if argmain: wordcount(arglist)`. Here we exercise it directly.
wordcount(["the quick brown fox", "--upper"])
wordcount(["hello world", "--by-char"])
```

prints:

```text
THE QUICK BROWN FOX: 4 words
hello world: 11 characters
```

Drop `wordcount(arglist)` behind an `if argmain:` and the same file becomes a real tool:
`ki wc.ki "the quick brown fox" --upper`.

## What you learned

- **`arglist`** is the command-line arguments (a `List` of Strings), empty inside imported modules;
  **`argmain`** is `True` only when the file is run directly — guard your entry point with
  `if argmain:`.
- The **`arg` module**: build a `Parser`, declare `positional`/`option`/`flag` (chainable), then
  `parse(arglist)` into a Dict. Option values are **typed by their default**, extras land in `"rest"`,
  and `-h`/`--help` prints usage and returns `None`.
- Parsing **throws clear, catchable errors** on bad input, so a `try` around `parse` gives friendly
  diagnostics.

Next bonus lesson: tabular **data analysis** with the `tabular` library.
