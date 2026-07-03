# Bonus Lesson 4 — Matrices, Vectors & Complex Numbers

Kirito ships three native (C++) numeric libraries: `matrix` (real matrices) and `complex` (complex
numbers **and** complex matrices) — both covered in this lesson — plus
[`tensor`](bonus-05-tensors.html) (N-dimensional arrays), the subject of the next lesson. They share
one idea — arbitrary-shape dense data — and one ergonomic trick: `apply`, which maps a function over
every element at native speed.

## Real matrices

Build a matrix from a nested list (rows must be equal length), or with a factory:

```kirito
var io = import("io")
var m = import("matrix")

var A = m.Matrix([[1, 2], [3, 4]])
io.print(A)                 # [[1.0, 2.0], [3.0, 4.0]]   (elements are Floats)
io.print(A.shape())         # [2, 2]
io.print(A[0, 1])           # 2.0   — element access; A[0] is the whole row
io.print(m.zeros(2, 3))     # [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
io.print(m.identity(2))     # [[1.0, 0.0], [0.0, 1.0]]
```

Matrices are **arbitrary shape** — any rows × cols. Arithmetic checks dimensions and throws on a
mismatch; the square-only operations (`determinant`, `inverse`, `trace`) throw on a non-square
matrix.

```kirito
var io = import("io")
var m = import("matrix")
var A = m.Matrix([[1, 2], [3, 4]])
var B = m.Matrix([[5, 6], [7, 8]])

io.print(A + B)             # [[6.0, 8.0], [10.0, 12.0]]
io.print(A * B)             # [[19.0, 22.0], [43.0, 50.0]]   — matrix multiply
io.print(A * 2)             # [[2.0, 4.0], [6.0, 8.0]]       — scalar multiply
io.print(A.transpose())     # [[1.0, 3.0], [2.0, 4.0]]
io.print(A.determinant())   # -2.0
io.print(A.trace())         # 5.0
io.print(A * A.inverse() == m.identity(2))   # False — `==` is exact and inversion has roundoff
io.print((A * A.inverse()).compare(m.identity(2), abs_tol = 1e-9))   # True — tolerant compare
```

## Applying a function to every element — `apply`

`m.apply(fn)` returns a **new** matrix with `fn` applied to each element. This is the idiomatic,
efficient "map over the whole matrix" — the loop runs in C++, you just supply the per-element
function:

```kirito
var io = import("io")
var m = import("matrix")
var A = m.Matrix([[1, 2], [3, 4]])

io.print(A.apply(Function(x): return x * x))       # [[1.0, 4.0], [9.0, 16.0]]
io.print(A.apply(Function(x): return x + 10))      # [[11.0, 12.0], [13.0, 14.0]]

# apply composes with the math module — e.g. an element-wise square root:
var math = import("math")
io.print(A.apply(Function(x): return math.sqrt(x)))   # [[1.0, 1.414...], [1.732..., 2.0]]
```

Because a vector is just a matrix (see below), `apply` maps over a vector's elements too.

## Vectors

A matrix with one dimension equal to `1` **is** a vector (a row `1×n` or column `n×1`). Build one
with `vector`, then use the vector operations:

```kirito
var io = import("io")
var m = import("matrix")

var u = m.vector([1, 2, 3])      # a 1×3 row vector
var v = m.vector([4, 5, 6])

io.print(u.dot(v))               # 32.0   — the scalar (dot) product: 1·4 + 2·5 + 3·6
io.print(u.cross(v))             # [[-3.0, 6.0, -3.0]]   — cross product (3-vectors)
io.print(m.vector([3, 4]).norm())   # 5.0   — Euclidean length sqrt(3² + 4²)

# map over a vector with apply, just like a matrix:
io.print(u.apply(Function(x): return x * 10))    # [[10.0, 20.0, 30.0]]
```

> `*` is **always** matrix multiply — never a dot product. The dot product is `u.dot(v)`. (Two
> same-shape row vectors have incompatible inner dimensions for `*`, so `u * v` would throw.)

## Complex numbers

