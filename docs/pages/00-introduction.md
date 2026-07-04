# Introduction

**Kirito** is a from-scratch, dynamically-typed, strongly-typed general-purpose scripting language
with the ergonomics of a high-level scripting language and a C++ soul. Source files use the `.ki`
extension.

It is implemented in modern C++20 as a **bytecode compiler + stack VM** behind a stable AST boundary,
shipped header-only, so it both runs standalone (`ki`) and embeds into any C++ application as a
library — a single `KiritoVM` object encapsulates an entire, isolated interpreter "process".

## Why Kirito

- **High-level and fast to write**: significant indentation, first-class functions,
  rich collections, exceptions, classes — with just enough abstraction and no boilerplate.
- **Strongly typed at runtime**: separate `Integer` and `Float`, no silent coercions across
  incompatible types, and *enforced* type annotations when you want them (`Function(d : Dict) -> Float:`
  actually checks).
- **A C++ framework**: adding a new value type, function, or whole module from C++ is a few lines and
  is indistinguishable from a built-in to the evaluator.
- **Fully encapsulated**: one `KiritoVM` owns all of its state (no global mutable state), so multiple
  VMs coexist and the whole context is designed to be serializable.

## A taste

```kirito
var io = import("io")

class Greeter:
    var _init_ = Function(self, name : String):
        self.name = name
    var hello = Function(self) -> String:
        return f"Hello, {self.name}!"

var g = Greeter("world")
io.print(g.hello())                 # Hello, world!

# multiple return values + unpacking
var divmod = Function(a, b):
    return a // b, a % b
var q, r = divmod(17, 5)
io.print(f"{q} remainder {r}")      # 3 remainder 2

# comprehension-free, but expressive
var squares = map(Function(x): return x * x, range(5))
io.print(String(squares))           # [0, 1, 4, 9, 16]
```

## How to read these docs

- **Getting Started** covers install and build; the **C++ API** page covers embedding, integrating, and growing
  Kirito from C++.
- **Language Guide** is the syntax/semantics reference; **Recipes** is task-oriented.
- **Builtins** and **Standard Library** document every function, module, and method.
- **The Kirito Course** is a hands-on, lesson-by-lesson path with explained sample projects — start
  there if you'd rather learn by doing.

The docs are generated from Markdown in `docs/pages/` by `docs/build_docs.py`; add a numbered page
and it appears in the sidebar automatically.
