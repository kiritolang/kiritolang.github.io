# Bonus Lesson 1 — Regular Expressions

A **regular expression** (regex) is a tiny language for describing *patterns* in text: "a run of
digits", "an email address", "a word at the start of a line". Kirito's `regex` module lets you
search, extract, replace, and split text by pattern. It's backed by a **linear-time** engine, so
unlike many languages' regex, it can never blow up into the exponential "catastrophic backtracking"
that hangs a program — more on that at the end.

```kirito
var io = import("io")
var re = import("regex")
io.print(re.search(r"\d+", "order #4821").group())   # => 4821
```

## A note on backslashes

Regex uses backslashes a lot (`\d`, `\w`, `\b`). In an ordinary string `\d` is not a valid escape, so
you'd have to double every backslash (`"\\d+"`). Kirito has **raw strings** (`r"..."`), where
backslashes are literal — so `r"\d+"` is exactly the three characters `\d+`. **Prefer raw strings for
regex patterns:**

```kirito
io.print(re.search(r"\d+", "abc123").group())   # => 123    (r"\d+" is the 3-char pattern \d+)
```

(The doubled form `"\\d+"` works too, but `r"\d+"` reads far closer to the pattern you mean.)

## search, match, fullmatch

The three entry points differ only in *where* the pattern must line up:

```kirito
io.print(re.search(r"\d+", "id 4821 x").group())     # => 4821   (anywhere)
io.print(re.match(r"\d+", "id 4821 x"))               # => None   (must start at index 0)
io.print(re.match("id", "id 4821 x").group())         # => id     (matches at the start)
io.print(re.fullmatch(r"\d+", "4821").group())        # => 4821   (must cover the WHOLE string)
io.print(re.fullmatch(r"\d+", "4821x"))               # => None
```

A successful call returns a **Match** object; a failed one returns `None`. Always check for `None`
before using the result:

```kirito
var found = re.search("z+", "all quiet... zzz!")
if found != None:
    io.print("found:", found.group())                 # => found: zzz
```

## The building blocks of a pattern

**Literals** match themselves; the dot `.` matches any one character (except a newline):

```kirito
io.print(re.search("a.c", "xaqcy").group())           # => aqc
```

**Character classes** match one character out of a set:

```kirito
io.print(re.search("[aeiou]", "rhythm and blues").group())   # => a
io.print(re.search("[A-Z]+", "shouting LOUDLY now").group()) # => LOUDLY   (a range)
io.print(re.search("[^0-9]+", "123abc").group())             # => abc      (^ negates the set)
```

**Shorthand classes** name the common sets (ASCII): `\d` digits, `\w` word chars
(letters/digits/`_`), `\s` whitespace — and their uppercase forms `\D \W \S` are the negations:

```kirito
io.print(re.search(r"\w+", "  hi_there!").group())    # => hi_there
io.print(re.search(r"\s+", "a   b").group())          # => "   " (three spaces)
```

## Quantifiers: how many

| Quantifier | Means |
|---|---|
| `*` | zero or more |
| `+` | one or more |
| `?` | zero or one (optional) |
| `{n}` | exactly n |
| `{n,m}` | between n and m |
| `{n,}` | n or more |

```kirito
io.print(re.search("ab*c", "ac").group())             # => ac     (* allows zero b's)
io.print(re.search("ab+c", "abbbc").group())          # => abbbc
io.print(re.search("colou?r", "color").group())       # => color  (? makes the u optional)
io.print(re.search(r"\d{3}-\d{4}", "call 555-0199 now").group())   # => 555-0199
```

### Greedy vs lazy

By default a quantifier is **greedy** — it grabs as much as it can. Add a `?` to make it **lazy** —
grabbing as little as possible. This is the classic "match the tag" example:

```kirito
io.print(re.search("<.*>", "<a><b>").group())         # => <a><b>   (greedy: as much as possible)
io.print(re.search("<.*?>", "<a><b>").group())        # => <a>      (lazy: as little as possible)
```

## Anchors and boundaries

These match a *position*, not a character: `^` start, `$` end, `\b` a word boundary.

```kirito
io.print(re.search("^id", "id 99").group())           # => id   (only because it's at the start)
io.print(re.search("99$", "id 99").group())           # => 99   (only at the end)
io.print(re.findall(r"\bcat\b", "cat scatter cat"))  # => ['cat', 'cat']  (whole word only, not "scatter")
```