The `complex` module gives you a `Complex` type and the full analytic math set. Reals coerce to the
real axis, so the functions accept plain numbers too.

```kirito
var io = import("io")
var C = import("complex")

var z = C.of(3, 4)               # 3 + 4i  (also C.Complex(3, 4))
io.print(z.re, z.im)             # 3.0 4.0
io.print(z + C.of(1, 1))         # 4.0+5.0i
io.print(z * C.i)                # -4.0+3.0i   (C.i is the imaginary unit)
io.print(z.conjugate())          # 3.0-4.0i
io.print(z.modulus())            # 5.0   — |z|
io.print(z.argument())           # 0.927...   — the phase angle in radians

# Euler's identity, and roots of negative numbers:
io.print(C.exp(C.of(0, C.pi)))   # ~ -1.0 + 0i
io.print(C.sqrt(C.of(-1, 0)))    # 0.0+1.0i  = i
```

Complex numbers are **unordered** (`<`, `>` throw), and — like every numeric type in Kirito — their
`==` is **exact** (real and imaginary parts compared bit-for-bit; `NaN` never equals anything). For a
tolerant comparison of computed values use the **`.compare(other, rel_tol = 1e-9, abs_tol = 0.0)`**
method, which `Complex`, real/complex `Matrix`, and `Tensor` all carry (so `(a - b).modulus()` checks
and `result == expected` on computed floats become `result.compare(expected)`).

## Complex matrices and vectors

`complex.Matrix` is the complex counterpart of `matrix.Matrix`, with the extras a complex space
needs — `conjugate`, `hermitian` (conjugate transpose), and a Gaussian `determinant` / fast
Gauss-Jordan `inverse`:

```kirito
var io = import("io")
var C = import("complex")

var M = C.Matrix([[C.of(1, 1), C.of(2, 0)],
                  [C.of(0, 1), C.of(1, -1)]])
io.print(M.determinant())        # 2.0-2.0i   (Gaussian elimination)
io.print(M * M.inverse() == C.identity(2))   # True
io.print(M.hermitian())          # the conjugate transpose

# apply maps over complex elements too:
io.print(M.apply(Function(z): return C.conjugate(z)))   # element-wise conjugate

# complex vectors: dot is the Hermitian inner product (so v.dot(v) is real ≥ 0)
var w = C.vector([C.of(1, 1), C.of(2, 0)])
io.print(w.dot(w))               # 6.0+0.0i  = |1+i|² + |2|²
io.print(w.norm())               # 2.449...  = sqrt(6)
```

## Keyword arguments and `inspect`

Every function and method in these modules is fully introspectable and accepts keyword arguments —
exactly like Kirito-level functions:

```kirito
var io = import("io")
var C = import("complex")
var m = import("matrix")

io.print(C.of(re = 1, im = 2))                  # keyword args on a constructor
io.print(m.zeros(rows = 2, cols = 3))           # ... and on a factory
io.print(m.Matrix([[1, 2], [3, 4]]).get(row = 0, col = 1))   # ... and on a method

io.print(inspect(C.sqrt))        # sqrt(z) -> Complex
io.print(inspect(m.identity))    # identity(n: Integer) -> Matrix
```

`inspect(import("complex"))` (or `inspect` of any matrix instance) prints the whole surface — every
function, its parameters, and its return type — so you can explore the library from the REPL.

## Recap

- `matrix` and `complex` provide arbitrary-shape dense matrices; square-only operations throw on
  non-square inputs.
- **`apply(fn)`** is the efficient map-over-every-element — the loop runs in C++.
- A matrix with a dimension of `1` is a **vector**: `vector`, `dot`, `cross`, `norm`. `*` is matrix
  multiply only.
- `complex` adds complex numbers (with the analytic math set) and complex matrices (`conjugate`,
  `hermitian`, Gaussian `determinant`, Gauss-Jordan `inverse`).
- All of it supports keyword arguments and `inspect`.

Next bonus lesson: **[Tensors & Autograd](bonus-05-tensors.html)** — arrays of any rank, with automatic differentiation.
