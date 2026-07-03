# C++ API

Everything a C++ program needs to **embed** or **extend** Kirito. Namespace: `kirito`; one umbrella
header pulls the whole (header-only) interpreter in:

```cpp
#include "kirito.hpp"
```

Compile as C++20 with `src/` on the include path (no library to link — everything is
`inline` / templated) and, on POSIX, `-pthread` (the dispatcher uses threads for `parallel`
workers). There is no `main()` in the library — the standalone interpreter's `main` lives only in
`main.cpp`. All state is owned by a `KiritoVM`; there is no global mutable state anywhere.

This page is organised **high level → low level** — a numbered outline, with cross-section jumps
where useful. You can safely stop reading at whichever layer already answers your question. The
top of the page is what you need to embed and extend; the bottom is the raw protocol reference.

---

## 1. Overview and quick start

Kirito's C++ side has two jobs:

- **Embed** the interpreter in your own program — construct a VM, run source, read the result.
- **Extend** the interpreter with your own functions, modules, and value types — everything you
  add is indistinguishable from a built-in to the evaluator.

Both go through the same objects, so both are covered on this page. The 15-line minimum:

```cpp
#include "kirito.hpp"
using namespace kirito;

int main() {
    KiritoDispatcher dispatcher;                       // recommended entry point
    KiritoVM& vm = dispatcher.mainVM();                 // fully-configured interpreter
    Handle result = vm.runSource("6 * 7\n");
    std::printf("%s\n", vm.stringify(result).c_str()); // 42
}                                                       // ~KiritoDispatcher joins workers
```

That's the whole shape: **construct → run → stringify → destruct**. Every subsequent section fills
in what happens between step 2 and step 3, and how you insert your own C++ code into the mix.

### Naming convention

Kirito-visible functions, modules, and methods are **all lowercase, no underscores** (`gettempdir`,
`startswith`, `symmetricdifference`) — match that when you add your own so a Kirito user can't tell
your extension from a built-in. C++ identifiers still follow ordinary C++ style; this rule is only
about names exposed to Kirito code.

---

## 2. Entry points

### `KiritoDispatcher` (in `dispatcher.hpp`) — the recommended way