## Capturing groups

Parentheses `( )` create a **group** — a sub-part of the match you can pull out afterwards with
`.group(n)` (group 0 is the whole match). `.groups()` returns them all, and `.span()` gives the
match's `[start, end]` in code-point indices:

```kirito
var m = re.search(r"(\d{4})-(\d{2})-(\d{2})", "log 2024-06-07 ok")
io.print(m.group())     # => 2024-06-07   (group 0: the whole match)
io.print(m.group(1))    # => 2024         (first parenthesized group)
io.print(m.group(2))    # => 06
io.print(m.groups())    # => ['2024', '06', '07']
io.print(m.span())      # => [4, 14]
```

A group made optional with `?` may not participate; then its `group(n)` is `None`:

```kirito
var partial = re.search(r"(\d+)?-(\d+)", "-5")
io.print(partial.group(1))     # => None   (the first group matched nothing)
io.print(partial.group(2))     # => 5
```

### Named groups

Naming a group with `(?P<name>...)` (or `(?<name>...)`) lets you fetch it by name and get them all
as a Dict with `.groupdict()` — far clearer than counting parentheses:

```kirito
var who = re.search(r"(?P<user>\w+)@(?P<host>[\w.]+)", "write ada@kirito.dev today")
io.print(who.group("user"))    # => ada
io.print(who.group("host"))    # => kirito.dev
io.print(who.groupdict())      # => {'host': 'kirito.dev', 'user': 'ada'}  (key order may vary)
```

## Alternation

`|` means "either side". Group it with `( )` to bound the choice:

```kirito
io.print(re.search("cat|dog|bird", "I have a dog").group())   # => dog
io.print(re.findall("(jan|feb|mar)", "feb and mar"))           # => ['feb', 'mar']
```

## Finding every match

`findall` returns the matches as a List; `finditer` returns the Match objects (for when you need
positions or groups of each):

```kirito
io.print(re.findall(r"\d+", "3 cats, 12 dogs, 1 fox"))     # => ['3', '12', '1']   (0 groups -> whole matches)
io.print(re.findall(r"(\w+)=(\d+)", "x=1 y=22"))           # => [['x', '1'], ['y', '22']]  (>1 group -> tuples)
for hit in re.finditer(r"\d+", "a1 b22"):
    io.print(hit.group(), "at", hit.start())                # => 1 at 1 / 22 at 4
```

## Replacing with sub

`sub(pattern, replacement, text)` replaces every match. The replacement can be a **template String**
(where `\1`, `\2`, `\g<name>` insert captured groups) or a **function** that receives each Match and
returns its replacement:

```kirito
io.print(re.sub(r"\s+", " ", "too   many    spaces"))       # => too many spaces
io.print(re.sub(r"(\w+)@(\w+)", r"\2.\1", "user@host"))   # => host.user   (template with backrefs)

var redact = Function(m):
    return "*" * len(m.group())
io.print(re.sub(r"\d+", redact, "card 1234 5678"))          # => card **** ****   (callable replacement)
```

Pass a count to limit how many are replaced: `re.sub("a", "X", "aaaa", 2)` → `XXaa`.

## Splitting

`split` breaks text at each match. If the pattern has groups, the captured separators are kept:

```kirito
io.print(re.split(r",\s*", "a, b,c ,  d"))     # => ['a', 'b', 'c ', 'd']
io.print(re.split(r"(\s+)", "one two"))         # => ['one', ' ', 'two']   (separator retained)
```

## Flags

Flags change how a pattern matches. Combine them with `+`:

- `re.IGNORECASE` (`re.I`) — case-insensitive.
- `re.MULTILINE` (`re.M`) — `^` and `$` match at every line, not just the whole string.
- `re.DOTALL` (`re.S`) — `.` also matches newlines.

```kirito
io.print(re.findall("cat", "Cat CAT cat", re.IGNORECASE))   # => ['Cat', 'CAT', 'cat']
io.print(re.findall(r"^\w+", "one\ntwo\nthree", re.MULTILINE))  # => ['one', 'two', 'three']
```

You can also set a flag inside the pattern with `(?i)` / `(?m)` / `(?s)`:

