# Lesson 7 — Type Annotations and Runtime Checks

Kirito is dynamically typed — a name can hold any value — but it is also **strongly** typed: it never
silently coerces incompatible values. On top of that, you can add optional **type annotations** to
function parameters and return values. Unlike type *hints* in some languages, Kirito's annotations
are **checked at runtime**.

## Annotating parameters and returns

Add `: Type` after a parameter and `-> Type` after the parameter list:

```kirito
var io = import("io")

var repeat = Function(text : String, times : Integer) -> String:
    return text * times

io.print(repeat("ab", 3))      # => ababab
```

When you call `repeat`, Kirito verifies each argument is an instance of the named type and that the
returned value matches `-> String`. A mismatch throws a clear, catchable error instead of producing
nonsense:

<!--norun (intentional annotation violation)-->
```kirito
repeat("ab", "3")              # error: argument 'times' must be Integer, got String
```

This turns a whole class of bugs into immediate, located errors at the boundary of your function.

## Annotations are inheritance-aware

An annotation is satisfied by the named type **or any subclass** of it. This matters once you have
class hierarchies (Lesson 10):

```kirito
var io = import("io")

class Animal:
    var _init_ = Function(self, name):
        self.name = name

class Dog(Animal):
    var bark = Function(self) -> String:
        return f"{self.name} says woof"

# Accepts any Animal — including a Dog, which is a subclass.
var describe = Function(creature : Animal) -> String:
    return creature.name

io.print(describe(Dog("Rex")))     # => Rex   (a Dog satisfies the Animal annotation)
```

## `Integer` and `Float` are distinct — no implicit widening

An annotation checks the value's **exact type** (or a subclass). `Integer` and `Float` are separate
types, so an Integer does **not** satisfy a `Float` annotation — pass a Float, or convert with
`Float(n)`:

```kirito
var io = import("io")

var half = Function(x : Float) -> Float:
    return x / 2.0

io.print(half(3.0))         # => 1.5
io.print(half(Float(3)))    # => 1.5   (convert an Integer first)
```

Calling `half(3)` with a bare Integer throws `argument 'x' must be Float, got Integer`. (Inside
arithmetic, mixing an Integer and a Float still promotes to Float — it is only the annotation *check*
that is strict.)

## `Any` and no annotation accept everything

Leaving a parameter unannotated, or annotating it `Any`, accepts any value — use this when a function
genuinely is generic:

```kirito
var io = import("io")

var identity = Function(x : Any):
    return x

var describe_type = Function(value):       # no annotation — anything goes
    return type(value)

io.print(identity(42), identity("hi"))     # => 42 hi
io.print(describe_type([1, 2]))            # => List
```

## Checking types yourself: `type` and `isinstance`

When you need to branch on a value's type at runtime, `type(x)` gives the type name, and
`isinstance(x, T)` tests membership (inheritance-aware), where `T` is a type-name String or a class:

```kirito
var io = import("io")

var stringify_smart = Function(value) -> String:
    if isinstance(value, "Float"):
        return f"{value:.2f}"              # floats get 2 decimal places
    elif isinstance(value, "List"):
        return f"a list of {len(value)} items"
    else:
        return String(value)

io.print(stringify_smart(3.14159))         # => 3.14
io.print(stringify_smart([1, 2, 3]))       # => a list of 3 items
io.print(stringify_smart("plain"))         # => plain
```

**Walkthrough:** annotations guard the *edges* of a function (catching bad callers automatically),
while `isinstance` lets you *dispatch* on type inside a function when the behavior genuinely depends
on it. Prefer annotations for contracts and reserve explicit `isinstance` checks for when you really
do want different logic per type.

## When to annotate

Annotate the public functions whose contracts you want enforced and self-documented; skip annotations
on small local helpers where they'd just be noise. The annotation doubles as documentation —
`inspect(some_function)` prints the full signature with its types.

## Try it

Write `scale(values, factor : Float)` annotated to take a `List` of numbers and return a `List`,
multiplying each by `factor`. Call it correctly, then deliberately call it with a String factor and
wrap the call in a `try`/`catch` (Lesson 11 covers `try`/`catch` in depth) to see the annotation
error as a value.

## What you learned

- Kirito is dynamically but strongly typed — no silent coercion.
- `: Type` / `-> Type` annotations are checked at runtime and are inheritance-aware.
- `Any` or no annotation accepts anything.
- `type(x)` and `isinstance(x, T)` for explicit runtime type checks and dispatch.