Owns the main VM plus the machinery for worker VMs (the [`parallel`](stdlib.html#parallel) module)
and cross-VM primitives. Costs nothing until Kirito actually calls `parallel.spawn`, so use it
even for a single-threaded embed and get `parallel` for free.

```cpp
class KiritoDispatcher {
public:
    KiritoDispatcher();
    ~KiritoDispatcher();                            // calls shutdown()
    KiritoDispatcher(const KiritoDispatcher&) = delete;
    KiritoDispatcher& operator=(const KiritoDispatcher&) = delete;

    KiritoVM&        mainVM();                       // lazily constructed + configured
    void             addLibPath(const std::string& d);   // applied to main + workers
    void             setMaxCallDepth(std::size_t n);     // applied to main + workers
    void             shutdown();                          // idempotent; wakes blocked workers
    static unsigned  cpuCount();                          // hardware threads (min 1)
};
```

**Why go through the dispatcher, always.** A `KiritoVM`'s arena is *unsynchronized*, so exactly one
OS thread may ever touch a given VM. Kirito's concurrency model is therefore **multiprocessing** —
the dispatcher owns the main VM plus a pool of workers that share nothing and communicate only by
passing serialized values through thread-safe primitives. The dispatcher wires that together:

- **It configures the VM.** `mainVM()` installs the `parallel` module (a bare `KiritoVM` has
  none). Every worker VM is configured the same way, so
  `parallel.spawn`/`Queue`/`Lock`/`Event`/`Semaphore`/`Barrier` work out of the box.
- **It forwards import paths.** `addLibPath` is recorded on the dispatcher and inherited by every
  worker, so a spawned function resolves the same `import(...)` modules as the main VM.
- **Its teardown is deadlock-safe.** `~KiritoDispatcher()` (or explicit `shutdown()`) aborts every
  blocked cross-VM primitive *before* joining threads — a worker parked on a queue/lock/event can
  never stall exit. `shutdown()` is idempotent.

The `ki` CLI does exactly this; that's why `parallel` is always available when you run a script
with `ki`. The cross-VM primitives themselves are in [Section 17](#17-multiprocessing-internals).

### `KiritoVM` (in `vm.hpp`) — the interpreter you drive

You drive Kirito through the `KiritoVM&` returned by `dispatcher.mainVM()`. The most commonly used
methods, grouped by role:

| Role | Method | Purpose |
|------|--------|---------|
| **Run code** | `runSource(src, chunkName = "<main>")` | Lex+parse+eval a chunk; returns the last value. |
| | `runRepl(src)` | Like `runSource` but with a persistent module scope. |
| | `stringify(h)` | The `str()` of any value — what `print` shows. |
| **Build values** | `makeInt(int64_t)` / `makeFloat(double)` / `makeBool(bool)` / `makeString(std::string)` | Lift a C++ scalar into the arena. |
| | `none()` | The interned `None` handle. |
| | `alloc(std::unique_ptr<Object>)` | GC-aware; use for `NativeClass` types and `BytesVal`. Prefer `Value(vm, x)` and the typed wrappers `List`/`Dict`/`Set`/`String`/… ([Section 5](#5-working-with-values-the-value-api)). |
| **Register** | `registerGlobal(name, handle)` | Bind a top-level name. |
| | `install<T>()` | Install a `NativeModule` subclass ([Section 7](#7-extending-adding-a-module)). |
| | `registerSourceModule(name, src)` | Install a module whose body is Kirito source. |
| | `addLibPath(dir)` | Add an import search directory (prefer the dispatcher's version). |
| | `setArgs(argv)` | Set `arglist` for a directly-run file. |
| **Introspect** | `arena()` | The `ObjectArena` ([Section 16](#16-objectarena-in-arena-hpp)). |
| | `dispatcher()` | The owning `KiritoDispatcher*`, or `nullptr` for a bare VM. |
| | `lastTraceback()` | The frames of the most recent uncaught error. |

The full `KiritoVM` public surface — GC controls, deserializer registration, chunk-file tracking,
and the rest — is in [Section 15](#15-kiritovm-full-surface).

### A bare `KiritoVM`

If you truly need the minimum — no `parallel`, no worker threads — construct a `KiritoVM` directly
and use the same surface:

```cpp
KiritoVM vm;                                            // no `parallel` module
vm.install<StatsModule>();                              // your own modules still work
Handle r = vm.runSource("import(\"stats\").mean([2, 4, 6, 8])\n");
```

This is the lightest embed and is what the integration test
(`tools/tests/integration/embed_demo.cpp`) uses. The only thing you give up is multiprocessing:
`import("parallel")` throws, because `parallel` is a dispatcher-provided capability. Reach for a
bare VM only when you're certain you'll never want worker VMs; otherwise prefer the dispatcher.

### Isolation and multiple dispatchers

There is no global mutable state. Each dispatcher owns an independent main VM and its own worker
pool; you can run several dispatchers in one process (per-request sandboxes, say) and they never
share values, modules, or primitives. Within one dispatcher, the main VM and its workers are
isolated from each other too — they exchange only serialized blobs.

### The `parallel` embedding pattern

Building through the dispatcher makes `parallel` available, but one requirement shapes how you
embed it: **a function passed to `parallel.spawn` must be defined in a loadable `.ki` file** — a
worker VM re-reads it from disk by its source span, so its arguments and result must be
serializable. The natural pattern is to run the script *from a file* (give `runSource` the real
path) rather than from an inline string:

```cpp
// worker.ki on disk:
//   var parallel = import("parallel")
//   var square = Function(x): return x * x
//   var tasks = []
//   for i in range(8):
//       tasks.append(parallel.spawn(square, i))
//   var total = 0
//   for t in tasks:
//       total = total + t.join()
//   import("io").print(total)        # 140

KiritoDispatcher dispatcher;
KiritoVM& vm = dispatcher.mainVM();
dispatcher.addLibPath(".");                        // workers inherit the import path
std::ifstream in("worker.ki");
std::stringstream ss; ss << in.rdbuf();
vm.runSource(ss.str(), "worker.ki");                // the real path lets a worker re-read `square`
```

An inline-string program can still use every *non-spawn* feature; only `spawn` needs the function
on disk. See the [`parallel`](stdlib.html#parallel) reference and the
[Concurrency](bonus-06-concurrency.html) bonus lesson for the full model.

---

## 3. Handles and GC lifetime

**Understand this section before anything else in the extension API** — every value the C++ side
sees is a `Handle`, and Kirito's mark-sweep GC has one rule you have to follow to use them safely.

### What a `Handle` is

```cpp
struct Handle {                                     // handle.hpp
    uint32_t slot = 0;
    uint32_t generation = 0;
    bool operator==(const Handle&) const = default;
};
```

An **opaque reference** to a value living in a `KiritoVM`'s `ObjectArena`. Trivially copyable
(8 bytes on the wire); the arena owns the actual `Object`, your C++ code never sees a raw
pointer. It is **not** a pointer — you dereference through the arena
(`vm.arena().deref(h) → Object&`), and the dereference is what throws when a handle is stale.

`std::hash<kirito::Handle>` is specialised in `handle.hpp`, so a `Handle` is a valid
`std::unordered_map` / `std::unordered_set` key.

### Reference-assignment semantics

Reference-assignment in Kirito (`var b = a`) means both names point at the same value — under the
hood, both bindings hold the same `Handle`. That's why mutating a shared List / Dict / Set through
one name is visible through the other. The evaluator's whole ownership model is: **one arena
slot = one object**, and Handles are how everyone else refers to it.

### The `Handle{}` sentinel

A default-constructed `Handle{}` — `slot=0, generation=0` — is a **sentinel meaning "no
handle"**. It is used as an empty / unset marker (for example, `KiritoThrow::value` on a
`KiritoError` that carries no live handle; `ClassValue::base` when the class has no base). Do
**not** dereference it — `vm.arena().deref(Handle{})` throws with a clear message.

Test with `h == Handle{}` — or better, propagate `Handle` values whose slot could be zero as
`std::optional<Handle>` in your own code so the sentinel doesn't leak silently.

### Generation-based staleness detection

Every arena slot carries a **generation counter**. When the GC reclaims a slot, its generation is
bumped; if some other allocation later reuses the same slot, dereferencing an old handle whose
`generation` no longer matches the slot's throws
`KiritoError("dangling handle (stale generation)")`.

This is deliberate: **a stale handle is detected, not silently reinterpreted as a wrong value**.
If you see the dangling-handle error, the fix is always the same — root the handle across the
allocating call that reclaimed it (see [rooting](#the-rooting-rule) below).

### Handle equality is identity

`h1 == h2` compares `slot` and `generation`; two live Handles compare equal **iff they point at
the same arena slot** — i.e. the same underlying `Object`. This is identity, not structural
equality:

- Two independently-built Strings with the same text produce **different** Handles and compare
  `!=`. Their **values** compare equal via `Object::equals` (`vm.arena().deref(h1).equals(...)`,
  or `Value(vm, h1).str() == Value(vm, h2).str()` if that's what you meant).
- Two Handles produced from the same reference-assignment chain compare `==`.

This is what makes Handle a good `unordered_map` key when you want to look objects up by identity
(a Handle-to-metadata map, say, that must survive `==` on distinct-but-equal values).

### Interned Handles share slots

Some values are interned per VM so they share one arena slot and one Handle:

- `vm.none()` — the unit value.
- `vm.makeBool(true)` / `vm.makeBool(false)` — each is one arena slot.
- `vm.makeInt(v)` for small `v` in `[-128, 255]` — the shared "small integer" pool.

For interned values, identity `==` and structural `==` coincide. For anything else — a fresh
String, a fresh List — every construction site produces a new Handle, and `==` distinguishes them.

### Handle vs Value — when to use which

Use **`Handle`** when you're storing a value, passing it around, or handing it to the runtime:

- Return type of every native function (`Handle fn(KiritoVM&, std::span<const Handle>)`).
- Argument to `registerGlobal`, `alloc` return, `ModuleBuilder::value`, `DictVal::set`,
  `ListVal::elems.push_back`, and similar sink APIs.
- Storing values on your own C++ side (map keys, member fields) — a Handle is 8 bytes and
  trivially copyable.

Use **`Value`** ([Section 5](#5-working-with-values-the-value-api)) when you want to READ a value
or build one ergonomically:

- Inside a native impl, wrap incoming Handles in `Args a(vm, raw, "myFn"); a.at(0).asInt(...)`.
- Anywhere you'd otherwise write `static_cast<IntVal&>(vm.arena().deref(h)).value()`.

`Value` converts implicitly to and from `Handle`, so you can freely mix — build with `Value(vm, x)`,
store the resulting `Value` where a `Handle` is expected, wrap a stored `Handle` in
`Value(vm, h)` when you need to read.

### The rooting rule

The one thing you have to remember about GC lifetime:

> **A Handle held in a C++ local is NOT itself a GC root.** If you keep one across a later call
> that can allocate — `runSource`, `evalIn`, `alloc`, `collectGarbage`, sometimes even
> `makeString` — the value the handle points at may be reclaimed, and the next dereference throws
> the dangling-handle error.

Root the handle for the window you need it. `RootScope` is the standard idiom:

```cpp
struct RootScope {                              // vm.hpp
    explicit RootScope(KiritoVM& v);
    ~RootScope();                                // pops every handle added
    Handle add(Handle h);                        // add & return, so it fits inline
};

// Use:
RootScope rs(vm);
Handle preserved = rs.add(vm.runSource("build()"));   // survives the next line's allocations
vm.runSource("do_more()");
vm.registerGlobal("prev", preserved);                  // still live
```

Values you read and immediately return (as inside a native impl) need no rooting — the runtime
already keeps them alive under the call frame's temp-root region. Root only when your handle
outlives the current expression.

**Alternatives to `RootScope`:**

- **`registerGlobal(name, h)`** roots via the module scope for as long as the binding survives.
- **`vm.pushTemp(h)`** / **`vm.popTempTo(mark)`** — the raw primitive `RootScope` wraps. Prefer
  `RootScope` unless you're implementing something like `RootScope` yourself.
- **The `List`/`Dict`/`Set`/`String`/… wrappers** pin their own allocation for their lifetime (a
  refcounted GC root via `vm.pinHandle` / `unpinHandle`), so you almost never touch the rooting
  primitives directly — building through the wrappers is self-rooting. See [Section 5 → Constructing
  values](#constructing-values).

You need the manual primitives above only when you hold a bare `Handle` — with no wrapper around it —
across another allocating call.

### `NamedArg` — a keyword argument

```cpp
struct NamedArg { std::string name; Handle value; };
```

The transport for a keyword argument (`f(name = value)`) at every call boundary — class
instantiation, `_init_` and `_call_` on an instance, keyword-aware natives, worker-VM argument
packing. If you write a signatured native ([Section 6](#6-extending-adding-a-function)),
`NativeFunction::bindArgs` does the positional↔named binding for you; if you write a variadic
kwarg-aware native, your impl receives `std::span<const NamedArg>` alongside the positional span.

### Never expose raw pointers

Do not let a raw `Object*` cross the C++/Kirito boundary in either direction. Always hold
`Handle`s and deref at the point of use. Two reasons:

1. **GC can move ownership under you.** A slot's `unique_ptr` may be reset on the next
   `collectGarbage()`; a raw pointer becomes a use-after-free with none of the generation guard's
   protection.
2. **Cross-object aliasing must go through the arena.** Two Kirito bindings sharing a value share
   a Handle; a raw pointer bypasses that model and makes the shape-of-shared-state harder to
   reason about.

---

## 4. Errors

Two exception types cross the C++/Kirito boundary. Both derive from `std::exception`, so an
embedder can catch either with a single `catch (const std::exception&)` when that's all the detail
you need.

```
std::exception
  └── KiritoThrow          (Handle value; the raw Kirito `throw <value>`)
        └── KiritoError    (adds std::string message; the C++-side diagnostic)
```

Because `KiritoError` **is-a** `KiritoThrow`, list `catch (KiritoError&)` **before**
`catch (KiritoThrow&)` at any site that treats them differently.

```cpp
class KiritoThrow : public std::exception {    // exceptions.hpp
public:
    KiritoThrow(Handle v, SourceSpan s = {});
    Handle value{};                             // the value the user threw
    SourceSpan span{};                          // throw / assert site
    std::string file;                           // defining chunk of the throwing function
    std::vector<TraceFrame> traceback;          // filled as the throw unwinds
    const char* what() const noexcept override; // returns "Kirito value thrown"
};

class KiritoError : public KiritoThrow {       // exceptions.hpp
public:
    explicit KiritoError(std::string message, SourceSpan sp = {});
    const char* what() const noexcept override; // returns the message
};
```

- **`KiritoError`** is what the C++ side throws for parse errors, type mismatches, div-by-zero,
  out-of-range indices, signature-check failures, and bad native-function arguments.
- **`KiritoThrow`** is the raw Kirito `throw <value>` unwind — its `.what()` is a stable generic
  label because at throw time no VM pointer is available to stringify the handle. Render the
  payload with `vm.stringify(t.value)`.
- **`runSource` promotes** a top-level uncaught Kirito `throw` into a `KiritoError` with message
  `"uncaught exception: <str(value)>"`, so an embedder that only ever runs whole scripts sees only
  `KiritoError`.

### The single-arm minimum

```cpp
try {
    vm.runSource(source, "<embedded>");
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
}
```

### The full pattern — separate arms for detail

```cpp
try {
    engine.runUserQuery(...);
} catch (const KiritoError& e) {                       // parse / type / native errors
    std::fprintf(stderr, "%u:%u: %s\n", e.span.line, e.span.col, e.what());
} catch (const KiritoThrow& t) {                        // Kirito `throw <value>` mid-call
    std::fprintf(stderr, "user throw: %s\n", vm.stringify(t.value).c_str());
} catch (const std::exception& e) {                     // any other native failure
    std::fprintf(stderr, "%s\n", e.what());
}
```

### Kirito catching your `std::exception`

The reverse direction Just Works: a Kirito `try/catch` catches any `std::exception` a native
throws. The runtime promotes it into a Kirito `String` (via `.what()`) at the catch site, so:

<!--norun (illustrative fragment; `...` is a placeholder, not valid Kirito syntax)-->
```kirito
try:
    var page = net.get(...)         # if the native throws std::runtime_error,
catch String as e:                    # Kirito sees the .what() text as a String
    io.print("failed: " + e)
```

To route on a value type instead, throw a Kirito `throw` (propagates as `KiritoThrow` and preserves
the value's real type).

---

## 5. Working with values — the `Value` API

The ergonomic layer, and where most extension code lives. Every Kirito builtin has a matching C++
wrapper — `Bool`, `Integer`, `Float`, `String`, `Bytes`, `List`, `Dict`, `Set` — all derived from a
polymorphic `Value` base. Two habits carry the whole API:

- **Peek then wrap**, zero-alloc: `arg.isDict()` then `Dict d = arg.asDict()` gives you a thin
  `(vm, handle)` view over the same object — no copy of the data.
- **Construct through constructors**, no dangling helpers: `Value(vm, 42)`, `String(vm, "hi")`,
  `List(vm, {1, "a", 3.14})`, `Dict(vm, {{"k", 1}, {"n", "s"}})`, `Set(vm, {1, 2, 3})`,
  `Value::None(vm)`.

Every wrapper converts implicitly to `Handle` (via `operator Handle()`), so this API interoperates
with the raw protocol without conversions. This section is itself high → low: the [`Value`
base](#the-value-base) is the one surface every value shares; the [type
reference](#type-reference) is the per-builtin extras; [`Args`](#args-reading-a-native-function-s-positional-arguments)
and the [walkthrough](#end-to-end-walkthrough) are how it comes together in a native function.

### When to hold a Kirito value in C++

The built-in Kirito types — the collections `ListVal` / `DictVal` / `SetVal`, the sequences `StrVal`
/ `BytesVal`, and the scalars `IntVal` / `FloatVal` / `BoolVal` / `NoneVal` — are not just what
Kirito code sees; each is a perfectly good general-purpose value for any C++ code that lives
alongside the interpreter. Reach for them (through the `Value` / `List` / `Dict` / `Set` API) when:

- **The data will cross the boundary later.** Storing it as a Kirito value from the start means no
  `std::…` → Kirito conversion when you cross.
- **You want structural equality, hashing, and stringification for free.** Every container's
  `equals`, `hash`, and `str` walks the tree correctly (cycle-guarded, depth-bounded), so nested
  Dicts compare and print with no extra code. Scalars follow suit.
- **You want the code-point-aware String.** `String` holds UTF-8 but `s[i]` indexes by **code
  point** — the right unit for anything user-facing. `std::string` gives you bytes, which
  mishandles every non-ASCII character. `.size()` counts code points; raw UTF-8 bytes still
  available via `.utf8()` / `.asStringRef()`; free UTF-8 helpers in `builtins.hpp`
  (`utf8Length` / `utf8Starts` / `utf8DecodeAt` / `utf8Encode` / `utf8ToUpperCp` / `utf8ToLowerCp`)
  let you do the same code-point work on any `std::string`.
- **`Bytes`** is the byte-exact counterpart to `String` — the right type for binary I/O.
- **Scalars are interned where it counts.** `None`, `True`/`False`, and small Integers in
  `[-128, 255]` share one arena slot per VM.
- **GC-managed ownership.** No `unique_ptr` / `shared_ptr` bookkeeping.

Fall back to `std::vector` / `std::unordered_map` / `std::string` / raw scalars only when the
value stays purely on the C++ side.

### The `Value` base

`Value` is the polymorphic base of every wrapper, so **everything below works on a `Bool`, a
`String`, a `List`, a `Dict` — any of them.** Ask "what kind am I?", read a scalar out, peek-and-wrap
into a typed view, apply an operator, hash it, call it, get/set an attribute. The [per-type
extras](#type-reference) are layered on top of exactly this.

```cpp
class Value {                                  // value.hpp
public:
    Value();                                    // empty view (unusable — for slot init)
    Value(KiritoVM& vm, Handle h);              // wrap an existing arena handle

    // conversions
    operator Handle() const;
    Handle       handle() const;
    KiritoVM&    vm() const;

    // introspection
    ValueKind    kind() const;
    std::string  typeName() const;
    bool         truthy() const;
    std::string  str() const;                    // via vm.stringify

    // type tests
    bool isNone() const;    bool isBool() const;     bool isInt() const;
    bool isFloat() const;   bool isNumber() const;   bool isString() const;
    bool isBytes() const;   bool isList() const;     bool isDict() const;   bool isSet() const;

    // scalar reads — `who` names the caller in the error message
    int64_t             asInt      (const char* who = "value") const;
    double              asFloat    (const char* who = "value") const;   // Integer or Float
    bool                asBool     (const char* who = "value") const;
    const std::string&  asStringRef(const char* who = "value") const;   // raw UTF-8 bytes

    // peek + wrap into a typed view — throws on wrong kind; the tryX form returns std::nullopt
    Bool    asBoolV  (const char* who = "value") const;   std::optional<Bool>    tryBoolV()   const;
    Integer asInteger(const char* who = "value") const;   std::optional<Integer> tryInteger()const;
    Float   asFloatV (const char* who = "value") const;   std::optional<Float>   tryFloatV() const;
    String  asString (const char* who = "value") const;   std::optional<String>  tryString() const;
    Bytes   asBytes  (const char* who = "value") const;   std::optional<Bytes>   tryBytes()  const;
    List    asList   (const char* who = "value") const;   std::optional<List>    tryList()   const;
    Dict    asDict   (const char* who = "value") const;   std::optional<Dict>    tryDict()   const;
    Set     asSet    (const char* who = "value") const;   std::optional<Set>     trySet()    const;

    // universal reads
    std::size_t         len() const;                                 // any type with a length
    std::vector<Value>  items() const;                               // iterate any iterable
    bool                equals(const Value& other) const;            // structural equals

    // operators — delegate to applyBinaryOp / applyUnaryOp so semantics match Kirito EXACTLY
    // (wraparound on Integer overflow, true-division /, exact IEEE-754 ==, per-type dispatch).
    Value  operator+ (const Value&) const;   Value  operator- (const Value&) const;
    Value  operator* (const Value&) const;   Value  operator/ (const Value&) const;
    Value  operator% (const Value&) const;   Value  operator- ()             const;   // unary neg
    Value  floordiv  (const Value&) const;   Value  pow       (const Value&) const;   // `//` and `**`

    bool   operator==(const Value&) const;   bool   operator!=(const Value&) const;
    bool   operator< (const Value&) const;   bool   operator<=(const Value&) const;
    bool   operator> (const Value&) const;   bool   operator>=(const Value&) const;

    explicit operator bool()        const;   bool   operator!()             const;   // truthiness
    bool   contains  (const Value&) const;                            // the `in` operator
    std::size_t hash() const;                                         // throws on unhashable

    // callable & attribute access — through the object protocol
    Value  call    (std::span<const Handle> args) const;
    Value  call    (std::initializer_list<detail::Anything> args) const;  // call({1, "x", …})
    Value  getAttr (std::string_view name) const;
    void   setAttr (std::string_view name, const Value& v) const;
};
```

**Operators match Kirito bit-for-bit.** `a + b`, `a < b`, `a == b` in C++ route through the same
`applyBinaryOp` / `applyUnaryOp` the bytecode VM uses, so you inherit Integer wraparound, true-division
`/`, exact IEEE-754 `==`, Set algebra on `-`/`<`/`<=`, list concatenation on `+`, and every user
`_add_` / `_lt_` override — no separate code path to keep in sync. `//` and `**` have no C++ operator,
so they are the named members `floordiv` / `pow`. `explicit operator bool()` means `if (v)` and `!v`
work (mirroring Kirito's `if v:` / `not v`) but a `Value` never *implicitly* decays to `bool` in
arithmetic.

**`.items()` gotcha.** On a **String**, `.items()` iterates code point by code point (each element
is a single-character `Value`); on a **Dict**, it yields the **keys**, not the pairs. If your
native expects a *List*, always check `.isList()` before iterating — a Kirito caller passing a bare
String otherwise silently unpacks into its characters. The router integration test
(`tools/tests/integration/embed_router.cpp`) had to add exactly one line to guard against this:

```cpp
if (!r.isList()) throw KiritoError("rule must return a List, got '" + r.typeName() + "'");
```

### Constructing values

Three routes, no free functions:

```cpp
// 1. Primitives — Value(vm, x) picks the Kirito type from the C++ type.
Value  n   (vm, 42);              // Integer      (also long / unsigned / … )
Value  pi  (vm, 3.14159);         // Float
Value  yes (vm, true);            // Bool
Value  name(vm, "Ada");           // String       (const char* / std::string / std::string_view)
Value  nil = Value::None(vm);     // None          (Value(vm, nullptr) also works)

// 2. Typed constructors — when you want the typed view straight away.
Integer i(vm, 42);   Float f(vm, 2.5);   Bool b(vm, true);   String s(vm, "hi");   Bytes raw(vm, blob);

// 3. Collections — from an initializer list mixing any C++ primitive or existing Value.
List nums(vm, {1, 2, 3});                             // List of Integer
Set  tags(vm, {"blue", "fast"});                      // Set of String
Dict cfg (vm, {{"name", "Ada"}, {"nums", nums}});     // Dict {String: Value}
```

The initializer list can't see the container's VM, so each element is a `detail::Anything` — a tiny
deferred holder that materialises into a `Handle` (via `vm.makeInt` / `makeString` / … or straight
through for an existing `Value`/`Handle`) inside the constructor, which does have the VM. That is why
`{1, "a", 3.14, someValue}` type-checks with mixed element types.

**Fresh-alloc constructors GC-pin; wraps don't.** Any constructor that ALLOCATES a new object
(`List(vm)`, `Dict(vm, {…})`, `String(vm, "hi")`, `Integer(vm, 42)`, …) pins its handle for the
wrapper's lifetime, so a GC triggered by a *later* allocation in the same expression can't sweep the
half-built value. The pin is a `shared_ptr`, so copying the wrapper shares it and the last copy
releases it. Constructors that merely WRAP an existing handle (`Dict(vm, someHandle)`, `arg.asDict()`)
don't pin — the object is already rooted wherever it came from (an argument, a Dict field, a global).
Consequence: you can freely return a `List`/`Dict`/`String` from a function, store it in a
`std::vector`, or keep it across another `runSource` — the pin travels with it.

### Type reference

Each wrapper **is-a `Value`**, so it carries the entire surface above; the declarations below show only
what each type *adds*. All are zero-cost `(KiritoVM&, Handle)` views — copying is O(1) and never
allocates, and a wrap constructor throws `<T> expected …, got '<actual>'` on a kind mismatch.

#### `Bool`

Kirito's boolean; implicitly decays to C++ `bool` for painless conditionals.

```cpp
class Bool : public Value {
    explicit Bool(KiritoVM& vm, bool v);
    Bool(KiritoVM& vm, Handle h);          // wrap; throws if not a Bool
    bool value() const;
    operator bool() const;                 // implicit -> bool
};
```
```cpp
Bool b = arg.asBoolV();
if (b) …                                   // uses operator bool()
```

#### `Integer`

64-bit signed. Carries `.compare` — the same tolerance predicate as Kirito's `n.compare(m, …)` —
because `==` on numbers is exact.

```cpp
class Integer : public Value {
    explicit Integer(KiritoVM& vm, int64_t v);   // also int / long / unsigned variants
    Integer(KiritoVM& vm, Handle h);              // wrap; throws if not an Integer
    int64_t value() const;
    operator int64_t() const;                     // implicit -> int64_t
    bool compare(const Value& other, double rel_tol = 1e-9, double abs_tol = 0.0) const;
};
```
```cpp
Integer n = arg.asInteger("count");
int64_t x = n;                             // implicit
Integer sum = (n + Value(vm, 1)).asInteger();   // operators live on the base
```

#### `Float`

IEEE-754 double, with the same tolerant `.compare` as `Integer`.

```cpp
class Float : public Value {
    explicit Float(KiritoVM& vm, double v);       // also float
    Float(KiritoVM& vm, Handle h);
    double value() const;
    operator double() const;
    bool compare(const Value& other, double rel_tol = 1e-9, double abs_tol = 0.0) const;
};
```
```cpp
Float f = arg.asFloatV();
if (f.compare(Value(vm, 0.1) + Value(vm, 0.2), 1e-9)) …   // tolerant; f == 0.3 would be exact
```

#### `String`

Unicode text. Indexes and counts by **code point**; the raw UTF-8 is still one call away.

```cpp
class String : public Value {
    explicit String(KiritoVM& vm, std::string_view utf8);   // also const char* / std::string
    String(KiritoVM& vm, Handle h);
    const std::string& utf8() const;              // raw bytes — zero-copy (alias: value())
    std::size_t        size() const;              // code-point count (Kirito's len(s))
    bool               empty() const;
    String  operator[](std::ptrdiff_t i) const;   // 1-code-point String; negatives from the end
    bool    contains(std::string_view sub) const;
    bool    startsWith(std::string_view p) const;
    bool    endsWith(std::string_view p) const;
    String  operator+(const String& rhs) const;   // concat (fast path; base + also works)
    bool    operator==(std::string_view rhs) const;   // compare to a literal
    operator std::string_view() const;                // implicit -> string_view
};
```
```cpp
String s(vm, "café");
s.size();          // 4 code points  (s.utf8().size() is 5 bytes)
s[-1].utf8();      // "é"
s.startsWith("ca");
```

#### `Bytes`

The byte-exact counterpart to `String` — for binary I/O. `b[i]` is the byte as an `int` (0–255).

```cpp
class Bytes : public Value {                      // methods defined in bytes.hpp
    explicit Bytes(KiritoVM& vm, std::string_view raw);   // also std::string
    Bytes(KiritoVM& vm, Handle h);
    const std::string& data() const;              // raw bytes — zero-copy
    std::size_t        size() const;
    bool               empty() const;
    int operator[](std::ptrdiff_t i) const;       // 0..255; negatives from the end
};
```
```cpp
Bytes b = resp.getAttr("content").asBytes();      // net.get(...).content is Bytes
int first = b[0];
```

#### `List`

Ordered, mutable sequence. `xs[i]` reads (negatives from the end), `push`/`pop` mutate, range-for
iterates.

```cpp
class List : public Value {
    explicit List(KiritoVM& vm);                              // fresh empty
    List(KiritoVM& vm, std::initializer_list<detail::Anything> items);
    List(KiritoVM& vm, const std::vector<Handle>& handles);   // bulk
    List(KiritoVM& vm, Handle h);                             // wrap

    std::size_t size() const;   bool empty() const;
    Value  operator[](std::ptrdiff_t i) const;    // negatives from the end; throws out of range
    void   set (std::ptrdiff_t i, const Value& v);
    List&  push(const Value& v);                  // chainable append (also push(handle) / push(x))
    Value  pop();                                 // throws on empty
    bool   contains(const Value& v) const;        // by value-protocol equality
    void   clear();
    // range-for: for (Value e : xs) …
};
```
```cpp
List xs(vm, {10, 20, 30});
xs.push(40);
xs[-1].asInt();                    // 40
for (Value e : xs) total += e.asInt();
```

#### `Dict`

Hash map. `d[k]` reads (throws if absent), `d.get(k, dflt)` substitutes, `d.set(k, v)` writes
chainably, `d.contains(k)` peeks, range-for yields `(key, value)` pairs.

```cpp
class Dict : public Value {
    explicit Dict(KiritoVM& vm);                              // fresh empty
    Dict(KiritoVM& vm, std::initializer_list<
             std::pair<detail::Anything, detail::Anything>> entries);
    Dict(KiritoVM& vm, Handle h);                             // wrap

    std::size_t size() const;   bool empty() const;
    Value  operator[](const Value& k)      const; // read; throws if absent (also (std::string_view))
    Value  get   (const Value& k, Value dflt) const;          // also (std::string_view, dflt)
    std::optional<Value> tryGet(const Value& k) const;        // also (std::string_view)
    Dict&  set   (const Value& k, const Value& v);            // chainable; also set(k, x) templated
    bool   contains(const Value& k) const;                    // also (std::string_view)
    bool   has   (std::string_view sk) const;                 // Kirito-style alias for contains
    bool   remove(const Value& k);                            // true if it was present
    void   clear();
    std::vector<Value> keys()   const;
    std::vector<Value> values() const;
    std::vector<std::pair<Value, Value>> pairs() const;
    // range-for: for (auto [k, v] : d) …
};
```
```cpp
Dict d(vm, {{"x", 1}, {"y", 2}});
d["x"].asInt();                        // 1
d.get("z", Value(vm, 0)).asInt();      // 0  — default on miss
d.set("z", 3).set("w", 4);             // chainable
for (auto [k, v] : d) …                // (key, value) pairs
```

#### `Set`

Hash set of unique values. `add` inserts, `contains` peeks, `discard` removes silently.

```cpp
class Set : public Value {
    explicit Set(KiritoVM& vm);                               // fresh empty
    Set(KiritoVM& vm, std::initializer_list<detail::Anything> items);
    Set(KiritoVM& vm, Handle h);                              // wrap

    std::size_t size() const;   bool empty() const;
    Set&   add(const Value& v);                   // chainable insert (also add(x) templated)
    bool   contains(const Value& v) const;
    void   discard(const Value& v);               // silent if absent
    void   clear();
    std::vector<Value> items() const;
    // range-for: for (Value e : s) …
};
```
```cpp
Set s(vm, {1, 2, 3});
s.add(2);                              // no-op — already present
s.contains(2);                         // true
Set diff = (s - Value(vm, other)).asSet();   // set algebra via the base `-` operator
```

### `Args` — reading a native function's positional arguments

```cpp
class Args {                                     // value.hpp
public:
    Args(KiritoVM& vm, std::span<const Handle> a, const char* fn = "function");
    std::size_t size() const;
    bool empty() const;
    Value operator[](std::size_t i) const;       // unchecked
    Value at(std::size_t i) const;               // throws "fn missing argument N"
    Value opt(std::size_t i, Value dflt) const;  // substitutes a default
    void  require(std::size_t n) const;          // throws "fn expects N arguments, got M"
    std::span<const Handle> raw() const;         // pass-through to the low-layer protocol
};
```

`at(i)` bounds-checks; `opt(i, dflt)` substitutes when absent. The `fn` label is spliced into the
error message so a bad argument reports the function name (`"demo missing argument 1"`,
`"x expected Integer, got 'Float'"`).

### End-to-end walkthrough

Build every collection from C++, hand it to Kirito, read the mutated result back — one line of
construction per type, and the read side mirrors Kirito's own methods:

```cpp
#include "kirito.hpp"
using namespace kirito;

KiritoVM vm;

// 1. Build every collection type from C++ — one line each -----------------
List nums(vm, {1, 2, 3});                             // List of Integer
Set  tags(vm, {"blue", "fast"});                      // Set of String
Dict cfg(vm, {                                        // Dict {String: Value}
    {"name", "Ada"},
    {"nums", nums},
    {"tags", tags},
});

// 2. Hand it to Kirito ----------------------------------------------------
vm.registerGlobal("cfg", cfg);

Handle out = vm.runSource(
    "cfg[\"nums\"].append(4)\n"
    "cfg[\"tags\"].add(\"hot\")\n"
    "cfg[\"length\"] = len(cfg[\"nums\"])\n"
    "cfg\n");

// 3. Read the result back -------------------------------------------------
Dict res(vm, out);
std::printf("name   = %s\n", res["name"].asString().utf8().c_str());
std::printf("length = %lld\n", (long long)res["length"].asInt());
std::printf("nums   = ");
for (Value n : res["nums"].asList()) std::printf("%lld ", (long long)n.asInt());
std::printf("\ntags   = ");
for (Value t : res["tags"].asSet()) std::printf("'%s' ", t.asString().utf8().c_str());
std::printf("\n");
```

Output:

```
name   = Ada
length = 4
nums   = 1 2 3 4
tags   = 'blue' 'fast' 'hot'
```

Patterns worth remembering:

- **A List/Dict/Set is held by reference across the boundary.** After `registerGlobal("cfg", cfg)`,
  Kirito's `cfg["nums"].append(4)` mutates the same List your C++ code built — you can read the
  appended element straight back through the local `nums` wrapper, no round-trip through `runSource`.
  Immutable scalars are also handles, but every mutation produces a *new* handle.
- **Bytes vs code points on a String.** `s[i]` is a 1-code-point String (Kirito semantics),
  `s.size()` is the code-point count, `s.utf8()` (or `.asStringRef()` on the base `Value`) is the raw
  UTF-8 buffer for byte-oriented I/O:

  ```cpp
  String s(vm, "café日");
  s.utf8().size();   // 9  — UTF-8 bytes
  s.size();          // 5  — code points
  for (std::size_t i = 0; i < s.size(); ++i) use(s[i]);   // c, a, f, é, 日
  utf8Length("naïve");   // 5 (not 6) — the free helper works on any std::string
  ```
- **Rooting is automatic for wrappers.** A fresh-alloc wrapper pins itself; a wrap of a live handle
  needs no rooting. Only when you hold a bare `Handle` (no wrapper) across another allocating call do
  you root it yourself: `RootScope rs(vm); rs.add(h);`.

Every bundled stdlib module — `io`, `math`, `random`, `matrix`, `complex`, `json`, `serialize`,
`dump`, `net`, `sys`, `time`, `zlib`, `hash` — is authored against this `Value` API, so those
headers double as worked examples of idiomatic native code.

---

## 6. Extending: adding a function

The lightest extension. A `NativeFunction` wraps a
`std::function<Handle(KiritoVM&, std::span<const Handle>)>`. Reach through `Args` + `Value` so a
bad argument becomes a clear `KiritoError` instead of a cast crash:

```cpp
vm.registerGlobal("clamp", vm.alloc(std::make_unique<NativeFunction>(
    "clamp",
    std::vector<NativeParam>{{"x", "Integer"}, {"lo", "Integer"}, {"hi", "Integer"}},
    "Integer",
    [](KiritoVM& vm, std::span<const Handle> raw) -> Handle {
        Args a(vm, raw, "clamp");                       // arity + labelled errors
        int64_t x  = a.at(0).asInt("x");
        int64_t lo = a.at(1).asInt("lo");
        int64_t hi = a.at(2).asInt("hi");
        return Value(vm, std::max(lo, std::min(x, hi)));
    })));
// Kirito:  clamp(12, lo=0, hi=9)   ->   9
```

### Declaring a signature — keyword arguments + `inspect`

The signatured constructor overload gives your function exactly what Kirito functions have:
**keyword arguments**, **defaults**, and an accurate `inspect()`. Construct `NativeParam`s as
`{"name"}`, `{"name", "Type"}`, or `{"name", "Type", defaultHandle}`:

```cpp
vm.registerGlobal("greet", vm.alloc(std::make_unique<NativeFunction>(
    "greet",
    std::vector<NativeParam>{{"name", "String"}, {"loud", "Bool", vm.makeBool(false)}},
    "String",
    [](KiritoVM& vm, std::span<const Handle> raw) -> Handle {
        Args a(vm, raw, "greet");
        std::string msg = "Hello, " + a.at(0).asString("name");
        return Value(vm, a.at(1).asBool("loud") ? msg + "!" : msg);
    })));
// Kirito:  greet("Ada")  /  greet(name="Ada", loud=True)  /  inspect(greet)
```

The impl still receives a flat positional `span` — the runtime binds keywords and fills defaults
before calling it. Inside a module's `setup`, the `ModuleBuilder` has the same overload; see
[Section 7](#7-extending-adding-a-module).

### The three `NativeFunction` shapes

```cpp
class NativeFunction : public Object {           // function.hpp
public:
    // 1) Positional-only: no keyword args, `...` under inspect.
    NativeFunction(std::string name, NativeFn fn, std::vector<Handle> captures = {});

    // 2) Signatured: keyword args + defaults; `inspect` shows the signature.
    NativeFunction(std::string name, std::vector<NativeParam> sig, std::string returnType,
                   NativeFn fn, std::vector<Handle> captures = {});

    // 3) Variadic AND keyword-aware (impl receives named args itself).
    NativeFunction(std::string name, NativeFnKw kwfn, std::vector<Handle> captures = {});
};
```

- **Shape 1** — the terse version. No signature, no keyword arguments; suitable for a `NativeFn`
  where you want minimum ceremony.
- **Shape 2** — one `NativeParam` per positional slot with an optional type annotation and
  default. Use this for anything a Kirito user might call with keyword arguments.
- **Shape 3** — for the few builtins that are both variadic **and** accept named options
  (`io.print(..., stream=f)`).

`captures` is an auxiliary root vector so a bound-method closure's captured handles stay GC-alive
for the function's lifetime — the mechanism `makeMethod` uses to bind `self` (see
[Section 8](#8-extending-adding-a-value-type)).

Full class details (`bindArgs`, the read-only surface, `NativeFn`/`NativeFnKw` typedefs,
`NativeParam` struct) are in [Section 14](#14-functions-native).

---

## 7. Extending: adding a module

Subclass `NativeModule`, override `name()` and `setup()`, register with one `install<T>()`. Inside
`setup`, declare members through the `ModuleBuilder` — `fn` for functions, `value` for constants,
`alias` for a second public name. This is a complete `stats` module — it is also the embedding
integration test (`tools/tests/integration/embed_demo.cpp`), so it is guaranteed to compile and
run:

```cpp
struct StatsModule : NativeModule {
    std::string name() const override { return "stats"; }

    void setup(ModuleBuilder& m) override {
        // mean(xs: List) -> Float — iterate any iterable, read each element as a number.
        m.fn("mean", {{"xs", "List"}}, "Float",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "mean");
                 double sum = 0; int64_t n = 0;
                 for (Value x : args.at(0).items()) { sum += x.asFloat("mean element"); ++n; }
                 if (n == 0) throw KiritoError("mean of empty list");
                 return Value(vm, sum / static_cast<double>(n));
             });

        // clamp(x, lo, hi) -> Integer — signatured, so it accepts keyword arguments and defaults.
        m.fn("clamp", {{"x", "Integer"}, {"lo", "Integer"}, {"hi", "Integer"}}, "Integer",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                 Args args(vm, a, "clamp");
                 int64_t x = args.at(0).asInt("x"), lo = args.at(1).asInt("lo"), hi = args.at(2).asInt("hi");
                 return Value(vm, std::max(lo, std::min(x, hi)));
             });

        m.value("VERSION", Value(m.vm(), "1.0"));   // a plain constant member
    }
};

vm.install<StatsModule>();   // register once; then import("stats") works from Kirito
```

<!--norun (uses the stats module defined in C++ above)-->
```kirito
var stats = import("stats")
stats.mean([2, 4, 6, 8])      # 5.0
stats.clamp(12, lo=0, hi=9)   # 9   — keyword args come straight from the signature
stats.VERSION                 # "1.0"
inspect(stats)                # lists mean/clamp with their parameter types and returns
```

A third party adds a module exactly the way the bundled stdlib does: `#include` the header, call
`install<T>()`, no global state. `ModuleBuilder`'s full surface:

```cpp
class ModuleBuilder {                            // native.hpp
public:
    ModuleBuilder(KiritoVM& vm, Handle module, ModuleValue& mod);
    Handle    moduleHandle() const;              // for capturing into a member's closure
    KiritoVM& vm();

    ModuleBuilder& fn   (std::string name, NativeFn impl);                       // positional-only
    ModuleBuilder& fn   (std::string name, std::vector<NativeParam> sig,         // signatured
                         std::string returnType, NativeFn impl);
    ModuleBuilder& kwfn (std::string name, NativeFnKw impl);                     // variadic + kwargs
    ModuleBuilder& value(const std::string& name, Handle h);
    ModuleBuilder& alias(const std::string& name, const std::string& existing);  // second public name
};
```

### Freezing a Kirito-source module

If your module is pure logic (no new C++ primitives), it is often cleaner to write it in Kirito
and freeze the source into the binary:

```cpp
inline constexpr std::string_view mymod = R"KI(
var double = Function(x):
    return x * 2
)KI";

vm.registerSourceModule("mymod", mymod);
```

The source is compiled once per VM on first `import("mymod")`; its top-level `var`s become the
module's members (names starting with `_` stay private). The bundled `itertools`, `collections`,
`statistics`, `tabular`, `xml`, and others are built this way — see `stdlib_kimodules.hpp`.

---

## 8. Extending: adding a value type

When returning built-in values isn't enough — you need a value with its own identity, methods, and
operators — subclass **`NativeClass<Derived>`**. The CRTP base fills in `kind` / `typeName` /
`truthy` / `equals`; you define `static constexpr const char* kTypeName` and override only the
protocol slots your type uses.

Here is a complete 2-D vector — attributes, methods, an overloaded `+`, and a custom `str`
(verified in `tools/tests/integration/embed_demo.cpp`):

```cpp
struct Vec2 : NativeClass<Vec2> {
    static constexpr const char* kTypeName = "Vec2";
    double x, y;
    Vec2(double x, double y) : x(x), y(y) {}

    std::string str(StringifyCtx&) const override {
        return "Vec2(" + std::to_string(x) + ", " + std::to_string(y) + ")";
    }

    // Attribute reads return values directly; a method name returns a callable with `self` bound,
    // so v.length() / v.dot(o) arrive with the receiver already in hand.
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "x") return Value(vm, x);
        if (name == "y") return Value(vm, y);
        if (name == "length")
            return makeMethod(vm, "length", {},                            // no params
                [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
                    auto& v = static_cast<Vec2&>(vm.arena().deref(self));
                    return Value(vm, std::sqrt(v.x * v.x + v.y * v.y));
                }, std::vector<Handle>{self});                              // bind self
        if (name == "dot")
            return makeMethod(vm, "dot", {"other"},                        // v.dot(o) or v.dot(other=o)
                [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                    Args args(vm, a, "dot");
                    auto& v = static_cast<Vec2&>(vm.arena().deref(self));
                    auto* o = dynamic_cast<const Vec2*>(&vm.arena().deref(args.at(0)));
                    if (!o) throw KiritoError("dot expects a Vec2");
                    return Value(vm, v.x * o->x + v.y * o->y);
                }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);                             // unknown -> clear error
    }

    // Operator overloading: Vec2 + Vec2 -> Vec2; anything else falls back to the default error.
    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override {
        if (op == BinOp::Add)
            if (auto* o = dynamic_cast<const Vec2*>(&vm.arena().deref(rhs)))
                return vm.alloc(std::make_unique<Vec2>(x + o->x, y + o->y));
        return Object::binary(vm, op, self, rhs);
    }
};

// A constructor name so `Vec2(x, y)` builds one from Kirito (signatured -> kwargs + inspect).
vm.registerGlobal("Vec2", vm.alloc(std::make_unique<NativeFunction>(
    "Vec2", std::vector<NativeParam>{{"x", "Number"}, {"y", "Number"}}, "Vec2",
    [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args args(vm, a, "Vec2");
        return vm.alloc(std::make_unique<Vec2>(args.at(0).asFloat("x"), args.at(1).asFloat("y")));
    })));
```

<!--norun (uses the Vec2 native type defined in C++ above)-->
```kirito
var a = Vec2(3, 4)
a.x                       # 3.0
a.length()                # 5.0
a.dot(Vec2(1, 0))         # 3.0
a.dot(other = Vec2(1, 0)) # 3.0   — methods built with makeMethod accept keyword arguments
String(a + Vec2(1, 1))    # "Vec2(4.000000, 5.000000)"
```

`vm.alloc(std::make_unique<T>(...))` boxes any `Object` (a built-in *or* a `NativeClass`) into the
arena and returns a `Handle`.

### `makeMethod` — member functions that also accept keyword arguments

```cpp
inline Handle makeMethod(KiritoVM& vm,
                         std::string name,
                         std::vector<std::string> params,
                         NativeFn impl,
                         std::vector<Handle> captures = {});
```

Wraps a positional impl so it also accepts keyword arguments *without* touching the impl. Naming
`params` gives the positional slots names; a keyword call binds them by name and left-fills unset
holes with `None`. The impl still receives a flat positional span. `captures` binds `self` (or
other values) into the returned callable so the receiver arrives in the closure and stays GC-alive.

**Prefer `makeMethod` for anything reachable as `obj.method`.** This is exactly how every built-in
type method (`xs.sort(key = f, reverse = True)`, `s.split(sep = ",")`) and every stdlib
native-class method gains keyword arguments. For a free function or a module function, declare a
signature on `NativeFunction` instead — that additionally surfaces the parameters under `inspect`.

### The protocol slots

Override only what your type supports; every slot defaults to a clear "unsupported" `KiritoError`.

| Slot | Triggered by |
|------|-------------|
| `binary(vm, op, self, rhs)` | `a + b`, `a < b`, `x in c`, … (`BinOp::Add/Sub/Mul/Div/FloorDiv/Mod/Pow/Eq/Ne/Lt/Le/Gt/Ge/In/NotIn`) |
| `unary(vm, op, self)` | `-a`, `not a` (`UnOp::Neg/Not`) |
| `call(vm, args)` | `obj(...)` — makes the value itself callable |
| `getAttr(vm, self, name)` | `obj.field` |
| `setAttr(vm, name, value)` | `obj.field = v` |
| `getItem(vm, keys)` | `obj[i]` / `obj[i, j]` (keys are variadic) |
| `setItem(vm, keys, value)` | `obj[i] = v` / `obj[i, j] = v` (keys are variadic) |
| `slice(vm, start, stop, step)` | `obj[a:b:c]` — a dedicated slice slot (each bound may be `None`) |
| `iterate(vm)` | `for x in obj` — return the elements as a vector of `Handle` |
| `length(vm)` | `len(obj)` |
| `contains(vm, value)` | `x in obj` |
| `str(ctx)` | stringify (`String(obj)`, printing, nesting) |
| `equals(arena, other)` | `==` |
| `hash()` | use as a Set element / Dict key |
| `children(out)` | GC reachability — push every `Handle` your object owns |

If your type holds Kirito values (a container, say), implement `children()` so the garbage
collector can trace them. The bundled `matrix`, `io.open` (File), `BytesIO`, `Random`, and socket
types are all `NativeClass`es — read `stdlib_matrix.hpp` or `stdlib_random.hpp` for production
examples.

### Argument helpers (in `native.hpp`)

Lower-level, when the full `Value` wrapper is more than you need in a hot native impl:

```cpp
inline const std::string& argString(KiritoVM& vm, Handle h, const char* who);   // requires String
inline int64_t            argInt   (KiritoVM& vm, Handle h, const char* who);   // requires Integer
inline void               requireArgs(std::span<const Handle> a, std::size_t n, const char* who);
inline std::vector<int64_t> sliceIndices(KiritoVM& vm, int64_t len,
                                          Handle startH, Handle stopH, Handle stepH);
```

- **`argString` / `argInt`** — throw `"<who> expects a String"` / `"<who> expects an Integer"` on
  a type mismatch.
- **`requireArgs`** — for methods whose fast path indexes a fixed `a[i]`, guard first with a clean
  "expected at least N argument(s)" message rather than reading past the span.
- **`sliceIndices`** — resolve `[start:stop:step]` to a concrete index list. Handles `None` bounds,
  negative indices, out-of-range clamping, and a negative step. Throws on a zero step. The single
  source of truth used by String / Bytes / List slicing.

### Cross-VM value types

A native type can cross between worker VMs **by identity** — the way `parallel.Queue` is shared,
where each VM holds a thin handle to one underlying C++ object. The pattern: keep the real state
in a `KiritoDispatcher`-owned object addressed by an integer id, then expose `_getstate_` /
`_setstate_` so serialization carries only that id, and register a deserializer that rebuilds the
handle and rebinds it via `vm.dispatcher()`.

The shape of it (a **pattern sketch** — `myObj(...)` and `myObjById(...)` stand in for your own
accessor and your own dispatcher lookup, which you add alongside the registry your type owns):

```cpp
// _getstate_ emits the shared object's id; _setstate_ rebinds to the same object in another VM.
if (name == "_getstate_")
    return makeMethod(vm, "_getstate_", {}, [self](KiritoVM& v, std::span<const Handle>) {
        return v.makeInt(static_cast<int64_t>(myObj(v, self).id()));
    }, {self});
if (name == "_setstate_")
    return makeMethod(vm, "_setstate_", {"state"}, [self](KiritoVM& v, std::span<const Handle> a) {
        static_cast<MyVal&>(v.arena().deref(self)).obj =
            myObjById(v, static_cast<uint64_t>(argInt(v, a[0], "_setstate_")));
        return v.none();
    }, {self});
```

`vm.dispatcher()` is null for a bare VM, so guard on it (a value that needs cross-VM identity only
makes sense under a dispatcher). See `stdlib_parallel.hpp` (`QueueVal` and the dispatcher's
`queueById`) for a complete, compiling instance. Live resources that *can't* meaningfully cross
(open sockets, file handles) should simply omit `_getstate_` — serialization then throws a clear
error instead of silently breaking.

### Serialization support for a native type

Opt in via `_getstate_` / `_setstate_` methods that emit and restore a serializable snapshot, then
register a factory so `serialize` / `dump` can reconstruct instances:

```cpp
vm.registerDeserializer("MyType", [](KiritoVM& vm, Handle) -> Handle {
    return vm.alloc(std::make_unique<MyType>());   // _setstate_ then replaces the data
});
```

The value types opted in this way — `Matrix`, `Vector`, `Complex`, `ComplexMatrix`, `DateTime`,
`Random`, and gradient-free `Tensor` — round-trip through both `serialize` (text) and `dump`
(binary).

---

## 9. Design rules

Universal notes that apply everywhere on this page:

- **Naming**: Kirito's public functions and methods are **all lowercase, no underscores**
  (`gettempdir`, `splitext`, `startswith`, `symmetricdifference`) — match that when you add your
  own so a Kirito user cannot tell your extension from a built-in.
- **No global mutable state.** Everything is VM-scoped, so multiple VMs stay isolated. Do not
  cache anything in a `static` variable that has to survive across VMs.
- **Never expose raw pointers.** Hold `Handle`s and deref at the point of use — see
  [Section 3](#3-handles-and-gc-lifetime).
- **Header-only ODR.** Everything `inline` or templated; `#ifndef` include guards (never
  `#pragma once`).
- **Throw `KiritoError`** with a clear, actionable message for bad arguments. Errors are part of
  the language, not an afterthought.

---

## 10. The `Object` protocol

Every Kirito value derives from `Object`. The evaluator only ever talks to this interface, so
built-ins, native (C++) types, and user `class` instances are indistinguishable at the call site.

```cpp
class Object {                                   // object.hpp
public:
    virtual ~Object() = default;

    // pooled allocation (thread-local small-object free-list; bypassed under sanitizers)
    static void* operator new(std::size_t n);
    static void  operator delete(void* p, std::size_t n) noexcept;

    // required
    virtual ValueKind    kind() const = 0;
    virtual std::string  typeName() const = 0;
    virtual bool         truthy() const = 0;
    virtual std::string  str(StringifyCtx&) const = 0;
    virtual bool         equals(const ObjectArena&, const Object& other) const = 0;

    // optional
    virtual bool                     hashable() const;                       // default false
    virtual std::size_t              hash() const;                           // throws by default
    virtual void                     children(std::vector<Handle>&) const;   // GC + serialize reachability
    virtual std::vector<std::string> inspectMembers() const;                 // one entry per member for `inspect()`

    // operation slots — each defaults to a clear "unsupported" KiritoError
    virtual Handle binary (KiritoVM&, BinOp, Handle self, Handle rhs);
    virtual Handle unary  (KiritoVM&, UnOp, Handle self);
    virtual Handle call   (KiritoVM&, std::span<const Handle> args);
    virtual Handle getAttr(KiritoVM&, Handle self, std::string_view name);
    virtual void   setAttr(KiritoVM&, std::string_view name, Handle value);
    virtual Handle getItem(KiritoVM&, std::span<const Handle> keys);         // variadic keys
    virtual void   setItem(KiritoVM&, std::span<const Handle> keys, Handle value);
    virtual Handle slice  (KiritoVM&, Handle start, Handle stop, Handle step); // None handles = omitted
    virtual std::optional<std::vector<Handle>> iterate(KiritoVM&);
    virtual std::optional<int64_t>             length(KiritoVM&);
    virtual bool                               contains(KiritoVM&, Handle value);   // the `in` operator
};
```

**Pooled `operator new`.** Every `Object` allocation goes through a thread-local segregated-size
free list (`pool.hpp`). Sized classes up to 224 B recycle rather than churning `malloc`/`free`.
Bypassed under AddressSanitizer / UBSan / ThreadSanitizer so the tool still instruments every
allocation.

**`inspectMembers()`.** A `NativeClass` overrides this to describe its members as one-line strings
(`"randint(a, b) -> Integer"`, `"year: Integer"`) for the `inspect()` builtin.

### Recursion guards — `StringifyCtx`, `EqualsGuard`, `singleKey`

```cpp
struct StringifyCtx {                            // object.hpp
    const ObjectArena&                 arena;
    fum::unordered_set<const Object*>  active;   // cycle guard
    KiritoVM*                          vm = nullptr;  // set when a user `_str_` may run
    int                                depth = 0;     // hard-capped at 1000
};

struct EqualsGuard {                             // object.hpp
    static constexpr int kMaxDepth = 1000;
    EqualsGuard();                                // throws KiritoError past kMaxDepth
    ~EqualsGuard();
};

inline Handle singleKey(const Object& self, std::span<const Handle> keys);  // arity-of-one helper
```

- **`StringifyCtx`** is threaded through `Object::str()` so containers detect cycles (already-
  active object — emit `...`) and cap depth on nested-but-acyclic structures.
- **`EqualsGuard`** is an RAII depth counter you instantiate inside a recursive `equals()`.
- **`singleKey`** validates that `getItem`/`setItem` was called with exactly one index (String,
  Bytes, Dict, List — types where multi-key indexing isn't meaningful) and returns the sole key.

---

## 11. Built-in value classes

Every one derives from `Object`. Prefer to construct through the `KiritoVM` helpers and the
`Value` API from [Section 5](#5-working-with-values-the-value-api); the raw classes are for direct
manipulation, custom serialization, and reading/writing public data members.

### Scalars

| Class | Kind | Construct in C++ | Read back |
|-------|------|------------------|-----------|
| `NoneVal` | interned unit value | `vm.none()` | `.isNone()` |
| `BoolVal` | interned `True`/`False` | `vm.makeBool(b)` | `.asBool()` |
| `IntVal` | 64-bit signed integer (small ints in `[-128,255]` interned) | `vm.makeInt(v)` | `.asInt()` |
| `FloatVal` | IEEE-754 double; equality is **exact** | `vm.makeFloat(v)` | `.asFloat()` (also accepts Integer) |

Each has an `explicit T(v)` constructor and a `value()` accessor if you must reach past the `Value`
API. `Bool` is **not** a subtype of `Integer` — comparing `True` with `1` returns `False`.

### `StrVal` (in `builtins.hpp`)

```cpp
class StrVal : public Object {
public:
    explicit StrVal(std::string v);
    const std::string& value() const;
    bool isAscii() const;                                    // O(1) after construction
    const std::vector<std::size_t>& codePointStarts() const; // lazy, cached
};
```

Immutable UTF-8 text, indexed and sliced by **code point**. Hash is computed once at construction.
The ASCII fast-path (`isAscii()`) keeps indexing and length O(1); non-ASCII strings build a
per-code-point offset table on first access. Construct with `vm.makeString(std::string)`; iterate
code points via `Value::items()`.

### `BytesVal` (in `bytes.hpp`)

```cpp
class BytesVal : public NativeClass<BytesVal> {
public:
    static constexpr const char* kTypeName = "Bytes";
    std::string data;                            // public

    BytesVal() = default;
    explicit BytesVal(std::string d);
};
```

Immutable sequence of raw bytes (0–255) — the byte-exact counterpart to `StrVal`. Construct in
C++ with `vm.alloc(std::make_unique<BytesVal>(std::string(raw)))`; read `.data` directly.
`String → Bytes` via `s.encode([enc])`; `Bytes → String` via `b.decode([enc])`. `latin-1` is the
lossless round-trip encoding.

### `ListVal` (in `collections.hpp`)

```cpp
class ListVal : public Object {
public:
    std::vector<Handle> elems;                   // direct access
};
```

Ordered, mutable sequence. `elems` is public, but prefer the [`List`](#list) wrapper — it pins its
own allocation for the GC and gives you `push`/`pop`/`[]`/range-for. Drop to a bare `ListVal` under a
`RootScope` only when you need direct `elems` access the wrapper doesn't expose.

### `DictVal` (in `collections.hpp`)

```cpp
class DictVal : public Object {
public:
    fum::unordered_map<std::size_t, std::vector<std::pair<Handle, Handle>>> buckets;
    std::size_t count = 0;

    void          set   (ObjectArena& arena, Handle key, Handle value);   // insert or overwrite
    const Handle* find  (const ObjectArena& arena, Handle key) const;     // nullptr if absent
    bool          remove(ObjectArena& arena, Handle key);                 // true iff removed

    std::vector<Handle>                    keys() const;
    std::vector<std::pair<Handle, Handle>> pairs() const;
};
```

Hash-bucketed key→value map. Keys must be hashable (`set`/`find` throws `unhashable type`).
Iteration order is bucket order — unspecified.

### `SetVal` (in `collections.hpp`)

```cpp
class SetVal : public Object {
public:
    fum::unordered_map<std::size_t, std::vector<Handle>> buckets;
    std::size_t count = 0;

    bool add     (ObjectArena& arena, Handle value);                   // false if already present
    bool contains(const ObjectArena& arena, Handle value) const;
    std::vector<Handle> items() const;
};
```

Hash-bucketed unique collection. Same hashable requirement as `DictVal`.

### Auxiliary helpers

Stringification (in `builtins.hpp` / `collections.hpp`):

- `floatToString(double)` — display renderer (`%.15g` with `.0` suffix so a round Float prints
  `2.0`); overflow-guarded (falls back to `%.17g`).
- `floatToRoundtrip(double)` — JSON-safe renderer (shortest precision that still `parse(repr(d))
  == d`).
- `reprString(const std::string&)` — quote + escape a String for `repr` form.
- `stringifyChild(StringifyCtx&, Handle)` — the `str`-vs-`repr` distinction (bare String raw;
  nested String quoted). Every container's `str()` recurses through this.
- `stringifyGuarded(this, ctx, open, close, emit)` — cycle-and-depth-guarded wrapper for
  containers.

UTF-8 code-point helpers (in `builtins.hpp`):

- `utf8Length(const std::string&)` — code-point count.
- `utf8Starts(const std::string&)` — byte offset of each code point.
- `utf8DecodeAt(const std::string&, std::size_t)` — the code point at a byte index.
- `utf8Encode(unsigned cp, std::string& out)` — append a code point.
- `utf8ToUpperCp` / `utf8ToLowerCp` — Unicode case mapping (ASCII + Latin-1 Supplement + Latin
  Extended-A).

Hashable check + bucket probe (in `collections.hpp`):

- `requireHashable(const Object&)` — throw `"unhashable type '...'"` if the object cannot be a
  Dict/Set key.
- `probeBucket(arena, bucket, key, keyOf)` — the shared linear probe used by `DictVal::find` and
  `SetVal::contains`.

---

## 12. User classes and Kirito functions

### `ClassValue` (in `class_value.hpp`)

```cpp
class ClassValue : public Object {
public:
    std::string name;
    fum::unordered_map<std::string, Handle> methods;
    Handle base{};
    bool   hasBase = false;
    Handle selfHandle{};                         // the class's own arena handle

    const Handle* findMethod(const ObjectArena&, const std::string& n) const;   // walks base chain
    Handle call    (KiritoVM&, std::span<const Handle> args) override;          // instantiate
    Handle callFull(KiritoVM&, std::span<const Handle> pos,
                    std::span<const NamedArg> named);                            // instantiate + kwargs
};
```

A user-defined class. Calling it instantiates (runs `_init_` on the new `InstanceValue`).

### `InstanceValue` (in `class_value.hpp`)

```cpp
class InstanceValue : public Object {
public:
    Handle cls{};
    Handle selfHandle{};
    std::string className;
    bool hasHashDunder = false;                  // set at instantiation (walks class chain)
    bool hasEqDunder   = false;
    fum::unordered_map<std::string, Handle> attrs;

    Handle callKw(KiritoVM&, std::span<const Handle> args, std::span<const NamedArg> named);
    const Handle* findMethod(const ObjectArena&, const std::string& n) const;
};
```

A live instance. The `_hash_` / `_eq_` presence flags are cached so the Dict/Set hot path avoids a
method lookup. All operator slots dispatch to `_op_` methods when defined.

### `SuperValue` (in `class_value.hpp`)

Proxy for `self._super_()`. Method resolution begins at the **base** of the class whose method is
currently running.

### `KiFunction` (in `function.hpp`)

The Kirito-authored function value — a user `Function(...)` literal builds one. An embedder rarely
constructs these; a native that wants to call back into a Kirito callable uses `.call(...)` /
`.callFull(...)` uniformly with `NativeFunction`.

### Naming helpers (in `class_value.hpp`)

- `binOpMethod(BinOp) → const char*` — Kirito method name for a binary op (`"_add_"`, `"_lt_"`).
- `unOpMethod(UnOp) → const char*` — `"_neg_"` or `"_not_"`.
- `isPrivateName(std::string_view) → bool` — single leading underscore, no trailing (private-per-
  class-chain).

---

## 13. Scopes and modules

### `EnvValue` (in `environment.hpp`)

```cpp
class EnvValue : public Object {
public:
    EnvValue();
    explicit EnvValue(Handle parent);
    bool          hasParent() const;
    Handle        parent() const;
    void          define(const std::string& name, Handle h);          // set or overwrite
    bool          assignLocal(const std::string& name, Handle h);     // rebind if present
    const Handle* findLocal(const std::string& name) const;
    const auto&   locals() const;                                     // for module-member harvest
};
```

A lexical scope: flat `name → Handle` vector plus an optional parent scope handle. Function scopes
are small, so a linear scan beats a hash map. Chain is global → module → local.

### `ModuleValue` (in `module.hpp`)

```cpp
class ModuleValue : public Object {
public:
    explicit ModuleValue(std::string name);
    fum::unordered_map<std::string, Handle> members;   // direct access
    const std::string& name() const;
};
```

A namespace of named members reached by `.` access (`io.print`). Rebinding a member is allowed
(`io.stdout = io.open(...)`), so `setAttr` is not const. Built either by a `NativeModule`'s
`setup()` or by importing a `.ki` file.

---

## 14. Functions (native)

### Callable typedefs

```cpp
using NativeFn   = std::function<Handle(KiritoVM&, std::span<const Handle>)>;
using NativeFnKw = std::function<Handle(KiritoVM&,
                                        std::span<const Handle>,
                                        std::span<const NamedArg>)>;
```

- **`NativeFn`** — positional handles only.
- **`NativeFnKw`** — additionally receives named arguments (for variadic-and-keyword-aware
  natives, e.g. `io.print(..., stream=f)`).

### `NativeParam`

```cpp
struct NativeParam {
    std::string name;
    std::string annotation;                      // "" if unannotated
    bool   hasDefault = false;
    Handle defaultValue{};

    NativeParam(std::string n, std::string ann = "");
    NativeParam(std::string n, std::string ann, Handle def);
};
```

One declared parameter of a signatured native function. Construct as `{"x"}`, `{"x", "Integer"}`,
or `{"x", "Integer", vm.makeInt(0)}` (with default).

### `NativeFunction` — full surface

```cpp
class NativeFunction : public Object {
public:
    // Constructors — the three shapes are shown in Section 6.

    const std::string&              name() const;
    bool                            acceptsKwargs() const;
    bool                            hasSignature() const;
    const std::vector<NativeParam>& params() const;
    const std::string&              returnType() const;

    Handle call  (KiritoVM&, std::span<const Handle> args) override;                     // positional
    Handle callKw(KiritoVM&, std::span<const Handle> args, std::span<const NamedArg>);   // signatured only

    // Bind positional + named against the declared signature into a flat positional vector
    // (fills defaults, catches unknown / duplicate / missing kwargs). Only if hasSignature().
    std::vector<Handle> bindArgs(std::span<const Handle> positional,
                                 std::span<const NamedArg> named) const;
};
```

---

## 15. `KiritoVM` — full surface

The subset in [Section 2](#2-entry-points) is what most embedders use. The rest, for direct arena
access, GC control, and interpreter internals:

```cpp
class KiritoVM {                                 // vm.hpp
public:
    KiritoVM();
    ~KiritoVM();
    static KiritoVM* activeVM();                 // thread-local currently-active VM

    // arena + allocation
    ObjectArena&       arena();
    const ObjectArena& arena() const;
    Handle             alloc(std::unique_ptr<Object> obj);

    // interned singletons + primitive construction
    Handle none() const;
    Handle undefined() const;                    // internal sentinel; do NOT surface into Kirito
    Handle makeBool(bool v) const;
    Handle makeInt   (int64_t v);
    Handle makeFloat (double v);
    Handle makeString(std::string v);

    // scopes
    Handle global() const;
    Handle newScope(Handle parent);
    Handle newModuleScope(bool isMain = true);   // binds `arglist` and `argmain`
    void   setArgs(const std::vector<std::string>& args);

    // GC controls
    void        pushTemp(Handle h);              // prefer RootScope
    std::size_t tempMark() const;
    void        popTempTo(std::size_t mark);
    void        pushAuxRoots(const std::vector<Handle>* v);   // extra root region
    void        popAuxRoots();
    void        collectGarbage();
    void        setGcThreshold(std::size_t n);
    void        setGcEnabled(bool on);
    std::size_t liveCount() const;

    // call-depth guard
    void        enterCall();
    void        leaveCall();
    void        setMaxCallDepth(std::size_t n);

    // registering
    void        registerGlobal(const std::string& name, Handle value);
    using ModuleFactory = std::function<Handle(KiritoVM&)>;
    void        registerModule(std::string name, ModuleFactory factory);
    template<class T> void install();            // install a NativeModule subclass
    void        installStandardLibrary();
    void        registerSourceModule(std::string name, std::string_view source);
    Handle      importModule(const std::string& name);
    void        addLibPath(std::string dir);
    const std::vector<std::string>& libPaths() const;

    // class + serializer registries
    void          registerClass(const std::string& name, Handle cls);
    const Handle* findClass    (const std::string& name) const;
    void          registerDeserializer(std::string name,
                                       std::function<Handle(KiritoVM&, Handle)> fn);

    // running code
    Handle runSource(std::string_view source, std::string_view chunkName = "<main>");
    Handle runRepl  (std::string_view source);                             // persistent scope
    Handle evalIn   (std::string_view source, Handle scope,
                     std::string_view chunkName = {});
    std::string stringify(Handle h) const;

    // multiprocessing hookup
    void              setDispatcher(KiritoDispatcher* d);
    KiritoDispatcher* dispatcher() const;

    // traceback (filled by the bytecode VM on error)
    void                            setLastTraceback(std::vector<TraceFrame> tb);
    const std::vector<TraceFrame>&  lastTraceback() const;

    // chunk-file scope (RAII; used internally for attributing errors)
    class ChunkFileScope {
    public:
        ChunkFileScope(KiritoVM& vm, std::string f);
        ~ChunkFileScope();
    };
};
```

RAII helpers next to the VM:

```cpp
struct CallGuard {                          // vm.hpp — increments/decrements the recursion counter
    explicit CallGuard(KiritoVM& v);
    ~CallGuard();
};

struct RootScope {                          // vm.hpp — the rooting rule (see Section 3)
    explicit RootScope(KiritoVM& v);
    ~RootScope();
    Handle add(Handle h);
};
```

---

## 16. `ObjectArena` (in `arena.hpp`)

The single owner of every shared Kirito value. Reached from a `KiritoVM` via `vm.arena()`; you
rarely touch it directly (prefer the `Value` API), but two operations belong to it.

```cpp
class ObjectArena {
public:
    Handle        alloc(std::unique_ptr<Object> obj);
    Object&       deref(Handle h);
    const Object& deref(Handle h) const;

    // GC primitives (driven by KiritoVM; do not call these directly)
    void        clearMarks();
    bool        markIfUnmarked(Handle h);
    std::size_t sweep();

    std::size_t liveCount() const;
    std::size_t capacity() const;
};
```

`deref` throws `"dangling handle ..."` on a stale or out-of-range handle. Prefer `vm.alloc(...)`
over `vm.arena().alloc(...)` — the VM version is GC-aware and may collect before returning.

Reclamation is precise mark-sweep: a slot's `unique_ptr` frees the `Object`, and the slot's
`generation` is bumped so any handle pointing at the reused slot detects the stale generation.
**Live objects never move** — a `Handle` is stable across GC cycles.

---

## 17. Multiprocessing internals

The cross-VM primitives that the `parallel` module hands to Kirito. Reachable from a
`KiritoDispatcher`; you'd only touch them directly when implementing a new cross-VM value type.

All primitives derive from `Waitable`:

```cpp
class Waitable {                                 // dispatcher.hpp
public:
    virtual ~Waitable() = default;
    virtual void abort() = 0;                    // wake blocked callers so shutdown can join
};
```

`enum class WaitResult { Ok, TimedOut, Closed, Aborted, Reentrant, Broken };` is the shared
return-type across the primitives' blocking ops.

The primitives themselves:

- **`ConcurrentQueue`** — the central transfer primitive. `put(blob, block, timeout)` /
  `get(block, timeout, &out)` return a `WaitResult`; `close()` marks the queue done (subsequent
  `put`s throw; `get` drains then throws).
- **`Lock`** — non-reentrant mutex. `acquire(block, timeout)` / `release()` / `locked()`.
- **`Event`** — resettable flag. `set()` / `clear()` / `isset()` / `wait(timeout)`.
- **`Semaphore`** — permit counter. `acquire(block, timeout)` / `release()`.
- **`Barrier`** — N-party rendezvous. `wait(timeout, &indexOut)` / `resetBarrier()` /
  `parties()` / `nwaiting()`.

Each also has an integer `id()` accessor; the `KiritoDispatcher` `xById(id)` lookups turn a
serialized id back into the same shared object in a worker VM. This is how a Queue travels
between VMs by identity rather than by value.

`Task` is the per-worker record kept inside the dispatcher (`std::thread thread; std::atomic<bool>
done; std::string result; bool hasError; std::string errorText;`); the surface an embedder sees is
`spawnTask` / `taskDone` / `joinTask` on the dispatcher.

Argument marshalling:

- `packArgs(vm, positional, named)` → a serialized blob suitable for the dispatcher's cross-VM
  transport.
- `unpackArgs(vm, packed, &pos, &named)` → the reverse on the worker side.
- `findFunctionBySpan(prog, line, col)` — used to look up a spawned function in the worker's
  freshly-parsed program.

The `KiritoDispatcher` methods that wire these together are shown in
[Section 2](#2-entry-points).

---

## 18. Enums, structs, and numeric helpers

### `ValueKind` (in `object.hpp`)

```cpp
enum class ValueKind {
    None, Bool, Integer, Float, String,
    Array, List, Set, Dict,
    Function, NativeFunction, Module, Class, Instance,
    Environment,
};
```

`Object::kind()` returns one of these. `Array` is an internal duplicate of `List` used by the
compiler; every user-facing List is `ValueKind::List`. `Instance` covers user-class instances
*and* every `NativeClass<Derived>` (which reports `ValueKind::Instance` + its own `typeName()`).

### `BinOp` / `UnOp` (in `common.hpp`)

```cpp
enum class BinOp { Add, Sub, Mul, Div, FloorDiv, Mod, Pow,
                   Eq, Ne, Lt, Le, Gt, Ge, In, NotIn };
enum class UnOp  { Neg, Not };
```

The operator vocabulary passed to `Object::binary` / `Object::unary`. `binOpMethod(BinOp)` /
`unOpMethod(UnOp)` (in `class_value.hpp`) map each op to its Kirito special-method name (`_add_`,
`_lt_`, `_neg_`, …).

### `SourceSpan`, `TraceFrame`, `formatTraceback` (in `common.hpp`)

```cpp
struct SourceSpan {
    uint32_t line = 0;                            // 1-based; 0 means "no position"
    uint32_t col = 0;
    uint32_t length = 0;                          // token length in code points
};

struct TraceFrame {
    std::string function;                         // "" or "<function>" (anonymous) or "<module>"
    std::string file;                             // source chunk
    uint32_t    line = 0;
};

std::string formatTraceback(const std::vector<TraceFrame>&);
```

`SourceSpan` is carried on every parse / runtime error. `TraceFrame`s are accumulated on an
in-flight exception (innermost first) and snapshotted onto the VM as its `lastTraceback()`.
`formatTraceback` renders the frames as a "Traceback (most recent call last):" block.

### Numeric helpers (in `common.hpp`)

- `parseDouble(const std::string& s, std::size_t* consumed = nullptr) → double` — like `std::stod`
  but treats underflow to subnormal/zero as success; still throws on no-conversion or overflow to
  ±inf.
- `hexDigitValue(char) → int` — 0–15 for a hex digit; `-1` otherwise.
- `floatClose(a, b, relTol, absTol) → bool` — `|a - b| ≤ max(relTol · max(|a|, |b|), absTol)`.
  NaN is never close; `inf == inf` is exact. The shared tolerant comparison behind Kirito's
  `.compare(other, rel_tol, abs_tol)` method.

---

## 19. Header layout

`#include "kirito.hpp"` transitively pulls the whole interpreter in. Individual headers:

| Header | Purpose |
|--------|---------|
| `common.hpp` | `SourceSpan`, `TraceFrame`, `BinOp`/`UnOp`, `formatTraceback`, `parseDouble`, `hexDigitValue`, `floatClose` |
| `exceptions.hpp` | `KiritoThrow`, `KiritoError` |
| `handle.hpp` | `Handle`, `NamedArg`, `std::hash<Handle>` specialisation |
| `object.hpp` | `Object` base, `ValueKind` enum, `StringifyCtx`, `EqualsGuard`, `singleKey` |
| `arena.hpp` | `ObjectArena` |
| `builtins.hpp` | `NoneVal`, `BoolVal`, `IntVal`, `FloatVal`, `StrVal`, UTF-8 helpers, stringify helpers |
| `collections.hpp` | `ListVal`, `DictVal`, `SetVal`, `requireHashable`, `probeBucket`, `stringifyGuarded` |
| `bytes.hpp` | `BytesVal` |
| `class_value.hpp` | `ClassValue`, `InstanceValue`, `SuperValue`, `binOpMethod`, `unOpMethod`, `isPrivateName` |
| `environment.hpp` | `EnvValue` (a lexical scope) |
| `module.hpp` | `ModuleValue` |
| `function.hpp` | `NativeFn`, `NativeFnKw`, `NativeParam`, `NativeFunction`, `KiFunction` |
| `native.hpp` | `argString`, `argInt`, `requireArgs`, `sliceIndices`, `ModuleBuilder`, `NativeModule`, `NativeClass`, `makeMethod` |
| `value.hpp` | `Value` + `Bool`/`Integer`/`Float`/`String`/`Bytes`/`List`/`Dict`/`Set` wrappers, `Args` |
| `vm.hpp` | `KiritoVM`, `RootScope`, `CallGuard`, `KiritoVM::ChunkFileScope` |
| `dispatcher.hpp` | `KiritoDispatcher`, `Waitable`, cross-VM `ConcurrentQueue`/`Lock`/`Event`/`Semaphore`/`Barrier` |

The bytecode/parser/compiler headers (`ast.hpp`, `bytecode.hpp`, `compiler.hpp`, `lexer.hpp`,
`parser.hpp`, `resolver.hpp`, `analyzer.hpp`, `runtime.hpp`, `bytecode_vm.hpp`) implement the
interpreter and are called through the `KiritoVM` public surface — an embedder does not use their
symbols directly.

---

## 20. Header-name reservations

After `using namespace kirito;` the following C++ names are taken by the interpreter and its
bundled stdlib. Declaring your own type with the same name fails to compile with
`error: reference to 'X' is ambiguous`. Prefer prefixes (`AppEvent`, `MyDict`) or drop the
`using namespace` and reach into `kirito::` explicitly.

`Lexer`, `Parser`, `Value`, `Handle`, `Dict`, `List`, `Set`, `Args`, `Object`, `Event`, `Task`,
`Queue`, `Lock`, `Semaphore`, `Barrier`, `Socket`, `Session`, `Response`, `DateTime`, `Random`,
`Matrix`, `BytesIO`, `Pattern`, `Match`, `Complex`, `Tensor`, `NativeFunction`, `NativeModule`,
`NativeClass`, `KiritoVM`, `KiritoDispatcher`, `KiritoError`, `KiritoThrow`.