```kirito
io.print(re.search("(?i)hello", "HELLO there").group())     # => HELLO
```

An inline flag applies to the **whole** pattern no matter where it appears (it is not positional, and
there are no scoped `(?i:...)` groups) — `re.search("a(?i)b", "AB")` matches. This differs from Python,
where `(?i)` must lead the pattern.

## Compile once, reuse many times

If you'll use a pattern repeatedly, `compile` it once into a `Regex` object and call methods on that
— it parses the pattern a single time:

```kirito
var word = re.compile("[A-Za-z]+", re.IGNORECASE)
io.print(word.findall("Hello, World!"))    # => ['Hello', 'World']
io.print(word.pattern)                      # => [A-Za-z]+   (the source pattern)
io.print(word.groups)                       # => 0           (number of capturing groups)
```

And when you need to match a *literal* string that might contain regex metacharacters, `escape` it:

```kirito
io.print(re.escape("a.b*c"))                            # => a\.b\*c
io.print(re.search(re.escape("3.14"), "pi=3.14").group())   # => 3.14  (the dot is literal here)
```

## A worked example: parsing log lines

```kirito
var io = import("io")
var re = import("regex")

# A log line looks like:  2024-06-07 12:30:45 ERROR  disk full
var line_pattern = re.compile(r"(?P<date>\d{4}-\d{2}-\d{2}) (?P<time>\d{2}:\d{2}:\d{2}) (?P<level>\w+)\s+(?P<msg>.*)")

var parse_log = Function(line):
    var m = line_pattern.fullmatch(line)
    if m == None:
        return None                          # not a well-formed line
    return m.groupdict()

var entry = parse_log("2024-06-07 12:30:45 ERROR disk full")
io.print(entry["level"])     # => ERROR
io.print(entry["date"])      # => 2024-06-07
io.print(entry["msg"])       # => disk full
io.print(parse_log("garbage"))   # => None
```

**Walkthrough:** one compiled, named-group pattern captures every field of a log line at once;
`fullmatch` insists the *whole* line fits the shape (so malformed input cleanly returns `None`); and
`groupdict()` hands back a ready-to-use Dict keyed by field name. This is the everyday power of
regex — turning unstructured text into structured data in a few lines.

## The linear-time guarantee (and what's left out)

Kirito's engine is built so matching is always **O(text length × pattern size)** — there is no
backtracking. The famous pattern that brings other engines to their knees runs instantly here:

```kirito
var re = import("regex")
io.print(re.search("(a+)+b", "a" * 4000 + "c"))   # => None, in milliseconds (never hangs)
```

A *backtracking* regular-expression engine runs `(a+)+b` on a long run of `a`'s in *exponential*
time — effectively a hang, and a real source of denial-of-service bugs. Kirito can't fall into that
trap: its engine is linear-time by construction.

The price of that guarantee (the same trade-off the RE2 library makes) is that two features which
*require* backtracking are not supported, and throw a clear error rather than misbehaving:

- **backreferences** like `(\\w+)\\1`
- **lookaround** like `(?=...)`, `(?!...)`, `(?<=...)`, `(?<!...)`

```kirito
try:
    discard re.compile(r"(a)\1")
catch as e:
    io.print("rejected:", e)     # => rejected: invalid regex: backreferences are not supported ...
```

## Try it

Write a function `extract_prices(text)` that returns a List of every dollar amount in `text` —
matching things like `$3`, `$19.99`, `$1000`. (Hint: a `$` is a metacharacter, so escape it as `\\$`;
then `\\d+` and an optional decimal part `(\\.\\d{2})?`.) Test it on
`"coffee $3, lunch $19.99, rent $1000"`.

## What you learned

- `re.search`/`match`/`fullmatch` return a Match or `None`; double every backslash in patterns.
- Literals, `.`, classes `[...]`, shorthands `\d \w \s`, quantifiers (greedy and lazy), anchors `^ $ \b`.
- Capturing and named groups; `.group()/.groups()/.groupdict()/.span()`.
- `findall`/`finditer`, `sub` (template and callable), `split`, flags, `compile`, and `escape`.
- The engine is linear-time — no catastrophic backtracking — at the cost of backreferences and
  lookaround, which it rejects with a clear error.
