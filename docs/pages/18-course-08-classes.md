# Lesson 8 — Classes I: Data and Behavior

A **class** is a blueprint for your own type. It bundles **data** (attributes stored on each
instance) with **behavior** (methods that act on that data). Defining a class lets you model the
concepts in your program directly — a `BankAccount`, a `Vector`, a `Player` — instead of juggling
loose dictionaries and lists.

## Defining a class

You call the class like a function to create an **instance**. The special method `_init_` runs at
construction time and sets up the instance's attributes. Every method takes the instance as its first
parameter, named `self` by convention:

```kirito
var io = import("io")

class BankAccount:
    var _init_ = Function(self, owner, balance):
        self.owner = owner          # store data on the instance
        self.balance = balance

    var deposit = Function(self, amount):
        self.balance = self.balance + amount

    var describe = Function(self) -> String:
        return f"{self.owner} has {self.balance}"

var account = BankAccount("Ada", 100)   # calls _init_(self, "Ada", 100)
account.deposit(50)
io.print(account.describe())             # => Ada has 150
```

`account` is an instance; `account.balance` reads its attribute; `account.deposit(50)` calls a method
(Kirito passes `account` as `self` automatically).

## Methods can call other methods

A method reaches the instance's other methods and attributes through `self`:

```kirito
var io = import("io")

class Rectangle:
    var _init_ = Function(self, width, height):
        self.width = width
        self.height = height

    var area = Function(self) -> Integer:
        return self.width * self.height

    var is_square = Function(self) -> Bool:
        return self.width == self.height

    var summary = Function(self) -> String:
        # build on the other methods
        var shape = "rectangle"
        if self.is_square():
            shape = "square"
        return f"{shape} with area {self.area()}"

io.print(Rectangle(4, 4).summary())      # => square with area 16
io.print(Rectangle(3, 5).summary())      # => rectangle with area 15
```

## Private members

An attribute or method whose name has a **single leading underscore** (and no trailing one) is
**private** — reachable only from inside methods of the same class (or one of its subclasses; the
boundary is the class chain, not the defining class). Use it for internal state that callers
shouldn't touch:

```kirito
var io = import("io")

class Counter:
    var _init_ = Function(self):
        self._count = 0             # private: an implementation detail

    var increment = Function(self):
        self._count = self._count + 1

    var value = Function(self) -> Integer:
        return self._count          # a method exposes it read-only

var c = Counter()
c.increment()
c.increment()
io.print(c.value())                  # => 2
```

Reaching `c._count` from *outside* the class is an error — privacy is enforced, not just a
convention. This lets you change internals later without breaking callers, because they could only
ever use the public methods.

## A worked example: a stack

```kirito
var io = import("io")

class Stack:
    var _init_ = Function(self):
        self._items = []            # private storage

    var push = Function(self, item):
        self._items.append(item)

    var pop = Function(self):
        if len(self._items) == 0:
            return None             # nothing to pop
        return self._items.pop()

    var size = Function(self) -> Integer:
        return len(self._items)

var work = Stack()
work.push("a")
work.push("b")
io.print(work.pop(), work.size())    # => b 1
```

**Walkthrough:** the list `_items` is private, so callers can only interact through `push`/`pop`/
`size` — the stack's *interface*. They can't reach in and corrupt the list, and you could swap the
list for something else without anyone noticing. That separation of interface from implementation is
the whole point of classes.

## Try it

Write a `Temperature` class storing a private `_celsius` attribute set in `_init_`, with methods
`celsius()` and `fahrenheit()` (`c * 9 / 5 + 32`). Construct `Temperature(100)` and print both
readings.

## What you learned

- `class` bundles attributes (data) and methods (behavior); call the class to construct an instance.
- `_init_` initializes a new instance; every method takes `self` first.
- Methods reach the instance's state and other methods through `self`.
- A single leading underscore makes a member **private**, enforcing the interface/implementation split.
