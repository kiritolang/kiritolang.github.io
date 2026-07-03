# Lesson 10 — Classes III: Inheritance and super

**Inheritance** lets one class build on another: a subclass automatically gets the base class's
methods, and can add new ones or **override** existing ones. This models "is-a" relationships — a
`Circle` *is a* `Shape`, a `Manager` *is an* `Employee` — and removes duplication.

## A base class and a subclass

Declare a subclass by naming its base in parentheses: `class Circle(Shape):`.

```kirito
var io = import("io")

class Shape:
    var _init_ = Function(self, name):
        self.name = name
    var area = Function(self) -> Float:
        return 0.0                          # subclasses override this
    var describe = Function(self) -> String:
        return f"{self.name} with area {self.area()}"

class Circle(Shape):
    var _init_ = Function(self, radius):
        self.name = "circle"
        self.radius = radius
    var area = Function(self) -> Float:     # override
        return 3.14159 * self.radius * self.radius

var c = Circle(2)
io.print(c.area())                          # => 12.56636   (Circle's area)
io.print(c.describe())                       # => circle with area 12.56636
```

Notice `describe` is defined only on `Shape`, yet `c.describe()` works and calls **Circle's**
`area()` — method lookup starts at the actual class and walks up to the base. This is *polymorphism*:
`describe` doesn't know or care which shape it's describing.

## Inheritance is annotation-aware

A subclass instance satisfies a base-class type annotation (Lesson 7), so functions written against
the base accept any subclass:

<!--norun (continues the Shape / Circle definition earlier on this page)-->
```kirito
var io = import("io")

# (reusing Shape/Circle from above)
class Square(Shape):
    var _init_ = Function(self, side):
        self.name = "square"
        self.side = side
    var area = Function(self) -> Float:
        return Float(self.side * self.side)

var total_area = Function(shape : Shape) -> Float:   # accepts any Shape
    return shape.area()

io.print(total_area(Circle(1)))      # => 3.14159
io.print(total_area(Square(3)))      # => 9.0
io.print(isinstance(Square(3), "Shape"))   # => True
```

## Extending a parent method with `_super_`

Often a subclass wants to *extend* a base method rather than replace it wholesale — for instance, run
the base constructor and then add more. `self._super_()` returns a **parent view** of the instance:
calling a method on it starts lookup at the base class, so you invoke the inherited version.

```kirito
var io = import("io")

class Employee:
    var _init_ = Function(self, name):
        self.name = name
    var describe = Function(self) -> String:
        return f"{self.name}, employee"

class Manager(Employee):
    var _init_ = Function(self, name, reports):
        self._super_()._init_(name)         # run Employee's constructor first
        self.reports = reports              # then add Manager-specific state
    var describe = Function(self) -> String:
        var base = self._super_().describe()   # reuse Employee's description
        return f"{base}, managing {self.reports}"

var m = Manager("Grace", 5)
io.print(m.describe())               # => Grace, employee, managing 5
```

`_super_()` only works when the class actually inherits (it throws otherwise), and it climbs exactly
one level per call — so in a three-level hierarchy each class's `_super_()` reaches the next one up,
and the chain composes correctly.

## Private members and inheritance

A private member (single leading underscore) is reachable from any method in the **class chain** —
the defining class **and its subclasses** — but not from outside it. So a subclass method may use a
base class's private state, while unrelated code cannot. Keep the *public* contract in non-private methods.

## A worked example: a shape hierarchy

```kirito
var io = import("io")

class Shape2:
    var _init_ = Function(self, name):
        self.name = name
    var area = Function(self) -> Float:
        return 0.0

class Rectangle(Shape2):
    var _init_ = Function(self, width, height):
        self._super_()._init_("rectangle")
        self.width = width
        self.height = height
    var area = Function(self) -> Float:
        return Float(self.width * self.height)

class Circle2(Shape2):
    var _init_ = Function(self, radius):
        self._super_()._init_("circle")
        self.radius = radius
    var area = Function(self) -> Float:
        return 3.14159 * self.radius * self.radius

var shapes = [Rectangle(3, 4), Circle2(1), Rectangle(2, 2)]
var total = 0.0
for shape in shapes:
    io.print(f"{shape.name}: {shape.area()}")
    total = total + shape.area()
io.print(f"total area: {total}")
# => rectangle: 12.0 / circle: 3.14159 / rectangle: 4.0 / total area: 19.14159
```

**Walkthrough:** every shape shares the `name` attribute (set once, via `_super_()._init_`) and
exposes an `area()` method, but each computes area differently. The loop treats them uniformly — it
just calls `.area()` — and the right implementation runs for each. Add a `Triangle(Shape2)` tomorrow
and this loop needs no changes. That open-ended extensibility is what inheritance and polymorphism
buy you.

## Try it

Add a `Triangle(Shape2)` (`0.5 * base * height`) to the hierarchy and drop one into the `shapes`
list. Confirm the loop and total update with no other edits.

## What you learned

- `class Sub(Base):` inherits Base's methods; subclasses add or **override** them.
- Method lookup starts at the instance's class — polymorphism falls out naturally.
- Subclasses satisfy base-class annotations and `isinstance`.
- `self._super_()` invokes the inherited version of a method, climbing one level per call.
