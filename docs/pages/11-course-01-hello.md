# Lesson 1 — Hello, Kirito

Welcome to the Kirito course: sixteen lessons that take you from your first line of output to a
complete program. Every lesson builds on the previous ones, and every code block is a real program
you can run. Type them into the REPL or save them to a `.ki` file — learning sticks when you run and
tweak the code yourself.

## Running code

There are two ways to run Kirito.

**The REPL** — launch `ki` with no arguments and type expressions; it prints each result. Multi-line
blocks (an `if`, a `for`, a function) continue with a `...` prompt until you enter a blank line.

```text
$ ki
>>> 1 + 2
3
>>> var io = import("io")
>>> for i in range(3):
...     io.print(i)
...
0
1
2
```

**A script** — save your program to a file (the extension is `.ki`) and run it:

```text
$ ki hello.ki
```

## Your first program

Output lives in the `io` module. You bring a module into scope with `import`, binding it to a name —
by convention the module's own name:

```kirito
var io = import("io")
io.print("Hello, Kirito!")
```

`io.print` writes its arguments separated by spaces and adds a newline. It accepts any number of
values of any type:

```kirito
io.print("the answer is", 42, "and pi is about", 3.14159)
# => the answer is 42 and pi is about 3.14159
```

Throughout this course we assume `var io = import("io")` has run at the top of the file; later
lessons won't repeat it in every snippet.

## Comments

A `#` begins a comment that runs to the end of the line. Use comments to explain *why*, not to
restate *what* the code obviously does:

```kirito
var seconds_per_day = 24 * 60 * 60   # 86400 — handy for date math later
io.print(seconds_per_day)
```

## Reading command-line arguments

Every file runs with two names automatically in scope: **`arglist`**, the list of arguments the
program was launched with (`arglist[0]` is the first one — Kirito does not put the program name in
it), and **`argmain`**, a Bool that is `True` when *this* file was run directly and `False` when it
was loaded by another file via `import`.

<!--norun (arglist is empty when run without arguments)-->
```kirito
# greet.ki — run as:  ki greet.ki Ada
var io = import("io")
if len(arglist) > 0:
    io.print(f"Hello, {arglist[0]}!")
else:
    io.print("Hello, stranger! (pass a name on the command line)")
```

`argmain` lets a file act as both a reusable module *and* a runnable program — put the "run me
directly" code behind `if argmain:`, so it doesn't fire when someone imports the file:

<!--norun (illustrates the run-vs-import idiom)-->
```kirito
var io = import("io")

var greet = Function(name):           # importable by other files
    return f"Hello, {name}!"

if argmain:                            # only runs when this file is the program
    io.print(greet("world"))
```

For richer command lines (named options, flags, defaults), the `arg` module gives you a parser —
you'll meet it in the standard-library tour.

## Try it

Write a program that prints three lines: a greeting, the current year as an Integer, and a short
sentence about yourself. Run it as a file *and* paste it into the REPL to feel the difference.

## What you learned

- Kirito runs as an interactive REPL or as a `.ki` script.
- `import` loads a module; `io.print` is your window to the world.
- `#` starts a comment; `arglist` holds the command-line arguments and `argmain` is `True` only when
  the file is run directly (the `if argmain:` idiom).

Next: the values those programs are made of.
