# Exceptions reference

Every error Kirito can throw — what it means, why it happens, and how to fix it. This page is a
catalogue; for the *language* mechanics of handling errors (`try` / `catch` / `finally` / `throw`),
see the [Errors course lesson](#course-11-errors) and the [Language guide](#language-guide).

## The exception model

Kirito has **two** exception types, sharing one `std::exception` base so a C++ host can catch either:

```
std::exception
   └── KiritoThrow          a raw Kirito `throw <value>` — carries the thrown value
          └── KiritoError   an interpreter diagnostic — carries a message string
```

- **`KiritoError`** is what the interpreter itself throws — a lexer/parser error, a type mismatch, a
  division by zero, a stdlib domain error, and so on. It carries a **message** (the text you see) and
  a source **span** (`file:line:col`). Inside Kirito a `try`/`catch` still catches it: the runtime
  **promotes the message to a Kirito `String`** at the catch site, so `catch as e:` binds `e` to that
  string.
- **`KiritoThrow`** is what a Kirito `throw <value>` produces — it carries the **live value** you threw
  (a String, an instance of your own class, anything), so a **typed catch** (`catch String as e:`,
  `catch MyError as e:`) can route on the value's actual type through the class chain.

Both unwind the VM's block stack, running any `finally` and `with`-exit handlers on the way out, and
both accumulate a **traceback** (the call chain) as they cross frames.

### What a message looks like

Interpreter diagnostics are formatted `file:line:col: message` — e.g.

```
prog.ki:3:11: name 'foo' is not defined
```

The same channel carries non-fatal **`warning:`** lines from the static analyser (unused variable,
dropped value, unreachable code, …); those are not exceptions and never stop execution — disable them
with `ki -w`. In the tables below, a message shown as `` `... '<X>' ...` `` interpolates a runtime
value where `<X>` is.

### Catching errors — in Kirito

<!--norun (illustrative — risky/handle/cleanup are placeholders)-->
```kirito
try:
    risky()
catch ValueError as e:        # a typed catch: matches instances of ValueError (and subclasses)
    handle(e)
catch as e:                   # a bare catch: matches ANYTHING — including an interpreter KiritoError
    io.print("failed: " + e)  # e is the promoted message String here
finally:
    cleanup()                 # always runs
```

A **bare `catch`** also catches any C++ `std::exception` that crosses the native boundary (see
[Standard C++ exceptions](#standard-c-exceptions)) — a misbehaving native module can't escape a Kirito
`try`. Every interpreter error is catchable at run time **except** the ones thrown *before* execution
starts: lexer, parser, name-resolution, and static-analysis errors are reported when the program is
compiled, so a `try` inside the same program can't catch its own syntax error.

### Catching errors — as a C++ embedder

```cpp
try {
    Handle r = vm.runSource(src);
    std::printf("%s\n", vm.stringify(r).c_str());
} catch (const kirito::KiritoError& e) {
    std::cerr << e.what() << "\n";          // interpreter diagnostic, already formatted
} catch (const kirito::KiritoThrow&) {
    // an uncaught Kirito `throw` escaped; runSource normally re-wraps this as a KiritoError
    // ("uncaught exception: <value>") so you rarely catch KiritoThrow directly
} catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";   // e.g. a std::bad_alloc a native couldn't handle
}
```

`KiritoError` **is-a** `KiritoThrow`, so if you distinguish them, order `catch (KiritoError&)` **before**
`catch (KiritoThrow&)`.

## Compile-time errors (thrown before your program runs)

These come from the lexer, parser, name resolver, and compiler — reported while the program is being
compiled, so a `try` in the same program **cannot catch them**. Fix the source and re-run. (Two
compiler checks — `positional argument follows keyword argument` and `duplicate switch case value` —
are deferred to the point the code is reached, so they *are* catchable; they're noted below.)

### Lexer — indentation

| Message | Cause | Fix |
|---|---|---|
| `inconsistent use of tabs and spaces in indentation` | A line's indent mixes tabs and spaces so it measures differently under tab=8 vs tab=1 | Indent with only tabs or only spaces |
| `inconsistent dedent` | A dedent doesn't line up with any enclosing indentation level | Align the dedented line to an existing outer block level |

### Lexer — literals & escapes

| Message | Cause | Fix |
|---|---|---|
| `invalid numeric literal '<text>'` | A malformed number (bad digits for the base, stray prefix) | Write a valid decimal/hex/octal/binary/float literal |
| `unterminated string` / `unterminated f-string` (also `unterminated triple-quoted …`) | A string reaches EOF, a single-line string hits a newline, or a raw/f string ends on a lone backslash | Close the quote (or use a triple-quoted form to span lines) |
| `invalid \x escape (expected hex digit)` | `\x` in a cooked string not followed by a hex digit | Supply two hex digits, e.g. `\x41` |
| `invalid escape '\<e>'` | An unrecognized backslash escape in a cooked string | Use a valid escape (`\n \t \r \0 \\ \" \'` `\xHH`) or a raw string |

### Lexer — tokens

| Message | Cause | Fix |
|---|---|---|
| `unexpected '!' (did you mean '!=' or 'not'?)` | A bare `!` (Kirito has no `!` unary operator) | Use `not` for negation or `!=` for inequality |
| `unexpected character '<c>'` | A character that starts no valid token | Remove the stray character |

### Parser — expected token / syntax

| Message | Cause | Fix |
|---|---|---|
| `expected <what>` | A required token is missing (e.g. `')' after parameters`, `':' after Function parameters`, `a parameter name`) | Add the missing token the message names |
| `expected end of statement` | Extra tokens after a complete statement | Split into separate statements or remove the trailing tokens |
| `expected an expression` | An expression was required but a non-expression token appeared | Provide a valid expression |
| `expected a member name after '.'` | `.` not followed by an identifier | Write a valid attribute name after the dot |
| `expected an index expression inside '[ ]'` | Empty subscript where an index was required | Put an index/slice inside the brackets |
| `expression nested too deeply` | Source expression exceeds the recursive-descent depth bound | Flatten or split the deeply nested expression |

### Parser — statement-context rules

| Message | Cause | Fix |
|---|---|---|
| `'break' outside loop` | `break` with no enclosing loop | Only use `break` inside `while`/`for` |
| `'continue' outside loop` | `continue` with no enclosing loop | Only use `continue` inside `while`/`for` |
| `'return' outside function` | `return` at module/class-body scope | Only `return` inside a function body |
| `two starred targets in assignment` | More than one `*name` on an unpack target | Use at most one starred target |
| `non-default parameter '<name>' follows a default parameter` | A parameter without a default declared after one with a default | Move defaulted parameters last |
| `an inline function cannot be comma-packed here …` | A bare comma-pack whose element is an inline-bodied `Function(): …` — e.g. `var f = Function(): return a, b` | Use an indented block body, or wrap the function in a List `[ ]` (a call-arg / list-element comma is fine) |

### Parser — switch / try

| Message | Cause | Fix |
|---|---|---|
| `a switch can have only one 'default'` | A second `default:` arm in a switch | Keep a single `default:` arm |
| `expected 'case' or 'default' in switch body` | A switch-body statement that isn't a `case`/`default` arm | Only put `case`/`default` arms in a switch body |
| `'switch' needs at least one 'case' or a 'default'` | An empty switch body | Add at least one `case` or a `default` |
| `'try' needs at least one 'catch' or a 'finally'` | A `try` with no handler and no finally | Add a `catch` or `finally` clause |

### Parser — f-strings

| Message | Cause | Fix |
|---|---|---|
| `f-string '{...}' must contain a single expression` | An f-string field is empty, holds more than one expression, or fails to parse | Put exactly one expression in the braces |
| `f-string '{...}' must contain an expression` | An f-string field parsed to a non-expression | Use a single expression inside the braces |
| `unmatched '{' in f-string` | An opening `{` with no matching `}` | Balance the braces (or `{{` to escape a literal brace) |
| `single '}' in f-string` | A lone `}` in f-string text | Escape it as `}}` or add the matching `{` |

### Resolver — undefined names

| Message | Cause | Fix |
|---|---|---|
| `name '<X>' is not defined` | A reference bound to no parameter, no `var`/`for`/`class`/`catch`/`with` name in an enclosing scope, no run-scope binding, and no builtin — resolved at compile time, so it is **not** catchable | Declare the name (`var`) or fix the typo before use |

### Compiler — assignment targets & nesting

| Message | Cause | Fix |
|---|---|---|
| `invalid assignment target` | Assigning to something that isn't a name, index, or member | Assign only to a name, `x[i]`, or `x.attr` |
| `starred expression is only valid as an assignment target` | `*expr` used outside an unpack target | Only star a name on the left of `=`/`var`/`for` |
| `'break'/'continue' outside a loop` | Loop-exit compiled with no enclosing loop context | Only use `break`/`continue` inside a loop |
| `expression too deeply nested to evaluate` | Compiler-side nesting bound exceeded | Flatten or split the nested expression |

### Compiler — calls & switch (deferred → catchable at runtime)

| Message | Cause | Fix |
|---|---|---|
| `positional argument follows keyword argument` | A positional argument written after a keyword argument in a call | Put all positional args before keyword args |
| `duplicate switch case value` | Two `case` arms with the same literal scalar value | Make each case value distinct |
| `switch case value must be Integer, Float, String, Bool, or None` | A `case` label that evaluates to a non-scalar at run time | Use a constant scalar case label |

### Static analysis warnings (NOT exceptions)

The analyzer (`analyzer.hpp`) never throws — it prints `file:line:col: warning: <msg>` to stderr and
execution continues. Disable it with `ki -w` / `--no-warn`.

| Warning | Cause |
|---|---|
| `variable '<name>' is assigned but never used` | A function-local binding never read |
| `result of expression is unused; prefix with 'discard' to ignore it intentionally` | A bare expression statement whose non-`None` value is dropped |
| `variable '<name>' is re-declared in this block` | A `var` re-declared in the same block |
| `unreachable code (the block already returns/throws/breaks/continues before this)` | A statement after a terminator in the same block |
| `self-assignment of '<name>' has no effect` | `x = x` (name-to-name) |
| `duplicate parameter name '<name>'` | The same parameter name declared twice |
| `todo: <message>` | A `todo` statement (a deliberate reminder) |

## Runtime errors — the core language

Everything below is a `KiritoError` (catchable by a bare `catch`) unless the type is shown in **bold**.

### Type errors — operators

| Message | Cause | Fix |
|---|---|---|
| `unsupported operand type '<T>' for arithmetic with '<U>'` / `… for comparison with '<U>'` | Numeric binary op or `<`/`>`/`<=`/`>=` given a non-numeric operand (`1 + "x"`, `1 < "x"`) — `<T>` is the offending operand, `<U>` the other | Convert operands to compatible numeric types |
| `type '<T>' does not support this binary operator` | A type with no such operator | Use a supported type/operator, or define the dunder |
| `type '<T>' does not support this unary operator` | Unary `-`/`not` on a type lacking it | Use a supporting type |
| `type 'String' does not support this operator` | Operator on String outside `+`/`*`/comparisons | Use a valid String operator |
| `can only concatenate String to String, not '<T>'` | `"a" + <non-String>` | Concatenate only Strings (convert with `String(x)`) |
| `can only concatenate List to List, not '<T>'` | `[...] + <non-List>` | Concatenate only Lists |
| `can only repeat String by an Integer` / `can only repeat List by an Integer` | `"x" * "y"` / `[1] * 2.0` | Repeat by an Integer count |
| `cannot order '<X>' and '<Y>'` | `sorted`/`sort`/`min`/`max` (or List-vs-List element ordering) hitting two unorderable types — a *direct* `a < b` on incompatible types instead raises the `unsupported operand`/`does not support this operator` message above | Compare only ordered/like types |
| `cannot order 'List' and '<T>'` | List ordered against a non-List | Compare List to List |
| `compare() expects a number, not '<T>'` | `n.compare(other)` with non-numeric `other` | Pass a number |
| `compare() rel_tol and abs_tol must be numbers` | Non-numeric tolerance kwarg to `.compare` | Pass numeric `rel_tol`/`abs_tol` |

### Type errors — protocol slots (callable / iterable / subscriptable / length / membership)

| Message | Cause | Fix |
|---|---|---|
| `type '<T>' is not callable` | Calling a non-callable value | Call a function/class/callable object |
| `type '<T>' is not iterable` | Iterating (`for`, unpack) a non-iterable | Iterate a collection/string/iterator |
| `type '<T>' is not indexable` | `x[i]` on a non-subscriptable type | Index a List/Dict/String/etc. |
| `type '<T>' does not support item assignment` | `x[i] = v` on a type that can't be mutated by index | Use a mutable indexable type |
| `type '<T>' takes exactly one index` | Multi-key `x[i, j]` on a type accepting one index | Pass a single index |
| `type '<T>' does not support slicing` | `x[a:b]` on a non-sliceable type | Slice a String/Bytes/List |
| `type '<T>' has no length` | `len(x)` on a type with no length | Use a sized type / define `_len_` |
| `type '<T>' does not support 'in'` | `x in y` where `y` has no membership | Use a container / define `_contains_` |
| `type '<T>' has no attribute '<name>'` | Base `getAttr`/`setAttr` fallback | Use a valid attribute |

### Attributes & private members

| Message | Cause | Fix |
|---|---|---|
| `'<class>' object has no attribute '<name>'` | Missing attribute/method on an instance | Reference an existing member |
| `'super' object has no attribute '<name>'` | `self._super_().<name>` not found on the parent chain | Reference a member the parent defines |
| `cannot access private member '<_name>' of '<T>' outside its class` | Touching a `_private` member from outside the class chain | Access it only from a method of the class/subclass |

### Names

| Message | Cause | Fix |
|---|---|---|
| `name '<X>' is not defined` | Load/rebind of an unbound name at run time (also caught earlier by the resolver in most cases) | Declare with `var` or fix the spelling |
| `name '<X>' is not defined` (read/rebind before its own `var` runs) | A local **read or plain `=` before its `var` executes** — Kirito uses strict lexical addressing (like Python's *UnboundLocalError*): a name declared in a scope is that scope's binding throughout, so it never silently reads an outer variable of the same name | Move the read below the `var`, or use a different name for the local |

### Indexing & slicing

| Message | Cause | Fix |
|---|---|---|
| `index out of range` / `List/String/Bytes index out of range` | Index beyond sequence bounds | Index within `0..len-1` (or a valid negative index) |
| `index must be Integer, not '<T>'` / `Bytes index must be Integer, not '<T>'` | Non-Integer sequence index | Use an Integer index |
| `slice indices must be Integer or None` | Non-Integer/None slice bound | Use Integer or `None` bounds |
| `slice step cannot be zero` | `x[a:b:0]` | Use a non-zero step |
| `pop index out of range` / `pop from empty List` | `pop()` on empty or bad index | Pop only in-range from a non-empty list |
| `pop index must be an Integer` | Non-Integer arg to List `pop` | Pass an Integer index |
| `remove: value not in List` / `index: value not in List` | `list.remove(v)`/`list.index(v)` for a value not present | Check with `in` first |

### Dict keys / Set / hashing

| Message | Cause | Fix |
|---|---|---|
| `key not found: <key>` | Dict indexing/`remove` on an absent key | Check with `in` or use `.get(k, default)` |
| `unhashable type '<T>'` | Using an unhashable value as a Dict/Set key or in `hash()` | Use a hashable (immutable) key |
| `unhashable type` | Set `remove` of an unhashable value | Pass a hashable value |
| `pop from an empty Set` | `set.pop()` when empty | Pop only from a non-empty set |
| `Dict changed size during a key comparison` / `Set changed size during a value comparison` | A user `_eq_`/`_hash_` mutated the container mid-probe | Don't mutate the container from its keys' `_eq_`/`_hash_` |
| `remove: value not in Set` | `set.remove(v)` for an absent value | Use `discard` (no-op if absent) or check first |
| `pop: key not found` | `dict.pop(k)` (no default) for an absent key | Pass a default, or check with `in` |
| `popitem: dictionary is empty` | `dict.popitem()` on an empty Dict | Check the Dict is non-empty |

### Arithmetic

| Message | Cause | Fix |
|---|---|---|
| `division by zero` | `/` with a zero divisor | Guard against a zero denominator |
| `integer division by zero` / `float division by zero` | `//` (or `/`) by zero | Check the divisor |
| `integer modulo by zero` / `float modulo by zero` | `%` by zero | Check the divisor |
| `zero cannot be raised to a negative power` | `0 ** -n` | Avoid a negative exponent on zero |
| `a negative base cannot be raised to a fractional power` | `(-2) ** 0.5` | Use the `complex` module for such powers |

### Bitwise & shift builtins

| Message | Cause | Fix |
|---|---|---|
| `shl: negative shift count` / `shr: negative shift count` | Negative count to `shl`/`shr` | Pass a non-negative count |
| `<fn>: <who> must be an Integer` | Non-Integer arg to a bitwise builtin | Pass Integers |
| `<name> expects an Integer` | `bin`/`oct`/`hex`/etc. given a non-Integer | Pass an Integer |

### Arity — positional / keyword / missing

| Message | Cause | Fix |
|---|---|---|
| `function takes <N> positional argument(s) but <M> were given` | Too many positional args | Pass at most N |
| `function got an unexpected keyword argument '<name>'` | Keyword not matching any parameter | Use a declared parameter name |
| `function got multiple values for argument '<name>'` | Same parameter given positionally and by keyword | Pass it once |
| `function missing required argument '<name>'` | Required parameter unbound | Provide the argument |
| `argument '<name>' must be <Type>, got <T>` | Enforced parameter annotation violated | Pass a value of the annotated type |
| `function must return <Type>, got <T>` | Enforced return annotation violated | Return the annotated type |
| `this callable does not accept keyword arguments` | Keyword args to a callable that can't take them | Call positionally |
| `<name>() takes no arguments (no _init_ defined)` | Instantiating a class (with args) that has no `_init_` | Define `_init_` or drop the args |
| `<name> expected <N> argument(s)` (e.g. `len expected 1 argument`) | Builtin/method arg-count mismatch | Match the builtin's arity |

### Unpacking

| Message | Cause | Fix |
|---|---|---|
| `cannot unpack non-iterable '<T>'` | `a, b = <non-iterable>` | Unpack an iterable |
| `expected <N> values to unpack, got <M>` | Count mismatch, no star target | Match the target count |
| `expected at least <N> values to unpack, got <M>` | Too few values for a starred unpack | Provide enough values |

### Iteration / join / apply

| Message | Cause | Fix |
|---|---|---|
| `join expects an iterable` | `sep.join(x)` with non-iterable `x` | Pass an iterable of Strings |
| `apply expects a function` | `.apply()` (List/Dict/Set/String/Bytes) with no/invalid fn | Pass a callable |
| `String apply: result must be a String` / `Bytes apply: result must be a byte (0..255)` | `.apply` fn returns the wrong element type | Return a String char / a 0–255 Integer |
| `extend expects an iterable` / `update expects a Dict or an iterable of [key, value] pairs` | Bad arg to `extend`/`update` | Pass the right iterable |
| `update: each pair must have exactly 2 elements (key, value)` / `Dict() items must be [key, value] pairs` | Malformed pair in Dict update/construction | Provide 2-element pairs |

### Assert & raw throw

| Message | Cause | Fix |
|---|---|---|
| **KiritoThrow** `assertion failed` (or the custom message) | `assert cond[, msg]` with a falsy `cond` | Fix the condition or the assumption |
| **KiritoThrow** `<value>` | A Kirito `throw <value>` throws a live value | Catch with a typed `catch`, or fix the cause |

### Recursion & nesting guards

| Message | Cause | Fix |
|---|---|---|
| `maximum recursion depth exceeded` | Native call depth exceeds the call-depth guard (deep/infinite recursion) | Reduce recursion / raise the configured limit |
| `maximum comparison recursion depth exceeded (cyclic structure?)` | A deep/cyclic structure compared with `==`/`<`/`sort`/`min`/`max` | Avoid comparing cyclic structures |
| `structure too deeply nested to stringify` | `str()`/print of a structure >1000 deep | Flatten the structure |
| `expression too deeply nested to evaluate` / `expression nested too deeply` | Pathologically nested source | Simplify the expression nesting |
| `'<Class>' _iter_ recurses too deeply (does _iter_ return self or a cycle?)` | A user `_iter_` that returns `self` (or forms an `_iter_` cycle), so iteration re-dispatches without bottoming out | Return a genuine iterable (a List / built-in iterator), not `self` |
| `'<Class>' _str_ recurses too deeply` | A user `_str_` that returns `self` (or forms a stringification cycle), so `str()`/print re-dispatches without bottoming out | Return a plain String from `_str_`, not `self` |

### Resource guards (repetition / padding / range)

| Message | Cause | Fix |
|---|---|---|
| `repeated String/List/Bytes too large` | `*` repetition exceeding the cap | Use a smaller count |
| `replace result too large` / `join result too large` | `String.replace`/`String.join` output exceeding the 256 MiB cap (memory-amplification guard) | Reduce the input/replacement size |
| `range too large` | A `range` span exceeding the cap | Use a smaller range |
| `product` / `permutations` / `combinations`: `result too large (> <N> …)` | An `itertools` combinator whose eager result would exceed the ~10M-element cap (e.g. `permutations(range(13))`) | Reduce the input size / narrow `r` |
| `range step cannot be zero` | `range(a, b, 0)` | Use a non-zero step |
| `range() got multiple values for '<start/stop/step>'` / `range() got an unexpected keyword argument '<name>'` / `range() missing required argument 'stop'` / `range expects 1 to 3 positional arguments` / `range expects Integers` | A malformed `range()` call | Fix the `range` args |
| `<op> width too large` / `zfill width too large` / `format width/precision too large` | Padding/format width or precision exceeds the cap | Use a smaller width/precision |
| `<op> expects an Integer width` / `zfill expects an Integer width` | Non-Integer width to `ljust`/`rjust`/`center`/`zfill` | Pass an Integer width |
| `<op> fill must be a single character` | Multi-char fill to a padding method | Pass one character |
| `<op> result too large` | `ljust`/`rjust`/`center` whose multi-byte fill makes the padded result exceed the cap | Use a smaller width / a single-byte fill |

### String search & format methods

| Message | Cause | Fix |
|---|---|---|
| `substring not found` | `index`/`rindex` (throwing variants) with a missing substring | Use `find`/`rfind` (return -1) or check first |
| `empty separator` | `split`/`join`-family with an empty `sep` | Pass a non-empty separator |
| `unmatched '{' in format string` | Unbalanced brace in `.format()` | Balance braces / escape with `{{` |
| `format field must be an index` / `format index out of range` | Bad/oversized positional field in `.format()` | Use valid `{0}`-style indices |
| `'in <String>' requires a String, not '<T>'` | `x in <String>` with non-String `x` | Test with a String substring |
| `levenshtein expects a String or a List of Strings` / `levenshtein: the List must contain only Strings` | Bad arg to `.levenshtein` | Pass a String or a List of Strings |

### Format mini-spec (`format()` / f-strings)

| Message | Cause | Fix |
|---|---|---|
| `invalid format spec '<spec>'` | An unparseable format spec | Fix the mini-spec syntax |
| `format type '<c>' needs an Integer` / `format type '<c>' needs a number, not '<T>'` | Value-type mismatch for the spec type | Match the value type to the format type |
| `unknown format type '<c>'` | Unrecognized presentation type char | Use a valid type (`d/f/e/x/…`) |
| `format: sign not allowed in string format specifier` (also `'#'`/`'='`/`','` variants) | An illegal option for a string spec | Drop the option for string formatting |
| `format: precision not allowed for integer type '<c>'` / `format: ',' not allowed with format type '<c>'` | Illegal option for integer/type combos | Remove the disallowed option |
| `format spec must be a String` / `format expected 1 or 2 arguments` | Bad `format(value, spec)` call | Pass a String spec / correct the arity |

### Converters & constructors

| Message | Cause | Fix |
|---|---|---|
| `cannot convert String to Integer: '<s>'` / `cannot convert String to Float: '<s>'` | `Integer("x")`/`Float("x")` on a non-numeric string | Pass a parseable numeric string |
| `cannot convert '<T>' to Integer` / `cannot convert '<T>' to Float` | Converting an unsupported type | Convert a supported type |
| `cannot convert Float NaN/infinity to Integer` / `Float is out of Integer range` | `Integer(nan/inf/huge float)` | Guard non-finite / out-of-range floats |
| `cannot round NaN/infinity to Integer` / `rounded value out of Integer range` | `round()` of a non-finite/huge value | Guard the value |
| `round ndigits must be an Integer` / `round expects a number` | Bad `round` args | Pass a number (and Integer ndigits) |
| `abs expects a number` | `abs()` of a non-number | Pass a number |
| `List() / Set() argument must be iterable` / `Dict() argument must be iterable of pairs` | Non-iterable to a collection constructor | Pass an iterable (of pairs for Dict) |
| `ord expects a String` / `ord expects a single character` | Bad `ord()` arg | Pass a single-char String |
| `chr expects an Integer` / `chr argument out of Unicode range` / `chr argument is a UTF-16 surrogate (not a valid scalar code point)` | Bad `chr()` code point | Pass a valid scalar code point |
| `enumerate() start must be an Integer, got <T>` / `sum start must be a number` / `sum expects numbers` | Bad builtin start/element types | Pass the expected types |
| `<who>() arg is an empty sequence` (min/max) | Empty `min`/`max` with no `default=` | Provide `default=` or non-empty input |
| `isinstance second argument must be a class, a built-in type, or a type-name String` | Bad 2nd arg to `isinstance` | Pass a type/class/type-name |
| `hasattr: name (2nd argument) must be a String` | Non-String name to `hasattr` | Pass a String name |
| `pow exponent must be non-negative with a modulus` / `pow modulus must be non-zero` / `pow modulus must be positive` | Bad 3-arg modular `pow` | Non-negative exponent, positive non-zero Integer modulus |
| `pow base/exp/mod must be Integer with 3 args` | A non-Integer base, exponent, or modulus in 3-arg `pow` | Use Integers for modular `pow` |

### Imports

| Message | Cause | Fix |
|---|---|---|
| `import expects a String module name` | A non-String argument to `import` | Pass the module name as a String |
| `no module named '<name>'` | Import target not found on the path | Check the name / add its dir with `--lib`/`KIRITO_PATH` |
| `circular import detected: <a> -> <b> -> <a>` | Re-entrant import of an in-progress module | Break the import cycle |

### Bytes — encode / decode / hex / construction

| Message | Cause | Fix |
|---|---|---|
| `'in <Bytes>' requires an Integer byte or a Bytes, not '<T>'` | Wrong `in` operand for Bytes | Test an Integer byte or a Bytes |
| `can only concatenate Bytes to Bytes, not '<T>'` / `can only repeat Bytes by an Integer` | Bad `+`/`*` operand for Bytes | Use Bytes / an Integer count |
| `'<enc>' codec can't encode code point U+<hex>` | `str.encode(enc)` code point unrepresentable in the target codec | Use utf-8 or a compatible codec |
| `'utf-8' codec can't decode: invalid UTF-8 byte sequence` / `'<enc>' codec can't decode byte <hex>` | `bytes.decode(enc)` on invalid bytes | Use the correct encoding / valid data |
| `unknown encoding: '<enc>'` | Unsupported encoding name | Use utf-8 / latin-1 / ascii |
| `fromhex: odd-length hex string` / `fromhex: non-hex digit` | Bad `fromhex` input | Pass an even-length hex string |
| `Bytes() expects a List of Integers, an Integer, a String, or Bytes` / `Bytes() list elements must be Integers` / `Bytes() element out of range (0..255)` | Bad `Bytes(x)` construction | Pass valid 0–255 Integers / supported types |
| `Bytes too large` | `Bytes(n)` with an oversized n | Use a bounded count |
| `negative count` | `Bytes(n)` with a negative n (e.g. `Bytes(-1)`) | Pass a non-negative count |

### Class dunder / super protocol

| Message | Cause | Fix |
|---|---|---|
| `'<class>' object is not callable` | Calling an instance whose class lacks `_call_` | Define `_call_` |
| `'<class>' _call_ does not accept keyword arguments` | Keyword args to a `_call_` that can't take them | Call positionally |
| `'<class>'._bool_ must return a Bool, got '<T>'` / `._hash_ must return an Integer` / `_len_ must return a (non-negative) Integer` | A dunder returns the wrong type | Return the required type from the dunder |
| `unhashable type '<class>'` | `_hash_` absent (or opted out) when hashing an instance | Define `_hash_` |
| `_super_() called in '<class>' …` (no base) | `self._super_()` where the class has no parent | Only call `_super_` from a subclass |
| `base class must be a class, got <T>` | `class D(x):` where the base `x` is not a class value | Inherit from a class |

## Standard library

### io — file open & modes

| Message | Cause | Fix |
|---|---|---|
| `open expected 1 or 2 arguments` | `io.open` called with 0 or >2 args | Pass `open(path[, mode])` |
| `open path must be a String` / `open mode must be a String` | Non-String path/mode to `open` | Pass Strings |
| `open: embedded NUL byte in path` | A String path containing a `\0` (the OS truncates at it — a validation bypass) | Remove the NUL byte |
| `unsupported file mode '<mode>'` | Mode not in the r/w/a[+][b] set | Use `"r"`/`"w"`/`"a"`/`"r+"` + optional `b` |
| `could not open file '<path>'` | OS could not open the path (missing dir, permission) | Check the path exists / is accessible |

### io — read / write on streams

| Message | Cause | Fix |
|---|---|---|
| `I/O operation on closed file: <path>` | read/write/seek after `close()` | Reopen or keep the handle open |
| `file not open for writing: <path>` / `file not open for reading: <path>` | Read/write against the wrong mode | Open with a matching mode |
| `write failed: <path>` | Underlying stream write failed (disk full, etc.) | Check the device/permissions |
| `stream is not writable` / `stream is not readable` | Read/write on a one-way stream | Use a stream of the right direction |
| `stdin is not writable` / `a write stream is not readable` | Writing to stdin / reading from stdout/stderr | Direct I/O to the correct std stream |
| `<who> expects a String or Bytes` | A non-String/Bytes value to `write`/`writelines`/`BytesIO.write` (e.g. `f.write(123)`) | Write a String or Bytes (convert with `String(x)`) |

### io — seek & BytesIO

| Message | Cause | Fix |
|---|---|---|
| `seek expects an offset` | `seek()` with no offset | Pass an integer offset |
| `seek: whence must be 0 (set), 1 (cur), or 2 (end)` | Out-of-range `whence` | Use 0, 1, or 2 |
| `seek: resulting position is out of range` / `seek: resulting position is negative` | Offset over/underflows the stream | Seek to a valid non-negative position |
| `BytesIO too large` | The in-memory buffer grew past the 256 MiB cap (via `write` or a `truncate` that extends after a large `seek`) | Stream to a file instead |
| `BytesIO expects a byte String` | Non-String passed to `BytesIO(...)` | Pass a (byte-transparent) String |

### path — pure paths & queries

| Message | Cause | Fix |
|---|---|---|
| `join expected at least one path component` | `path.join()` with zero args | Pass ≥1 component |
| `path: embedded NUL byte in path` | A filesystem query/mutation on a String path containing a `\0` (the OS truncates at it) | Remove the NUL byte |
| `getsize: <reason>` | `getsize` on a missing/non-regular path | Ensure the file exists |

### path — mutation (strict by default)

| Message | Cause | Fix |
|---|---|---|
| `mkdir: '<p>' already exists (pass exist_ok = True to ignore)` | `mkdir` on an existing dir | Pass `exist_ok = True` |
| `remove: '<p>' does not exist (pass missing_ok = True to ignore)` | `remove` of an absent target | Pass `missing_ok = True` |
| `rmtree: '<p>' does not exist (pass missing_ok = True to ignore)` | `rmtree` of an absent target | Pass `missing_ok = True` |
| `mkdir/remove/rmtree/rename … : <reason>` | The OS op failed (permission, parent missing, is-a-dir) | Fix permissions / use the right op |

### sys — environment & process

| Message | Cause | Fix |
|---|---|---|
| `setenv failed for '<name>'` | OS rejected the env-var set | Use a valid name/value |
| `createprocess: args must be a (non-empty) List of Strings (the program and its arguments)` | Bad/empty argv | Pass a non-empty List of Strings |
| `<ProcError message>` (e.g. `process timed out`, `failed to start '<argv0>'…`) | `createprocess`/`shell` spawn failed or the `timeout` elapsed — **ProcError** re-wrapped as a `KiritoError` | Fix the command/cwd, or raise the timeout |
| `process produced more than 256 MiB of output (capture limit exceeded)` | A child's stdout/stderr exceeded the `kMaxCapture` bound — a catchable error, not a crash (the child is still drained so it can't deadlock) | Have the child write to a file, or filter/limit its output |

### time — DateTime construction & methods

| Message | Cause | Fix |
|---|---|---|
| `DateTime: epoch <secs> is out of representable range` / `datetime: timestamp out of representable range` | Instant outside the representable window | Use an in-range instant |
| `datetime: cannot convert NaN/infinity to a timestamp` | `datetime(NaN/inf)` | Pass a finite number |
| `make: date component out of range` | A `make(...)` field beyond ±2e9 | Use realistic calendar fields |
| `strptime: text does not match format` | Parse text/format mismatch | Fix the format string or the input |
| `add/sub expects 1 argument (seconds)` / `add/sub expects a number of seconds` | Bad arg to `add`/`sub` | Pass exactly one number |
| `add/sub: cannot convert NaN/infinity to Integer` / `add/sub: result out of Integer range` / `DateTime arithmetic overflow` | Non-finite/huge delta or epoch overflow | Pass a finite, in-range delta |
| `diff expects 1 argument (a DateTime)` / `diff expects a DateTime` | Bad arg to `diff` | Pass a DateTime |
| `format expects a String` | Non-String format to `format` | Pass a format String |
| `sleep: seconds must be a finite number` / `sleep: seconds too large (maximum 1e9)` | `time.sleep(inf/nan)` or an absurdly large duration | Pass a finite, in-range number of seconds |
| `DateTime _setstate_: <expected an Integer epoch / cannot re-initialize an established DateTime>` | Deserializing a corrupt DateTime, or re-`_setstate_`-ing an already-initialized one | Deserialize only trusted data |

### net — TCP sockets

| Message | Cause | Fix |
|---|---|---|
| `socket() failed: <err>` | OS socket creation failed | Resource exhaustion — retry |
| `port out of range: <p> (must be 0-65535)` | Connect/bind port out of range | Use 0–65535 |
| `could not resolve host` | DNS resolve failed (connect) | Check the hostname/DNS |
| `connect failed: <err>` | TCP connect failed | Check host/port/reachability |
| `bind: cannot resolve host '<host>'` / `bind failed: <err>` | Bind address unresolvable / port in use | Use a valid, free local address |
| `listen failed: <err>` / `accept failed: <err>` | listen/accept on the socket failed | Ensure the socket is bound / listening |
| `send expects a String or Bytes` | Non-String/Bytes to `send` | Pass String or Bytes |
| `send failed: <err>` / `recv failed: <err>` | Socket write/read failed | Peer closed — reconnect / handle EOF |
| `recv size must be non-negative` | Negative `recv(n)` | Pass n ≥ 0 |
| `recvall: received data exceeds the size limit` | `recvall` accumulated past the cap | Read in bounded chunks |
| `<op>: socket is closed` | Any op (send/recv/bind/detach/fileno/…) on a closed or detached socket — a second `detach()` or a `fileno()` on a dead socket lands here too | Don't reuse a socket after `close()`/`detach()` |
| `unknown address family '<s>' (expected 'inet' or 'inet6')` | Bad `family` to `socket()`/`socketpair()` | Use `"inet"` or `"inet6"` |
| `unknown socket type '<s>' (expected 'stream' or 'dgram')` | Bad `type` to `socket()`/`socketpair()` | Use `"stream"` or `"dgram"` |
| `sendto: datagram too large` | A `sendto` payload past the datagram size limit | Send a smaller datagram |
| `sendto: could not resolve host '<host>'` | DNS resolve failed for the `sendto` peer | Check the host/DNS |
| `sendto failed` / `recvfrom failed` | The UDP send/receive syscall failed | Check the socket state / peer reachability |
| `recvfrom size must be non-negative` | Negative `recvfrom(n)` | Pass n ≥ 0 |
| `shutdown: how must be 'read', 'write', or 'both'` | Bad `how` to `shutdown` | Use `"read"`/`"write"`/`"both"` |
| `setsockopt: unknown option '<opt>'` / `getsockopt: unknown option '<opt>'` | An option name outside the supported set | Use a documented option name |
| `<net op> failed: <err>` | An OS socket syscall failed (setsockopt/getsockopt/shutdown/setblocking/settimeout, the named convenience setters) | Check the socket state / the option value |
| `socketpair(<type>) failed` | The OS could not create the connected pair | Retry; check resource limits |
| `gethostbyname: cannot resolve '<host>'` | DNS lookup returned no address | Check the hostname/DNS |
| `getaddrinfo('<host>') failed` | Address resolution failed | Check the host/service arguments |

### net — URL parsing

| Message | Cause | Fix |
|---|---|---|
| `URL must start with http:// or https://` | Unsupported scheme | Prefix with `http://`/`https://` |
| `malformed IPv6 URL (missing ']')` / `(junk after ']')` | Malformed `[…]` host | Use `[host]:port` |
| `invalid port in URL '<url>': '<portStr>'` / `port out of range in URL …` | Non-numeric / out-of-range URL port | Use a numeric port 1–65535 |
| `URL contains a control character` | A raw CR/LF/NUL-style control character embedded in the URL (a request-splitting guard) | Percent-encode or strip the control character |

### net — HTTP client & Response

| Message | Cause | Fix |
|---|---|---|
| `could not resolve host '<host>'` / `could not connect to <host>: <reason>` | DNS/connect failed during the exchange | Check the host/reachability |
| `header contains a control character (CR/LF): '<k>'` / `cookie contains a control character (CR/LF): '<name>'` | A request header/cookie name or value with an embedded CR or LF (a header/response-splitting injection guard) | Strip CR/LF from user-supplied header/cookie data |
| `multipart field name must not contain CR or LF` / `multipart filename must not contain CR or LF` | A `files=` field name or filename with an embedded CR/LF (a multipart header-injection guard) | Strip CR/LF from the field name/filename |
| `invalid gzip data` / `truncated gzip data` | The server's gzip body is bad/short | Server bug — retry / disable gzip |
| `<method>() expected at least <n> argument(s)` | Too few args to `get`/`post`/`request`/… | Pass the required args |
| `HTTP <status> <reason> for <url>` | `raiseforstatus()` on a ≥400 response | Handle the status yourself |
| `Response indexing takes a single string key` / `Response index must be a String key` / `Response has no field '<name>'` | Bad `resp[...]`/attribute access | Use `"status"`/`"body"` or a documented field |
| `header() expected at least 1 argument (the header name)` | `header()` with no name | Pass the header name |

### net — TLS (`KIRITO_ENABLE_TLS`)

| Message | Cause | Fix |
|---|---|---|
| `TLS handshake with <host> failed<why>` | Handshake failed (no CA, cipher, etc.) | Set `SSL_CERT_FILE` or pass `verify=False` |
| `TLS certificate verification failed for <host> (pass verify=False to skip)` | The peer cert didn't verify | Trust the CA or pass `verify=False` |
| `SSL_write failed` | TLS write error mid-request | Reconnect |
| `https requires building with KIRITO_ENABLE_TLS (OpenSSL); use http:// otherwise` | HTTPS on a non-TLS build | Rebuild with TLS or use http:// |
| `SSL_CTX_new failed` | OpenSSL could not create the TLS context | Environment/OpenSSL problem — retry |
| `starttls: only a stream (TCP) socket can use TLS` | `socket.starttls` on a UDP/datagram socket | TLS needs a stream socket |
| `starttls: this socket is already using TLS` | `starttls` called twice on the same socket | Upgrade once |
| `starttls: a server_hostname is required for certificate verification (or pass verify=False)` | `starttls(verify=True)` with no hostname to check (IP-only, never connected) | Pass `server_hostname=` or `verify=False` |
| `starttls: socket TLS requires building with KIRITO_ENABLE_TLS (OpenSSL); net.tlsenabled is False in this build` | `socket.starttls` on a non-TLS build | Rebuild with TLS (never silently plaintext) |
| `detach: cannot detach a TLS socket (the TLS session owns the fd)` | `detach()` on a TLS-active socket | Close it instead; the fd is owned by the TLS session |
| `HTTPS response exceeds the size limit` | A TLS response body grew past the cap | Fetch a smaller resource / stream it |

### random — RNG construction & sampling

| Message | Cause | Fix |
|---|---|---|
| `Random: unknown generator '<name>' …` | Bad `generator=` kwarg | Use `"xoshiro"` or `"mersenne_twister"` |
| `expected a number` / `uniform expects (a, b)` / `randint expects (a, b)` / `randrange expects 1 to 3 arguments` | Bad arity/type to a distribution | Pass the expected numeric args |
| `expovariate: lambda must be positive and finite` | λ ≤ 0 or a non-finite λ (a NaN slips past `≤`) | Pass a finite λ > 0 |
| `gauss: sigma must be non-negative` / `gauss: mu and sigma must be finite numbers` | A negative or non-finite param to `gauss`/`normalvariate` | Pass a finite `sigma ≥ 0` |
| `uniform: a and b must be finite numbers` | A NaN/inf bound to `uniform` | Pass finite bounds |
| `randint: empty range` / `randrange: empty range` / `randrange: step must not be zero` / `randrange: range too large to sample` | A degenerate integer range | Widen the range / use a non-zero step |
| `choice/choices from empty sequence/population` | Sampling an empty population | Provide items |
| `choices: k must be non-negative` / `choices: k too large` | Bad `k` for `choices` | Pass 0 ≤ k ≤ cap |
| `shuffle requires a List` | Non-List to `shuffle` | Pass a List |
| `sample expects (population, k)` / `sample: k out of range` | Bad `sample` args | Use an iterable + 0 ≤ k ≤ len |
| `seed expects a value` | `Random.seed()` called with no argument | Pass a seed value |
| `Random _setstate_: <malformed engine state / unknown generator '<kind>'>` | Deserializing a corrupt/foreign Random blob | Deserialize only trusted `dump`/`serialize` output |
| `choice`/`choices`/`sample` `expects a(n) (iterable) sequence/population` | A non-iterable population to `choice`/`choices`/`sample` | Pass a sequence/iterable population |
| `randombytes: count must be non-negative` / `randombytes: count too large` (also `randomhex`/`randomurlsafe`) | A negative or over-cap `n` to a secure-random helper | Pass `0 ≤ n ≤` cap |
| `randombelow: n must be positive` | `randombelow(n)` with `n ≤ 0` | Pass `n ≥ 1` |
| `randombytes: OS secure random source unavailable` / `randombelow: OS secure random source unavailable` | The kernel CSPRNG (getrandom / `/dev/urandom` / BCrypt) failed — no weak fallback is ever used | Fix the OS entropy source |

### math — domain & Integer requirements

| Message | Cause | Fix |
|---|---|---|
| `<fn>: math domain error (got <x>)` | A unary `math` fn outside its domain (`sqrt`(x<0), `asin`/`acos`(abs>1), `acosh`(x<1), `atanh`(abs≥1), `log2`/`log10`(x≤0), `gamma`/`lgamma` at a non-positive integer) | Restrict the argument to the domain (a `NaN` passes through) |
| `fmod: math domain error (divisor is zero)` / `(infinite dividend)` | `math.fmod(x, 0)`, or `math.fmod(inf, y)` (libm would return a silent `NaN`) | Use a finite dividend and a non-zero divisor |
| `log: math domain error (argument must be > 0)` / `(base must be > 0 and != 1)` | `math.log` with x≤0 or a bad base | Positive argument / valid base |
| `pow: math domain error (a negative base requires an integer exponent)` / `(zero to a negative power)` | `math.pow(-2, 0.5)` / `math.pow(0, -1)` | Integer exponent for a negative base (or use `complex`) |
| `<who>: cannot convert NaN/infinity to Integer` / `result out of Integer range` | `floor`/`ceil` of a non-finite/huge value | Feed a finite, in-range value |
| `math function expected 1 argument` / `pow expected 2 arguments` / `atan2 expected 2 arguments` / `hypot expected 2 arguments` | Wrong `math` fn arity | Match the arity |
| `gcd/lcm expects Integers` / `factorial expects an Integer` / `comb/perm expects Integers` | A Float/other where an Integer is required | Pass Integers |
| `factorial is not defined for negatives` / `comb/perm require non-negative Integers` | A negative argument | Pass non-negative Integers |
| `prod expects an iterable` / `prod start must be a number` / `prod expects numbers` | Bad `math.prod` input | Pass an iterable of numbers |
| `<fn> result too large for Integer` | Overflow in `gcd`/`lcm`/`factorial`/`prod`/`comb`/`perm` | Result exceeds int64 (arbitrary precision is future work) |

### complex — scalar & analytic

| Message | Cause | Fix |
|---|---|---|
| `<who> expects a Complex or a number` | A non-numeric operand in Complex arithmetic / a module fn | Pass a Complex, Integer, or Float |
| `complex numbers are not ordered (no <, <=, >, >=)` | `<`/`>`/… on a Complex | Complex is unordered; compare magnitudes explicitly |
| `complex division by zero` | `z / 0` | Use a non-zero divisor |
| `complex pow: zero to a negative or complex power` | `0j ** -1` / `0j ** 1j` | Avoid the singularity |
| `complex.pow expects 2 arguments` | Wrong arity to `complex.pow` | Pass two arguments |
| `<fn>: math domain error (logarithm of zero)` / `(atanh of ±1)` / `(atan of ±i)` | `complex.log`/`log10` of 0, `atanh(±1)`, or `atan(±i)` (each a pole) | Keep the argument in domain |
| `math domain error (polar requires finite modulus and angle)` | `complex.polar` with a non-finite `r`/`theta` (would drive `std::polar` to silent NaN) | Pass finite arguments (a negative finite modulus is allowed) |
| `Complex does not support this operator` / `Complex does not support this unary operator` | An operator outside `+ - * / **` (or unary `-`) on a Complex | Use a supported operator |
| `Complex _setstate_: malformed state` | Deserializing a corrupt Complex blob | Deserialize only trusted data |

### matrix / ComplexMatrix

| Message | Cause | Fix |
|---|---|---|
| `Matrix/ComplexMatrix index must be Integer` / `index out of range` / `index needs 1 (row) or 2 (element) indices` | Bad `m[...]` index | Use `m[i]` or `m[i, j]` within bounds |
| `Matrix/ComplexMatrix element assignment needs two indices: m[i, j] = v` | Assigning with ≠2 indices | Use `m[i, j] = v` |
| `Matrix/ComplexMatrix +/- requires matrices of equal shape` / `multiply: inner dimensions differ` | Shape/dimension mismatch | Match shapes / conform inner dims |
| `Matrix/ComplexMatrix too large` | Element count exceeds the cap (~16M) | Reduce the dimensions |
| `determinant/inverse/trace requires a square Matrix/ComplexMatrix` | A square-only op on a non-square matrix | Use a square matrix |
| `dot/cross expects a … vector` / `dot requires vectors of equal length` / `cross is only defined for two 3-element vectors` | Malformed vector operands | Use conforming 1×n / n×1 vectors |
| `Matrix rows must have equal length` / `Matrix expects a nested list …` / `… dimensions must be non-negative` | Bad constructor/factory input | Pass a rectangular nested list / valid dims |
| `Matrix _setstate_: negative dimension` / `… malformed state` / `… data size does not match shape` | Corrupt/hostile serialized Matrix state (a negative dim would otherwise build a garbage matrix) | Deserialize only trusted data |
| `Matrix() got an unexpected keyword argument '<name>'` / `got multiple values for 'rows'` | Bad keyword to `Matrix()` | Use `rows=`/`cols=` once each |
| `Matrix/ComplexMatrix does not support this operator` | An operator past `+ - *` on a matrix | Use a supported matrix operator |
| `compare expects a Matrix/ComplexMatrix` | `.compare(other)` with a non-matrix `other` | Pass a matrix of the same kind |
| `Matrix(rows, cols) needs both rows and cols` | A dimension-form `Matrix()` with only one of rows/cols | Pass both `rows` and `cols` |
| `ComplexMatrix _setstate_: element must be [re, im]` | Deserializing a corrupt ComplexMatrix blob | Deserialize only trusted data |

### tensor

The tensor engine throws **`TensorError`** (a `std::runtime_error`) internally; every `stdlib_tensor.hpp`
call site re-wraps it as a `KiritoError`, so the messages below surface as ordinary catchable errors.

| Message | Cause | Fix |
|---|---|---|
| `tensors are not broadcastable to a common shape` | Element-wise op on non-broadcastable shapes | Make shapes broadcast-compatible |
| `reshape: total number of elements must be unchanged` | `reshape` to an incompatible size | Preserve the element count |
| `matmul: inner dimensions differ` / `matmul requires tensors of rank >= 2` | Bad `matmul` operands | Conform dims; rank ≥ 2 |
| `permute: axes count must equal the tensor rank` / `permute: axes must be a permutation` | Bad `permute`/`transpose` axes | Pass a full valid permutation |
| `<op> axis out of range` | Any axis arg past `ndim` (reductions, `slice`, `squeeze`, `stack`, `tensordot`, …) | Axis within `[0, ndim)` |
| `Tensor index must be Integer` / `Tensor index out of range` / `too many indices for tensor` | Bad index / assignment key | One in-range Integer per dimension |
| `cannot index/slice a 0-D tensor` / `item() requires a tensor with exactly one element, got <n>` | Indexing a scalar / `.item()` on a multi-element tensor | Use `.item()` on a 0-D / reduce first |
| `boolean mask must match the tensor shape` | `t[mask]` with a wrong-shaped mask | Match the mask shape |
| `tensor division/modulo/floor-division by zero` / `slice step cannot be zero` | A zero divisor / zero step | Use a non-zero divisor / step |
| `tensor <fn>: math domain error (got <x>)` / `tensor pow: math domain error …` / `tensor clip: lower bound <lo> …` | Out-of-domain element math | Keep elements in domain / order clip bounds |
| `Tensor dtype must be "Float" or "Complex"` | Unknown `dtype=` string | Use `"Float"` or `"Complex"` |
| `this math op is Float-only on tensors …` / `pow`/`**`/`%`/`//` `is Float-only on tensors` / `<who>: Float tensors only` | A Float-only op on a Complex tensor | Use a Float tensor / the `complex` module |
| `ordering comparisons are not defined for Complex tensors` / `min/max is not defined for this dtype` | Ordering a Complex tensor | Complex is unordered — use a Float tensor |
| `backward: gradients are Float-only` / `does not require grad` / `the seed gradient shape must match` / `a seed gradient is required for a non-scalar tensor` | Misuse of `.backward()` | Float leaf, `requiresgrad=True`, matching seed |
| `gradients are Float-only (a Complex tensor cannot require grad)` / `cannot serialize a Tensor that requires grad; call detach() first` | Grad on a Complex tensor / serializing a grad tensor | Float tensors only; `.detach()` before serializing |
| `mean/min/max/median of an empty tensor` / `zero-size reduction …` / `std/var: not enough elements for the given ddof` | Reduction over an empty/degenerate axis | Reduce a non-empty tensor |
| `determinant/inverse/trace requires a square 2-D tensor` / `matrix is singular (no inverse)` / `matrix contains a non-finite value` | Bad linalg operand | Square, invertible, finite matrix |
| `solve requires a square 2-D A` / `solve: A and B shapes are incompatible` / `outer/kron/cross requires …` | Malformed linalg operands | Match the required shapes |
| `tensordot: …` / `einsum: …` | Malformed `tensordot`/`contract`/`einsum` axes or subscripts | Give matching, in-range specs |
| `Tensor too large` | Element count exceeds `kMaxElems` (~64Mi) | Reduce the size |
| `Tensor shape dimensions must be non-negative` / `Tensor: nested list is ragged …` / `Tensor: too many dimensions (max 64)` | Bad construction input | Use a rectangular nested list, ≤64 dims |
| `eye/linspace: … must be non-negative` / `arange step must be non-zero` / `arange: missing stop` | Bad factory arguments | Pass valid factory args |
| `split: axis length is not divisible …` / `repeat count must be non-negative` / `searchsorted: the first tensor must be 1-D and sorted` | Bad structural-op args | Match the op's requirements |
| `<op> expects a Tensor` / `Tensor does not support this operator (use .matmul for matrix products)` / `Tensor does not support this unary operator` | Wrong operand type / unsupported operator | Pass Tensor(s); use `.matmul`; only `-` is defined |
| `Tensor _setstate_: malformed state` / `_setstate_: complex element must be [re, im]` / `tensor data size does not match its shape` | A corrupt/hostile serialized tensor blob | Deserialize only trusted data |
| `cannot squeeze an axis whose size is not 1` | `squeeze` of an axis whose length is not 1 | Squeeze only size-1 axes |
| `concatenate: tensors must have the same rank` / `concatenate: shapes differ off the join axis` | Mismatched inputs to `concatenate` | Match rank and the non-join dimensions |
| `concatenate/stack needs at least one tensor` | `concatenate`/`stack` with an empty input list | Pass at least one tensor |
| `broadcastto: cannot broadcast to the requested shape` | `broadcastto` target incompatible with the source shape | Use a broadcast-compatible shape |
| `dot requires two 1-D tensors` / `dot requires vectors of equal length` | Bad operands to tensor `dot` | Pass two equal-length 1-D tensors |
| `take: tensor has no axes` | `take` on a 0-D tensor | Take from a tensor with ≥ 1 axis |
| `split: section sizes must sum to the axis length` | Explicit `split` sections don't cover the axis | Make the sizes sum to the axis length |

### json — parse & stringify

| Message | Cause | Fix |
|---|---|---|
| `JSON parse error: unexpected character` / `invalid literal` / `invalid number` | `json.parse` hit a malformed token | Give well-formed JSON |
| `JSON parse error: trailing characters after JSON value` | Extra non-whitespace after the top-level value | Parse one value; strip trailing junk |
| `JSON parse error: nesting too deep` | Arrays/objects nested past 1000 | Flatten the structure |
| `JSON parse error: expected string key` / `expected ':'` / `expected ',' or '}'` / `expected ',' or ']'` | Malformed object/array structure | Fix the separators/keys |
| `JSON parse error: unterminated string` / `bad escape` / `bad \u escape` / `invalid \u escape …` / `invalid low surrogate …` | Malformed string/escape | Fix the string escaping |
| `cannot serialize a cyclic structure to JSON` / `structure too deeply nested to serialize to JSON` | `json.stringify` hit a cycle / >1000 depth | Break the cycle / flatten |
| `cannot serialize '<T>' to JSON` | A Set/Function/native in the graph | Convert to JSON-native types (List/Dict/scalars) |
| `json.stringify: indent too large (maximum 100)` | A huge `indent=` to `stringify` (would overflow/OOM the pad) | Use an indent ≤ 100 |

### serialize / dump — text (KSER1) & binary (KDMP)

`serialize` (text) and `dump` (binary) share one graph walk, so most errors are common to both.

| Message | Cause | Fix |
|---|---|---|
| `structure too deeply nested to serialize` / `… to dump` | Graph deeper than the guard (10000; 1500 under sanitizers) | Flatten the structure |
| `cannot serialize/dump type '<T>' (define _getstate_/_setstate_ to make it serializable)` | An instance with no `_getstate_` / a live-resource native | Add `_getstate_`/`_setstate_`, or exclude it |
| `cannot serialize/dump type '<T>'` | A non-serializable kind (Socket, open file, Regex) | Exclude the resource from the graph |
| `cannot serialize/dump a native/built-in function '<name>' (only Kirito-defined functions are serializable; a module reconnects by import)` | A bound reference to a builtin/native function (e.g. `var f = math.sqrt`) in the graph | Serialize a Kirito `Function` wrapper, or re-`import` the module on load |
| `cannot serialize/dump this function: its source text was not captured …` | A `Function` literal defined inside an f-string (its source isn't recorded) | Define the function as a top-level/`var` binding, not inside an f-string |
| `cannot serialize/dump class '<name>': its source text was not captured` | A class whose defining source wasn't recorded (should not occur for normally-defined classes) | Define the class normally |
| `serialized root/child id out of range` / `truncated/unexpected end …` / `corrupt … : <what>` | Corrupt/truncated serialized blob | Re-dump; deserialize only trusted data |
| `cannot deserialize: class '<name>' is not defined in this VM` | An instance tag names a user class that neither travels in the blob nor is defined here | Deserialize output produced by this build; the class usually travels with the instance now |
| `cannot deserialize function: <reason>` / `cannot deserialize class '<name>': <reason>` | A serialized function/class re-parse or re-run failed (corrupt/foreign source, or a free variable that can't be re-bound) | Deserialize only trusted `serialize`/`dump` output |
| `cannot deserialize: cyclic class-definition dependency (a base class or class-variable initializer forms a cycle)` | A blob whose classes have a definition-time cycle (impossible for validly-serialized source) | Deserialize only trusted data |
| `cannot deserialize: a free-variable name is not a String` | A corrupt function/class record whose free-variable name slot isn't a String | Deserialize only trusted data |
| `cannot deserialize '<name>': no class or registered deserializer in this VM` | A stateful native tag with no factory | `vm.registerDeserializer(name, …)` |
| `cannot deserialize '<name>': it defines _getstate_ but no _setstate_` | Class can serialize but not restore | Add `_setstate_` |
| `bad serialization header` / `bad dump header` / `unsupported dump version` | Wrong/foreign format header | Feed real `serialize.dumps`/`dump.dumps` output |
| `loads expects a Bytes (or String) of dump data` | `dump.loads` given the wrong type | Pass the `dumps` Bytes |
| `could not open file for saving` / `could not open file for loading` | `save`/`load` couldn't open the path | Check the path/permissions |
| `bad serialization tag '<t>'` / `bad dump tag` | A corrupt blob carries an unrecognized type tag | Deserialize only trusted data |
| `cannot deserialize: instance attribute name is not a String` | A corrupt instance record with a non-String attribute name | Deserialize only trusted data |
| `cannot deserialize '<name>': missing state` | An instance/native tag whose state payload is absent | Deserialize only trusted data |

### regex

Compile rejections are thrown as **RegexError** inside the engine and re-wrapped as
`invalid regex: <detail>`; runtime group/replacement errors are plain `KiritoError`.

| Message | Cause | Fix |
|---|---|---|
| `invalid regex: unbalanced ')' …` / `missing ) …` / `unterminated character class` | Unbalanced grouping/class in the pattern | Balance the parentheses/brackets |
| `invalid regex: nothing to repeat` / `bad repetition bounds {<lo>,<hi>}` / `repetition count too large (max 1000)` | A misplaced/oversized quantifier | Fix the quantifier |
| `invalid regex: pattern nested too deeply` / `compiled pattern too large` | A pathological pattern | Simplify the pattern |
| `invalid regex: bad character range in class` / `trailing backslash …` / `incomplete \x/\u escape` / `escape value out of range …` | Malformed class/escape | Fix the class/escape |
| `invalid regex: escape is a UTF-16 surrogate (not a scalar code point)` | A `\u` escape naming a surrogate (e.g. `\uD800`) | Escape a valid scalar code point |
| `invalid regex: too many capture groups (max 1000)` | A pattern with more than 1000 capturing groups | Reduce the group count / use non-capturing `(?:…)` |
| `invalid regex: duplicate/empty/bad group name …` / `malformed named group …` / `unsupported (?...) group` | A malformed group construct | Use a supported group form |
| `invalid regex: backreferences are not supported …` / `named backreferences …` / `lookahead …` / `lookbehind …` | Backrefs/lookaround (rejected by design for linear time) | Restructure without them |
| `regex match exceeded its complexity budget …` | A many-capture-group pattern over a long input (cost is O(text × program × groups)) | Simplify the pattern or reduce capture groups |
| `no such group: <g>` / `group key must be an Integer index or a String name` | Bad group access on a Match | Use a valid group number/name |
| `invalid group reference <g> in replacement template` / `bad replacement: …` | Malformed `sub` template | Reference an existing group; fix the `\g<…>` |
| `sub replacement must be a String or a function` / `sub replacement function must return a String` | Bad `sub` replacement | Pass a String template / return a String |

### zlib / gzip

Both throw **DeflateError** internally, re-wrapped with a `zlib:` / `gzip:` prefix.

| Message | Cause | Fix |
|---|---|---|
| `zlib: zlib data too short` / `unsupported zlib compression method` / `checksum mismatch (corrupt data)` | Malformed/corrupt zlib stream | Provide a valid zlib stream |
| `zlib: unexpected end of deflate stream` / `invalid Huffman code` / `invalid block type` / `truncated stored block …` / `invalid length/distance symbol` / `distance too far back` / `bad dynamic lengths` | Corrupt/truncated DEFLATE data | Data is corrupt — re-fetch |
| `zlib: invalid stored block lengths` / `invalid repeat` | A corrupt DEFLATE stored-block header / code-length repeat | Data is corrupt — re-fetch |
| `zlib: invalid zlib window size` / `zlib preset dictionary is not supported` / `invalid zlib header check` | A malformed or unsupported zlib header (bad window/CMF-FLG check, preset dictionary) | Provide a standard zlib stream |
| `zlib: decompressed data exceeds the size limit` | Inflate output passed the 256 MiB guard (zip bomb) | Don't decompress the hostile stream |
| `zlib: deflate: input too large (max 2 GiB)` / `gzip: deflate: input too large (max 2 GiB)` | Compressing an input larger than 2 GiB (the LZ77 hash-chain index is 32-bit) | Compress ≤ 2 GiB, or split the input |
| `gzip: stream too short` / `bad magic (not a gzip stream)` / `unsupported compression method` | Not a valid gzip stream | Provide a real `.gz` stream |
| `gzip: truncated FEXTRA/header/stream` / `CRC-32 mismatch …` / `ISIZE mismatch …` | Corrupt/truncated gzip member | Data is corrupt/truncated |
| `gzip: trailing data after the last member` / `trailing data is not a valid gzip member` | Leftover bytes after the stream | Strip the trailing junk |

### hash

| Message | Cause | Fix |
|---|---|---|
| `unhashable type '<T>'` | `hash(x)` on a value with no hash (a List, or an instance lacking `_hash_`) | Pass a hashable value |
| `<fn> expects a String or Bytes` | A `hash`/`zlib`/`gzip` function given something other than a `String` or `Bytes` | Pass text or binary data |
| `hmac: unknown algorithm '<algo>'` | A bad `algo=` to `hmac` | Use `"sha256"`/`"sha512"`/… |
| `pbkdf2: iterations must be >= 1` / `dklen must be >= 1` / `dklen too large (max 1048576)` / `unknown algorithm '<algo>'` | A bad PBKDF2 parameter | Positive iterations, 1 ≤ dklen ≤ 1 MiB, a known algo |

### crypto (`KIRITO_ENABLE_TLS`)

OpenSSL-gated. On a non-TLS build every function throws the first row below; branch on `crypto.enabled`.

| Message | Cause | Fix |
|---|---|---|
| `crypto.<fn> requires building with KIRITO_ENABLE_TLS (OpenSSL)` | Any `crypto` function on a non-TLS build (`crypto.enabled` is `False`) | Rebuild with `-DKIRITO_ENABLE_TLS`, or branch on `crypto.enabled` |
| `aes: key must be 16, 24 or 32 bytes (AES-128/192/256)` | A wrong-length AES key | Use a 16/24/32-byte key |
| `aesencrypt: iv must be non-empty` / `aesdecrypt: iv must be non-empty` | An empty IV | Pass a unique per-message IV (12 bytes standard) |
| `aesdecrypt: tag must be 16 bytes` | A truncated/oversized GCM tag | Pass the full 16-byte tag `aesencrypt` returned |
| `aesdecrypt: authentication failed (wrong key/iv/tag or tampered data)` | The GCM tag didn't verify — wrong key/iv/aad or tampered ciphertext | Never trust the output; discard it |
| `rsagenerate: bits must be in [512, 16384]` | An out-of-range RSA key size | Use e.g. 2048/3072/4096 |
| `crypto: unknown hash '<algo>'` | A bad `algo=` to `rsasign`/`rsaverify`/`ecsign`/`ecverify` | Use `"sha256"`/`"sha384"`/`"sha512"` |
| an OpenSSL error string | A malformed PEM key, unknown curve, or unparseable certificate to `rsa*`/`ec*`/`x509parse` | Pass a valid PEM key / curve name / certificate |

### int — arbitrary-precision integers

`BigInt` errors `throw` a plain-String message (catch with a bare `catch` or `catch String as e`).

| Message | Cause | Fix |
|---|---|---|
| `int: base must be between 2 and 36` / `fromstring: base must be between 2 and 36` | A radix outside 2..36 | Use base 2–36 |
| `int: invalid integer literal '<s>'` | Non-numeric text (for the chosen base) to `BigInt`/`fromstring` | Pass valid digits |
| `BigInt expects an Integer, a String, or a BigInt` | `BigInt(x)` on an unsupported type | Pass an Integer/String/BigInt |
| `int: number too large (exceeds size limit)` / `int: pow result too large …` / `int.pow: exponent too large` / `pow: exponent too large` | A BigInt op would exceed the `kMaxLimbs` guard (runaway mul/pow/factorial) | Reduce the magnitude/exponent |
| `int.pow: negative exponent (use ** for a Float, or modpow for modular)` | A negative exponent to `int.pow` | Use `**` (Float) or `modpow` |
| `integer division by zero` / `integer modulo by zero` / `division by zero` | `//` / `%` / `/` a BigInt by zero | Guard the divisor |
| `modpow: modulus is zero` / `modpow: negative exponent` | Bad `modpow` args | Positive modulus, non-negative exponent |
| `modinv: modulus must be >= 2` / `modinv: arguments are not coprime (no inverse exists)` | No modular inverse exists | Coprime operands, modulus ≥ 2 |
| `isqrt: negative operand` | `isqrt` of a negative | Pass n ≥ 0 |
| `factorial: not defined for negatives` / `comb: requires non-negative integers` / `perm: requires non-negative integers` | A negative to `factorial`/`comb`/`perm` | Pass non-negative integers |
| `int: OS secure random source unavailable (needed for primality/randomprime)` | `isprobableprime`/`randomprime` when the OS CSPRNG failed (deterministic `isprime` on small n still works) | Fix the OS entropy source |
| `isprobableprime: rounds must be >= 1` / `randomprime: rounds must be >= 1` / `randomprime: bits must be >= 2` / `randomprime: bits too large` | A bad Miller-Rabin `rounds` or `randomprime` `bits` | rounds ≥ 1, bits ≥ 2 within the cap |
| `BigInt does not support this operator` / `BigInt does not support this unary operator` | A right-operand-only or unsupported op — BigInt dispatches on the **left** operand, so `3 + BigInt(2)` throws while `BigInt(2) + 3` works | Put the BigInt on the left, or convert |
| `toint: value does not fit in a native Integer` | `.toint()` on a BigInt beyond int64 range | Keep it a BigInt / check `.bitlength()` |

### Standard library — Kirito-authored modules

The frozen-source modules (`itertools`, `functools`, `statistics`, `base64`, `heapq`, `enum`, `arg`,
`tabular`, `semver`, …) are written in Kirito and `throw` plain-String errors — caught by a bare `catch`
or `catch String as e`. (Their combinator size caps — `product`/`permutations`/`combinations` *result too
large* — live under [Resource guards](#resource-guards-repetition--padding--range) above.)

| Message | Cause | Fix |
|---|---|---|
| `itertools.count needs a stop bound (no lazy generators)` | `count()`/`count(start)` with no stop (Kirito has no lazy generators) | Pass a stop bound |
| `itertools.count step must not be zero` | `count(start, 0)` | Use a non-zero step |
| `step for islice() must be a positive integer` | `islice(xs, start, stop, step)` with `step ≤ 0` | Use a positive step |
| `reduce of empty sequence with no initial value` | `functools.reduce(f, [])` without an initializer | Pass an initial value, or a non-empty sequence |
| `mean requires at least one data point` / `median requires at least one data point` / `pvariance requires at least one data point` | `statistics` central-tendency on an empty list | Provide data |
| `variance requires at least two data points` / `quantiles requires at least two data points` | `statistics.variance`/`quantiles` on fewer than two points | Provide ≥ 2 points |
| `no mode for empty data` | `statistics.mode([])` | Provide data (or use `multimode`, which returns `[]`) |
| `quantiles: n must be at least 1` | `statistics.quantiles(data, n)` with `n < 1` | Use `n ≥ 1` |
| `invalid base64 character: '<ch>'` / `invalid base64: a lone trailing character (invalid length)` / `invalid base64: truncated or corrupted input` | `base64.decode` of malformed input | Decode only valid base64 |
| `heappop from empty heap` / `heapreplace on empty heap` | `heapq.heappop`/`heapreplace` on an empty list | Check the heap is non-empty |
| `duplicate enum member: <name>` | Two members with the same name passed to `enum.Enum` | Use unique member names |
| `no such enum member: <name>` | `e.nameof(v)` / member lookup for an unknown value | Look up an existing member |
| `unknown option: --<name>` / `unknown option: -<c>` | `arg.Parser.parse` given a flag/option the parser doesn't declare | Declare the option, or drop it |
| `missing required argument: <<name>>` | A required positional not supplied on the command line | Supply the positional |
| `option --<name> expects an integer, got '<v>'` / `expects a number, got '<v>'` | An option whose default is Integer/Float given an unconvertible value | Pass a value of the option's type |
| `option --<name> requires a value` / `option <token> requires a value` | A value-taking option given at the end with no value | Supply the option's value |
| `Series: index length does not match values length` | Constructing a `Series` with mismatched index/values | Match the lengths |
| `Series: length mismatch (<a> vs <b>)` | Element-wise op between Series of different lengths | Align the Series first |
| `DataFrame: data must be a Dict of columns or a List of rows` | `DataFrame(x)` with an unsupported shape | Pass a column-Dict or a row-List |
| `DataFrame: all columns must have the same length` | Ragged column data | Make every column the same length |
| `DataFrame: new column length must match row count` | Assigning a column of the wrong length | Match the row count |
| `boolean mask length does not match row count` | Boolean-mask selection with a wrong-length mask | Match the mask to the frame |
| `merge: how must be one of inner/left/right/outer, got '<how>'` | Invalid `how` to `tabular.merge` | Use inner/left/right/outer |
| `readcsv: row <r> has <n> fields, expected <k>` | A CSV row with more fields than the header (no silent data loss) | Fix the row / header |
| `invalid version '<s>': need MAJOR.MINOR.PATCH` / `non-numeric core` / `empty prerelease identifier` / `invalid prerelease identifier '<p>'` / `empty build identifier` / `invalid build identifier '<b>'` / `invalid version (too many components): <rest>` | `semver.parse`/`clean` of a malformed version — a prerelease/build identifier must be non-empty and drawn from `[0-9A-Za-z-]` (`valid()` returns `None` instead of throwing) | Pass a valid semver, or probe with `valid()` first |
| `inc: release must be major/minor/patch, got '<release>'` | `semver.inc` with an unknown release part | Use major/minor/patch |

## Concurrency — the `parallel` module

`parallel` is only installed by the `ki` CLI; a bare embedded `KiritoVM` doesn't have it.

### spawn & serialization

| Message | Cause | Fix |
|---|---|---|
| `parallel.<op>: requires running under a KiritoDispatcher (…a bare embedded KiritoVM does not)` | `parallel` used on an embedded VM | Run under the `ki` CLI |
| `parallel.spawn: missing function argument` / `first argument must be a Kirito function (defined in a .ki file)` | Bad first arg to `spawn` | Pass a file-defined Kirito function |
| `parallel.spawn: the function must be defined in a loadable .ki file (its source is <src>)` | A closure / `<main>` / anonymous fn | Define it in an importable `.ki` file |
| `cannot dump type '<T>' (define _getstate_/_setstate_ to make it serializable)` | A non-serializable arg/queue item (socket, file, live regex) | Pass only serializable values |
| `parallel: worker thrown: <err>` | The spawned task threw in its worker VM | Fix the worker function |
| `parallel.spawn: cannot read source file '<f>'` / `could not load '<f>'` / `could not locate the spawned function in '<f>'` | The worker can't re-load the fn's source | Keep the `.ki` file on the lib path; don't relocate the fn |
| `parallel.spawn: cannot spawn from a module's top level …` (`put your spawn calls behind 'if argmain:'`) | `spawn` reached at import/module-top-level | Guard `spawn` calls with `if argmain:` |
| `parallel: the dispatcher is shutting down` | `spawn`/enqueue after shutdown began | Don't spawn during shutdown |
| `parallel: invalid task handle` | A `Task` method on a foreign/garbage handle | Use a `Task` returned by `spawn` |
| `parallel: corrupt spawn arguments` | The serialized spawn-argument blob is malformed | Pass only serializable args |

### blocking primitives (Queue / Lock / Event / Semaphore / Barrier)

| Message | Cause | Fix |
|---|---|---|
| `<op>: queue is closed` | put/get on a closed Queue | Don't use a closed Queue |
| `parallel: operation aborted (dispatcher shut down)` | A blocked op woken by shutdown | Expected on shutdown — catch it |
| `<op>: lock is not reentrant (already held by this worker)` | Re-acquiring a held Lock | Don't re-enter the lock |
| `<op>: barrier is broken` | Waiting on a broken Barrier | Recreate the Barrier |
| `<op>: timed out` / `parallel: timeout must be a number or None` | A blocking op exceeded / a bad `timeout` | Raise the timeout; pass a number or None |
| `parallel.Queue: put to a full queue` / `get from an empty queue` | Non-blocking put/get with no room/item | Block, or check `full`/`empty` first |
| `parallel.Lock: release of an unlocked lock` | `release()` without holding it | Only release what you hold |
| `parallel.Queue: maxsize must be >= 0` / `Semaphore: value must be >= 0` / `Barrier: parties must be >= 1` | A bad constructor argument | Pass a valid initial value |
| `parallel.<Type>: uninitialized …` / `cannot rebind (unknown id…)` | A method on an uninitialized/foreign primitive | Construct it in this dispatcher |
| `Queue.put: missing item` / `Queue.putnowait: missing item` | `put`/`putnowait` called with no item argument | Pass the item to enqueue |

## Kirito `throw` and `assert`

A Kirito `throw <value>` throws a **`KiritoThrow`** carrying the live value; `assert cond[, msg]`
lowers to the same when `cond` is falsy (value `"assertion failed"` or your message). A bare `throw`
inside a `catch` re-throws the in-flight value with its original span. A typed `catch T as e:` routes
on the value's class chain; a bare `catch as e:` catches anything. If one reaches the top level
uncaught, it is re-wrapped as `uncaught exception: <value>` (see the CLI table below).

## Embedding & extending — errors you can only hit from C++

Everything above surfaces from *Kirito* code. This section covers the throws you reach **only from the
C++ side** — driving a `KiritoVM`, holding `Handle`s/`Value`s, or writing a `NativeFunction`/
`NativeClass`. They are all `KiritoError` (so they're catchable in Kirito as a String and, on the C++
side, as `kirito::KiritoError` — or `std::exception`). See the [C++ API](cpp-api.html) for the types.

### Handle lifetime — the dangling handle

A `Handle` is a `{slot, generation}` reference into the VM's arena. Dereferencing one that no longer
names a live object throws — this is the guard that turns a use-after-free into a clean, catchable
error. It comes from `ObjectArena::at()` (via the `dangling()` helper).

| Message | Cause | Fix |
|---|---|---|
| `dangling handle (stale generation)` | The object was **swept by GC** (the slot was reused / its generation bumped) while you still held the handle — the classic *unrooted handle across an allocation*. The reserved sentinel `Handle{}` (generation 0) also lands here. | **Root** anything you hold across a call that can allocate: a stack-scoped `RootScope`, or a `PinnedHandle` for a value stored in a long-lived C++ object. See the C++ API's *rooting rule*. |
| `dangling handle (slot out of range)` | The handle's slot is outside this arena — e.g. a handle from a **different `KiritoVM`**, or a fabricated/default one. | Never mix handles between VMs (each VM owns its arena); only deref handles this VM produced. |

The arena's ABA guard (a slot is retired once its generation would wrap past 2³²) means a stale handle
can **never** silently re-validate against a recycled object — you always get this throw, not wrong data.

### Native functions — argument helpers (`native.hpp`)

When you write a `NativeFunction`/`NativeClass` method, the argument helpers throw `KiritoError` on
misuse — these become ordinary catchable Kirito errors at the call site.

| Message | Cause |
|---|---|
| `<who>() expected at least N argument(s), got M` | `requireArgs(args, N, "who")` — too few positionals |
| `<who> expects a String` / `<who> expects an Integer` | an `Args`/argument accessor got the wrong kind |
| `<name>() got an unexpected keyword argument '<k>'` | a signatured native/`makeMethod` received a keyword it doesn't declare |
| `<name>() got multiple values for argument '<k>'` | the same parameter was passed positionally **and** by keyword |
| `slice indices must be Integer or None` / `slice step cannot be zero` | a custom `slice()` slot given non-Integer bounds / a zero step |
| `alias target '<x>' not registered` | `ModuleBuilder::alias("new", "x")` where `x` was never registered |
| `<fn> missing argument N` | `Args::at(N)` (the C++ positional accessor) reaching past the arguments actually passed |

### The `Value` API — typed reads

The `Value` wrapper's typed accessors throw a `KiritoError` when the underlying value is the wrong kind,
so a native that peeks-then-wraps fails loudly instead of reading garbage.

| Message | Cause | Fix |
|---|---|---|
| `<who> expected <Type>, got '<actual>'` | `arg.asInt("who")` / `.asString(...)` / `.asDict()` … on a value of another kind | Peek first (`arg.isInt()`, `arg.isDict()`) before the typed read, or accept the type mismatch as the error |
| `unhashable type '<T>'` | using a non-hashable `Value` as a Dict key / Set element / `.hash()` target from C++ | Only hash hashable kinds (scalars, String, Bytes, frozen tuples-as-Lists are **not** hashable) |
| `uninitialised Value (no VM)` | calling a method on a default-constructed `Value{}` (no bound VM) | Construct values as `Value(vm, …)`; never operate on a `Value{}` |
| `Dict has no key '<k>'` | `Dict::at(k)` (the C++ throwing accessor) for a key not present | Check with `.contains(k)` / use `.get(k, dflt)` before `at` |

### The shared limits behind the guards

The resource and depth guards in the sections above surface identically to a C++ embedder. Two facts
worth knowing when you embed:

- **One 256 MiB ceiling.** The repetition/padding cap (`kMaxRepeat`, also bounding `String.replace`/
  `join` output), the inflate/zip-bomb cap (`kMaxInflateOut`), the network `recvall` cap
  (`kMaxRecvAll`), the in-memory `BytesIO` buffer, and the external-process output capture
  (`kMaxCapture`, for `sys.shell`/`createprocess`) all share the same 256 MiB bound — a single number to
  reason about for untrusted input.
- **Depth guards shrink under sanitizers.** The parser, VM call-depth, equality, and serde depth limits
  are lower under an ASan/TSan build (`KIRITO_SANITIZER_BUILD`), because instrumented native frames are
  larger. So a deeply-nested input that runs under `release` may hit a guard under `asan` — expected,
  not a regression.
- **`serialize`/`dump` `loads` is a trust boundary.** Deserializing rebuilds arbitrary **registered**
  classes and runs their `_setstate_` — like Python's `pickle`, it is **not safe on hostile bytes**.
  The format is bounds-checked against OOM/overflow, but only ever `loads()` data you produced or trust.

## The native boundary — how a C++ exception becomes catchable

The per-`Proto` interpreter loop (`bytecode_vm.hpp`) wraps native calls in a three-arm handler.
Order matters: `KiritoError` **is-a** `KiritoThrow`, so it is caught first.

| What crosses the boundary | Caught by | Catchable in Kirito? | What the user sees |
|---|---|---|---|
| A native/stdlib `KiritoError` (the normal diagnostic) | `catch (KiritoError&)` | Yes — routed to the innermost `try`; the value is the message as a **Kirito String** | `e.what()`, e.g. `list index out of range` |
| A Kirito `throw <value>` | `catch (KiritoThrow&)` | Yes — routed on the live value, so a **typed** `catch T as e` can match by class chain | the thrown value itself |
| Any *other* C++ `std::exception` (an escaped `std::bad_alloc`, `std::out_of_range`, …) | `catch (const std::exception&)` | Yes, but only by a **bare `catch`** (surfaced as a **Kirito String** = `e.what()`); a *typed* catch won't match | raw `e.what()`, e.g. `std::bad_alloc`, `stoi` |
| An engine subtype (`TensorError`/`RegexError`/`DeflateError`/`ProcError`) | Re-wrapped as a `KiritoError` in the stdlib glue *before* the boundary | Yes — behaves like the first row | the engine's `.what()`, sometimes prefixed (`invalid regex: …`, `zlib: …`) |

## Standard C++ exceptions

Each is ultimately caught by the boundary `catch (const std::exception&)` (→ a catchable Kirito
String) unless re-wrapped as a `KiritoError` sooner.

| Message (`.what()` shape) | Cause | Fix |
|---|---|---|
| **std::bad_alloc** `std::bad_alloc` | An allocation the resource guards didn't bound exhausts memory (huge native structure, deep native recursion) | Reduce the size; rely on the guarded paths (`range`/repetition/`Tensor too large` caps) |
| **std::length_error** | A container grown past its implementation max | Cap the size before building; use the guarded builders |
| **std::out_of_range** → re-wrapped `format index out of range` | `stoull` on a `{N}` format field whose index overflows | Use a valid, in-range positional index |
| **std::out_of_range** `stoi` / `basic_string::at` (bare) | `stoi`/`stod`/`.at()` overflow inside a native that didn't wrap it | Pass in-range input (most sites already re-wrap into a clean message) |
| **std::invalid_argument** → re-wrapped `cannot convert String to Integer/Float: '<s>'` | `Integer("x")`/`Float("x")` on a non-numeral | Pass a well-formed numeric literal |
| **std::system_error** | The OS refuses to create a thread/lock (resource limit) in `parallel`/`proc_compat` | Lower the concurrency/worker count; retry under less load |
| **std::filesystem_error** `filesystem error: <op>: <path>` | A `path`/`io` op fails at the OS layer in a way not pre-checked (permission, race) | Check permissions/existence; use `exist_ok`/`missing_ok` |
| **std::bad_variant_access** | An internal `std::variant` accessed on the wrong alternative | internal invariant — not user-triggerable |

## Top-level CLI handling & exit codes

From `main.cpp`. The `parallel` dispatcher stringifies worker `std::exception`s into data first, so
cross-VM errors arrive on the main VM as values, not live throws.

| Scenario | Output | Exit code |
|---|---|---|
| Uncaught `KiritoError` escapes | The call-chain traceback, then `<file>:<line>:<col>: error: <what()>` to stderr (`<file>` prefers the imported module) | `1` |
| Uncaught `std::exception` escapes | `<file>: error: <what()>` to stderr (deliberately avoids `std::terminate`) | `1` |
| Uncaught Kirito `throw <value>` reaches the top | `<file>:<line>:<col>: error: uncaught exception: <value>` + traceback | `1` |
| The same errors inside the **REPL** | `error: <what()> (line <L>:<C>)` / `error: <what()>`; the loop continues | REPL always `0` on exit |
| CLI usage problem | `ki: unknown option '<arg>'` / `ki: --lib needs a directory` | `2` |
| Script file cannot be opened | `ki: cannot open '<file>'` | `1` |
| Successful run | — | `0` |

