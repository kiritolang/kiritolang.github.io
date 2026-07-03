# Recipes

Task-oriented snippets. All assume `var io = import("io")`.

## Read a file line by line

<!--norun (reads a file that need not exist)-->
```kirito
with io.open("data.txt", "r") as f:
    for line in f:
        io.print(line.strip())
```

## Count word frequencies

```kirito
var text = "the cat sat on the mat the cat"
var counts = {}
for word in text.split(" "):
    counts[word] = counts.get(word, 0) + 1
for word, n in counts.items():
    io.print(f"{word}: {n}")
```

## Sort by a key

```kirito
var people = [["Ann", 30], ["Bo", 25], ["Cy", 41]]
var byAge = sorted(people, Function(p): return p[1])
io.print(String(byAge))         # youngest first
```

## Swap and multiple return values

```kirito
var a = 1
var b = 2
a, b = b, a

var minmax = Function(xs):
    return min(xs), max(xs)
var lo, hi = minmax([4, 1, 7, 3])
```

## Group with defaultdict

```kirito
var col = import("collections")
var groups = col.defaultdict(Function(): return [])
for n in range(10):
    groups[n % 3].append(n)
io.print(String(groups.items()))
```

## A small stack-based class

```kirito
class Stack:
    var _init_ = Function(self):
        self._items = []
    var push = Function(self, x):
        self._items.append(x)
    var pop = Function(self):
        if len(self._items) == 0:
            throw "pop from empty stack"
        return self._items.pop()
    var _len_ = Function(self) -> Integer:
        return len(self._items)

var s = Stack()
s.push(1)
s.push(2)
io.print(String(s.pop()))       # 2
io.print(String(len(s)))        # 1
```

## Make a class iterable

```kirito
class Countdown:
    var _init_ = Function(self, n):
        self._n = n
    var _iter_ = Function(self):
        var out = []
        var i = self._n
        while i > 0:
            out.append(i)
            i = i - 1
        return out

for x in Countdown(3):
    io.print(String(x))         # 3, 2, 1
```

## Typed, validated function

```kirito
var area = Function(width : Float, height : Float) -> Float:
    return width * height
# area("a", 2)  -> error: argument 'width' must be Float, got String
```

## JSON round-trip

```kirito
var json = import("json")
var data = json.parse("{\"name\": \"Kirito\", \"version\": 1}")
io.print(data["name"])
io.print(json.stringify(data))
```

## Compress and hash data

```kirito
var z = import("zlib")
var h = import("hash")
var blob = "x" * 10000
var packed = z.compress(blob)
io.print(f"{len(blob)} -> {len(packed)} bytes")
io.print(h.sha256(blob))
```

## Inspect an object at runtime

<!--norun (references `Stack` from an earlier snippet on this page)-->
```kirito
var math = import("math")
io.print(inspect(math))         # lists the module's functions
io.print(inspect(Stack))        # lists Stack's methods + signatures
```
