# Lesson 13 — Files, I/O, and Streams

Programs that matter usually read and write data. The `io` module covers it all: console I/O, files,
in-memory buffers, and filesystem helpers — all built on one interchangeable **stream** abstraction.

## Console I/O

`io.print` writes a line; `io.input` reads one; `io.write` writes without a trailing newline:

<!--norun (io.input blocks waiting for stdin)-->
```kirito
var io = import("io")
var name = io.input("What's your name? ")    # prints the prompt, reads a line
io.print(f"Hello, {name}!")
```

## Writing and reading files

`io.open(path, mode)` returns a file object. The modes are `"r"` (read), `"w"` (truncate then
write), `"a"` (append), and `"r+"` (read/write) — and, with a trailing `"b"` (`"rb"`/`"wb"`/…), a
**binary** mode where `read` returns raw `Bytes` and `write` accepts them (use it for non-text files
like images or `.gz` data). Always use a `with` block so the file closes itself:

```kirito
var io = import("io")
var path = import("path")
var notes = path.join(path.getcwd(), "notes.txt")

# Write three lines.
with io.open(notes, "w") as f:
    f.writelines(["first\n", "second\n", "third\n"])

# Read it all back at once.
with io.open(notes, "r") as f:
    io.print(f.read())            # => first / second / third

# Append a line.
with io.open(notes, "a") as f:
    f.write("fourth\n")
```

## Reading line by line

A file is iterable — looping over it yields one line at a time, which is the memory-friendly way to
process large files:

```kirito
var io = import("io")
var path = import("path")
var notes = path.join(path.getcwd(), "notes.txt")

var line_number = 0
with io.open(notes, "r") as f:
    for line in f:
        line_number = line_number + 1
        io.print(f"{line_number}: {line.strip()}")
# => 1: first / 2: second / 3: third / 4: fourth
discard path.remove(notes, missing_ok = True)           # clean up
```

`f.readline()` reads one line, and `f.readlines()` reads them all into a List, if you prefer explicit
control over the loop.

## Filesystem helpers: `path` and `io`

Path operations — interpreting a path string and querying the filesystem about it — live in the
dedicated **`path`** module (Kirito's `os.path`):

```kirito
var io = import("io")
var path = import("path")
var here = path.getcwd()
io.print(path.exists(here))            # => True
io.print(path.isdir(here))             # => True
io.print(path.basename("/a/b/c.txt"))  # => c.txt
io.print(path.dirname("/a/b/c.txt"))   # => /a/b
io.print(path.splitext("report.csv"))  # => ['report', '.csv']
io.print(path.join("dir", "sub", "file.txt"))   # => dir/sub/file.txt  (always '/')
```

`path` also has `isfile` and `getsize`, and `path.join` for building paths (os.path.join semantics,
always `/`). The operations that *change* or *list* the filesystem are all in `path` too: `mkdir`,
`remove`, `rmtree` (recursive `rm -rf`), `rename`, `chmod`, `listdir`, `walk`, and `getcwd`.

## In-memory buffers: `BytesIO`

A `BytesIO` is a stream backed by memory, not disk — perfect for assembling text, or for tests that
shouldn't write files. It supports the same `read`/`write`/`readline` interface as a file:

```kirito
var io = import("io")
var buffer = io.BytesIO()
buffer.write("part one; ")
buffer.write("part two")
io.print(buffer.getvalue())        # => part one; part two
```

## Streams are interchangeable

Here's the unifying idea: `io.print`/`write`/`input`/`read` act on whatever is bound to `io.stdout` /
`io.stdin`, and those are **rebindable**. Point `io.stdout` at a file or a `BytesIO` and every
`io.print` flows there instead of the screen — instant output redirection:

```kirito
var io = import("io")
var capture = io.BytesIO()

io.stdout = capture                # redirect stdout
io.print("this is captured")
io.print("so is this")
io.stdout = io.__stdout__          # restore the original (kept safe here)

io.print("back to the screen")     # => back to the screen
io.print("captured was:", capture.getvalue())
# => captured was: this is captured
#    so is this
```

For a one-off redirect without rebinding, every console function takes a `stream=` keyword:

```kirito
var io = import("io")
var log = io.BytesIO()
io.print("only to the log", stream=log)    # this one call goes to `log`
io.print("to the screen")                    # this one uses the normal stdout
io.print("log holds:", log.getvalue())       # => log holds: only to the log
```

**Walkthrough:** because files, `BytesIO`, the standard streams, and even your own objects (anything
with `write`/`readline`) all speak the same stream protocol, redirection is *just an assignment*. The
same `io.print` you've used since Lesson 1 works unchanged whether it targets the terminal, a file,
or a buffer.

## Try it

Write a function `save_report(lines, path)` that opens `path` for writing in a `with` block and
writes each String in `lines` on its own line. Then write `load_report(path)` that reads the lines
back into a List (stripping newlines). Round-trip a few lines and confirm they match.

## What you learned

- `io.print`/`input`/`write` for the console; `io.open(path, mode)` with `"r"/"w"/"a"/"r+"`.
- Use `with` so files close themselves; iterate a file for line-by-line reading.
- Filesystem and path helpers (`exists`, `join`, `basename`, `listdir`, `walk`, …).
- `BytesIO` is an in-memory stream; `io.stdout` is rebindable and every call takes `stream=` —
  redirection is just an assignment.
