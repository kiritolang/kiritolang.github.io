# Lesson 3 — Strings

Text is everywhere — names, file contents, user input, output. Kirito strings are **Unicode**
(indexed by code point, not byte), immutable, and come with a rich set of methods.

## Creating and joining strings

```kirito
var io = import("io")
var greeting = "Hello"
var subject = "world"
io.print(greeting + ", " + subject + "!")   # => Hello, world!   (+ concatenates)
io.print("ab" * 3)                            # => ababab          (* repeats)
io.print(len("café"))                         # => 4   (4 code points, not 5 bytes)
```

## Ways to write a string

Quote a string with `'` or `"` — use whichever lets you put the *other* quote inside without
escaping. Tripling either quote (`'''`/`"""`) makes a **multiline** string. An `r` prefix makes a
**raw** string where backslashes are literal:

```kirito
var io = import("io")
io.print('single quotes work too')
io.print("it's an apostrophe")        # ' inside "..." is just a character
io.print('she said "hello"')          # " inside '...' is just a character
io.print("""a string
that spans
several lines""")
io.print(r"C:\path\to\file")          # raw: \p \t \f stay as backslash + letter
```

Inside a normal (non-raw) string, **escapes** stand for special characters: `\n` newline, `\t` tab,
`\\` a backslash, `\"`/`\'` a quote, and `\xHH` a byte from two hex digits. A raw string interprets
none of these. (These prefixes and quote styles also apply to f-strings: `f'...'`, `f"""..."""`, and
raw `rf"..."` all work.)

## f-strings: the readable way to build text

An `f"..."` string evaluates `{expression}` pieces inline. This is almost always clearer than
chains of `+` and `String(...)`:

```kirito
var name = "Ada"
var score = 95
io.print(f"{name} scored {score} points")     # => Ada scored 95 points
io.print(f"next year: {score + 5}")           # => next year: 100   (any expression works)
```

You can add a **format spec** after a colon to control width, precision, and padding:

```kirito
var pi = 3.14159
io.print(f"pi to 2 dp: {pi:.2f}")              # => pi to 2 dp: 3.14
io.print(f"padded: {42:05d}")                  # => padded: 00042
io.print(f"hex: {255:#x}")                     # => hex: 0xff
```

## Indexing and slicing

Strings are indexed from `0`. Negative indices count from the end. A **slice** `[start:end]`
returns the substring from `start` up to (but not including) `end`:

```kirito
var word = "Kirito"
io.print(word[0])        # => K     (first character)
io.print(word[-1])       # => o     (last character)
io.print(word[0:3])      # => Kir   (characters 0, 1, 2)
io.print(word[3:])       # => ito   (from 3 to the end)
io.print(word[:3])       # => Kir   (from the start to 3)
```

Because strings are immutable, slicing always returns a **new** string; the original is untouched.

## The essential methods

Strings carry dozens of methods. Here are the ones you'll reach for daily:

```kirito
var raw = "  Hello, World  "
io.print(raw.strip())               # => Hello, World   (trim surrounding whitespace)
io.print("hello".upper())           # => HELLO
io.print("HELLO".lower())           # => hello
io.print("hello".startswith("he"))  # => True
io.print("hello".endswith("lo"))    # => True
io.print("a,b,c".split(","))        # => ['a', 'b', 'c']  (split into a List)
io.print("-".join(["a", "b", "c"])) # => a-b-c          (join a List into a String)
io.print("banana".replace("a", "*"))# => b*n*n*
io.print("banana".count("a"))       # => 3
io.print("banana".find("n"))        # => 2  (index of first match, or -1 if absent)
```

Many search methods accept optional start/end bounds, and `split`/`replace` take an optional count —
see the **Types** reference for the full list.

## Classifying characters

```kirito
io.print("42".isdigit())            # => True
io.print("abc".isalpha())           # => True
io.print("abc123".isalnum())        # => True
io.print("   ".isspace())           # => True
```

## A worked example: title-casing a sentence

```kirito
var io = import("io")

var title_case = Function(sentence : String) -> String:
    var words = sentence.split(" ")
    var capitalized = []
    for word in words:
        if len(word) > 0:
            # Upper-case the first code point, keep the rest as-is.
            capitalized.append(word[0].upper() + word[1:])
    return " ".join(capitalized)

io.print(title_case("the quick brown fox"))   # => The Quick Brown Fox
```

**Walkthrough:** we split on spaces into a List of words, rebuild each word as "first letter upper +
the rest unchanged", and re-join with spaces. Slicing (`word[1:]`) and `join` carry the work;
because strings are immutable we compose new ones rather than mutating in place.

## Try it

Write `is_palindrome(text)` that returns `True` if `text` reads the same forwards and backwards,
ignoring case and spaces. (Hint: `text.lower().replace(" ", "")`, then compare against its reverse —
you can reverse a string with a slice once you've met them, or build it character by character.)

## What you learned

- Strings are immutable Unicode, concatenated with `+` and repeated with `*`.
- f-strings with format specs are the readable way to build text.
- Indexing, negative indices, and slicing.
- The everyday methods: `strip`, `upper`/`lower`, `split`/`join`, `replace`, `find`, `count`, the `is*` family.
