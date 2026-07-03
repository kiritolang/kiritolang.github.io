# Lesson 11 — Errors and Exceptions

Things go wrong: a file is missing, input is malformed, a divisor is zero. **Exceptions** let you
signal such problems where they're detected and handle them where you can do something about it —
without threading error codes through every return value. Kirito uses indented blocks
with the keyword names `throw` (to throw), `try`/`catch`/`finally` (to handle).

## Throwing with `throw`

`throw` throws an exception, which unwinds the program until a matching `catch` handles it (or, if
none does, stops the program with a message):

```kirito
var io = import("io")

var safe_divide = Function(a, b) -> Float:
    if b == 0:
        throw "cannot divide by zero"      # you can throw any value, even a String
    return a / b

io.print(safe_divide(10, 2))               # => 5.0
```

## Handling with `try` / `catch`

Wrap risky code in `try:`; put the recovery in `catch:`. A bare `catch` handles anything thrown:

<!--norun (uses `safe_divide` defined in an earlier snippet on this page)-->
```kirito
var io = import("io")

try:
    var result = safe_divide(10, 0)
    io.print(result)                        # skipped — the throw jumps past it
catch as e:
    io.print(f"handled: {e}")               # => handled: cannot divide by zero
```

`catch as e` binds the thrown value to `e` so you can inspect or report it.

## Custom exception types

Throwing a String works, but a **class** carries structured data and lets callers catch *specific*
kinds of error. Define a class, throw an instance, and catch by type:

```kirito
var io = import("io")

class ValidationError:
    var _init_ = Function(self, field, message):
        self.field = field
        self.message = message

var validate_age = Function(age):
    if age < 0:
        throw ValidationError("age", "must not be negative")
    if age > 150:
        throw ValidationError("age", "is implausibly large")
    return age

try:
    discard validate_age(-5)
catch ValidationError as err:               # catch only this type (and subclasses)
    io.print(f"invalid {err.field}: {err.message}")
# => invalid age: must not be negative
```

`catch ValidationError as err` matches that class and any subclass of it; other exception types would
sail past this handler to an outer one. (The `discard` marks that we call `validate_age` only to
trigger the error and don't need its return value.)

## `finally`: cleanup that always runs

A `finally:` block runs whether or not an exception occurred — perfect for releasing resources:

```kirito
var io = import("io")

var attempt = Function(should_fail):
    try:
        if should_fail:
            throw "boom"
        io.print("work succeeded")
    catch as e:
        io.print(f"work failed: {e}")
    finally:
        io.print("cleanup runs no matter what")

attempt(False)    # => work succeeded / cleanup runs no matter what
attempt(True)     # => work failed: boom / cleanup runs no matter what
```

## Multiple, specific handlers

Order handlers from specific to general. List several `catch` clauses to react differently per type:

```kirito
var io = import("io")

class NotFound:
    var _init_ = Function(self, key):
        self.key = key
class Forbidden:
    var _init_ = Function(self):
        self.dummy = 0

var lookup = Function(key):
    if key == "secret":
        throw Forbidden()
    if key == "missing":
        throw NotFound(key)
    return f"value for {key}"

var fetch = Function(key):
    try:
        io.print(lookup(key))
    catch NotFound as e:
        io.print(f"404: {e.key} not found")
    catch Forbidden as e:
        io.print("403: access denied")

fetch("ok")        # => value for ok
fetch("missing")   # => 404: missing not found
fetch("secret")    # => 403: access denied
```

## `assert` for invariants

`assert` checks a condition you believe must hold; if it's false it throws with your message. Use it
to catch programming mistakes early (not for ordinary input validation, which deserves real
exceptions):

```kirito
var io = import("io")

var average = Function(numbers) -> Float:
    assert len(numbers) > 0, "average of an empty list is undefined"
    return sum(numbers) / len(numbers)

io.print(average([2, 4, 6]))     # => 4.0
```

## Try it

Write `parse_positive_int(text)` that converts `text` to an Integer and `throw`s a custom
`ParseError` (with the offending text) if the result is negative or the conversion fails. Call it
inside a `try`/`catch ParseError as e` and print a friendly message. (Hint: wrap the `Integer(text)`
call in its own `try` to turn a conversion failure into your `ParseError`.)

## What you learned

- `throw` throws any value; uncaught exceptions stop the program with a message.
- `try` / `catch as e` handles errors; `catch Type as e` handles a specific type (and subclasses).
- Custom exception classes carry structured data; list multiple handlers specific-to-general.
- `finally` always runs (cleanup); `assert` checks invariants.
