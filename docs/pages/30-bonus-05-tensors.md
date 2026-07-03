# Bonus Lesson 5 — Tensors & Autograd

This is the longest lesson, and it earns it: the `tensor` module is a complete numeric-array library
with **automatic differentiation** built in. We'll go slowly and cover the whole surface.

A **tensor** is a dense, rectangular array of numbers with any number of dimensions:

- a **scalar** is a 0-dimensional tensor,
- a **vector** is 1-dimensional,
- a **matrix** is 2-dimensional,
- and you can keep going — 3-D, 4-D, and beyond.

Three words describe every tensor:

- its **rank** (`ndim`) — how many dimensions it has,
- its **shape** — the length along each dimension, as a list,
- its **dtype** — the element type, either `"Float"` (the default, a real number) or `"Complex"`.

The `tensor` module is also the foundation the `matrix` and `complex` matrix types are built on: a
2-D Float tensor *is* a real matrix. Everything runs on the CPU.

Throughout this lesson:

```kirito
var io = import("io")
var T = import("tensor")
```

## 1. Creating tensors

Build a tensor from a nested list — the depth of nesting sets the rank, and every row at a given
level must be the same length (the array is rectangular). Integers are accepted and stored as Floats.

```kirito
var io = import("io")
var T = import("tensor")

var a = T.Tensor([[1, 2, 3], [4, 5, 6]])    # a 2×3 tensor
io.print(a.shape())            # [2, 3]
io.print(a.ndim())             # 2
io.print(a.size())             # 6     — total number of elements
io.print(a.dtype())            # Float
```

There are factories for common shapes. Each takes the shape as a list (or a count, for `eye`):

```kirito
var io = import("io")
var T = import("tensor")

io.print(T.zeros([2, 2]))            # [[0.0, 0.0], [0.0, 0.0]]
io.print(T.ones([3]))                # [1.0, 1.0, 1.0]
io.print(T.full([2, 2], 9))          # [[9.0, 9.0], [9.0, 9.0]]
io.print(T.eye(3))                   # the 3×3 identity matrix
io.print(T.arange(0, 6, 2))          # [0.0, 2.0, 4.0]   — start, stop (exclusive), step
io.print(T.linspace(0, 1, 5))        # [0.0, 0.25, 0.5, 0.75, 1.0]   — 5 evenly-spaced points
```

A few more builders construct from, or relative to, existing tensors:

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2], [3, 4]])

io.print(T.zeroslike(a))                  # zeros with a's shape
io.print(T.diag(T.Tensor([1, 2, 3])))     # a vector -> a diagonal matrix
io.print(T.diag(a))                       # a matrix -> its diagonal: [1.0, 4.0]
io.print(T.tril(T.ones([3, 3])))          # lower triangle (triu for upper)
```

## 2. Indexing, slicing, and selection

A **full index** — one integer per dimension — returns the scalar element. A **partial index**
returns the sub-tensor of the remaining dimensions.

```kirito
var io = import("io")
var T = import("tensor")
var cube = T.Tensor([[[1, 2], [3, 4]], [[5, 6], [7, 8]]])    # shape [2, 2, 2]

io.print(cube[1, 0, 1])        # 6.0   — one element
io.print(cube[0])              # [[1.0, 2.0], [3.0, 4.0]]   — a 2×2 sub-tensor
cube[0, 0, 0] = 99             # assign into an element (full index)
io.print(cube[0, 0, 0])        # 99.0
```

You can **slice** the first dimension with `start:stop:step` (the bounds follow the usual rules —
`stop` is exclusive, and negative numbers count back from the end):

```kirito
var io = import("io")
var T = import("tensor")
var v = T.Tensor([10, 20, 30, 40, 50])

io.print(v[1:4])               # [20.0, 30.0, 40.0]
io.print(v[:2])                # [10.0, 20.0]
io.print(v[-2:])               # [40.0, 50.0]
```

Two more selection forms, both returning a fresh tensor:

- an **index list** picks rows along the first dimension, in the order given,
- a **boolean mask** (a 0/1 tensor of the same shape, usually from a comparison) selects the
  elements where it is non-zero, flattened into a 1-D tensor.

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2, 3], [4, 5, 6]])

io.print(a[[1, 0]])            # [[4.0, 5.0, 6.0], [1.0, 2.0, 3.0]]   — rows 1 then 0
io.print(a[a.gt(3)])           # [4.0, 5.0, 6.0]   — elements greater than 3
```

> Indexing and `start:stop:step` slicing return **detached** copies (they don't carry gradients). For
> the gradient-aware versions, use the `t.slice(start, stop, step, axis)` and `t.take(indices, axis)`
> **methods** instead — they appear again in the autograd section.

## 3. Element-wise arithmetic and broadcasting

`+`, `-`, `*`, `/` operate **element by element**. In particular `*` is the element-wise product,
**not** matrix multiplication (that's `matmul`, coming up).

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2, 3], [4, 5, 6]])

io.print(a + a)                # [[2.0, 4.0, 6.0], [8.0, 10.0, 12.0]]
io.print(a * a)                # element-wise square
io.print(a * 2)                # a scalar applies to every element
io.print(-a)                   # negation
```

When the two shapes differ, they are combined by **broadcasting**. The rule:

1. Align the shapes from the **right**.
2. For each dimension, the sizes must be **equal**, or one of them must be **1**.
3. A size-1 dimension is **stretched** to match the other.

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2, 3], [4, 5, 6]])    # shape [2, 3]

io.print(a + T.Tensor([[10, 20, 30]]))      # [1,3] stretched over both rows
io.print(a + T.Tensor([[100], [200]]))      # [2,1] stretched over all columns
```

The other element-wise operators are `**` (power), `%` (remainder), and `//` (floor division):

```kirito
var io = import("io")
var T = import("tensor")
io.print(T.Tensor([2, 3, 4]) ** 2)     # [4.0, 9.0, 16.0]
io.print(T.Tensor([5, 7, 9]) % 3)      # [2.0, 1.0, 0.0]
io.print(T.Tensor([5, 7, 9]) // 3)     # [1.0, 2.0, 3.0]
```

## 4. Comparisons, logic, and selection

Comparisons produce a **0/1 mask** the same shape as the inputs. The methods are `eq`, `ne`, `lt`,
`le`, `gt`, `ge`; the operators `<`, `<=`, `>`, `>=` do the same.

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([1, 2, 3, 4])

io.print(a.gt(2))              # [0.0, 0.0, 1.0, 1.0]
io.print(a > 2)                # the same
io.print(a.eq(T.Tensor([1, 9, 3, 9])))   # [1.0, 0.0, 1.0, 0.0]
```

> `==` and `!=` are reserved for **whole-tensor** equality (they return a single `Bool`, true when
> the tensors have equal shape and values). For the element-wise comparison, use `t.eq(...)` /
> `t.ne(...)`.

Combine masks with `logicaland` / `logicalor` / `logicalxor` / `logicalnot`, and use them to select:

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([1, 2, 3, 4, 5])

var mask = a.gt(1).logicaland(a.lt(5))         # 1 < a < 5
io.print(mask)                                  # [0.0, 1.0, 1.0, 1.0, 0.0]
io.print(T.where(mask, a, T.zeros([5])))        # keep where mask, else 0
io.print(a.clip(2, 4))                          # clamp every element into [2, 4]
io.print(a.maximum(T.full([5], 3)))             # element-wise max against 3
```

`where(cond, a, b)` builds a new tensor taking elements from `a` where `cond` is non-zero and from
`b` otherwise (all three broadcast together).

## 5. Matrix products, dot, and contraction

`*` is element-wise, so matrix multiplication is the **`matmul`** method. It also works **batched**:
for tensors of rank ≥ 2 it multiplies the last two dimensions and broadcasts the leading ones.

```kirito
var io = import("io")
var T = import("tensor")

io.print(T.Tensor([[1, 2], [3, 4]]).matmul(T.Tensor([[5, 6], [7, 8]])))   # [[19.0, 22.0], [43.0, 50.0]]
io.print(T.Tensor([1, 2, 3]).dot(T.Tensor([4, 5, 6])))                    # 32.0   — 1-D dot product
```

`tensordot` generalizes `matmul` to contract over **any** chosen axes. Pass an integer `N` to contract
the last `N` axes of the first tensor with the first `N` of the second (`N = 1` is matrix multiply;
`N = 2` multiplies and sums every element), or a `[a-axes, b-axes]` pair to name the axes explicitly.

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2], [3, 4]])
var b = T.Tensor([[5, 6], [7, 8]])

io.print(T.tensordot(a, b, 1))     # [[19.0, 22.0], [43.0, 50.0]]   — same as matmul
io.print(T.tensordot(a, b, 2))     # 70.0   — sum of every a[i,j]*b[i,j]

var x = T.full([2, 3, 4], 1)
var y = T.full([4, 3, 5], 2)
# contract x's axes 1 and 2 with y's axes 1 and 0:
io.print(T.contract(x, y, [1, 2], [1, 0]).shape())     # [2, 5]
```

For the fully general case, **`einsum`** takes a subscript string. Repeated letters are summed,
letters that survive to the right of `->` form the output:

```kirito
var io = import("io")
var T = import("tensor")
var m = T.Tensor([[1, 2], [3, 4]])

io.print(T.einsum("ij,jk->ik", m, T.eye(2)))   # matrix multiply -> m
io.print(T.einsum("ij->ji", m))                # transpose
io.print(T.einsum("ii->", m))                  # trace: 5.0
```

## 6. Reductions

Reductions collapse the whole tensor to a scalar, or collapse a single `axis` to a lower-rank tensor.

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2, 3], [4, 5, 6]])

io.print(a.sum())              # 21.0           — the whole tensor
io.print(a.sum(0))             # [5.0, 7.0, 9.0] — collapse dimension 0
io.print(a.sum(1))             # [6.0, 15.0]     — collapse dimension 1
io.print(a.mean(), a.min(), a.max())            # 3.5 1.0 6.0
io.print(a.prod())             # 720.0
```

The full set: `sum`, `mean`, `prod`, `min`, `max`, `argmin`, `argmax`, `std`, `var`, `median`, `ptp`
(max − min), `all`, `any`, `cumsum`, `cumprod`. Each takes an optional `axis`.

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 5, 2], [4, 0, 6]])

io.print(a.argmax(1))          # [1.0, 2.0]   — index of the max along each row
io.print(a.std())              # spread of all six values
io.print(a.all(), a.any())     # False True   — all/any non-zero
io.print(T.Tensor([1, 2, 3, 4]).cumsum())      # [1.0, 3.0, 6.0, 10.0]
```

## 7. Reshaping and structural operations

These rearrange elements without changing them:

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2, 3], [4, 5, 6]])

io.print(a.transpose())        # [[1.0, 4.0], [2.0, 5.0], [3.0, 6.0]]
io.print(a.reshape([3, 2]))    # same six values, new shape
io.print(a.flatten())          # [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
io.print(a.permute([1, 0]))    # reorder axes (here, == transpose)
io.print(a.swapaxes(0, 1))     # swap two axes
io.print(a.flip())             # reverse
```

Add or drop size-1 axes, broadcast to a shape, or repeat:

```kirito
var io = import("io")
var T = import("tensor")

io.print(T.zeros([1, 3, 1]).squeeze().shape())     # [3]   — drop size-1 axes
io.print(T.Tensor([1, 2]).expanddims(0).shape())   # [1, 2] — insert a size-1 axis
io.print(T.Tensor([[1], [2]]).broadcastto([2, 3])) # stretch to [2, 3]
io.print(T.Tensor([1, 2]).repeat(2))               # [1.0, 1.0, 2.0, 2.0]
io.print(T.Tensor([1, 2]).tile([2]))               # [1.0, 2.0, 1.0, 2.0]
```

Join several tensors, or split one apart:

```kirito
var io = import("io")
var T = import("tensor")
var a = T.Tensor([[1, 2], [3, 4]])

io.print(T.concatenate([a, a], 0).shape())                     # [4, 2]  — stack rows
io.print(T.stack([T.Tensor([1, 2]), T.Tensor([3, 4])], 0))     # [[1,2],[3,4]] — new axis
io.print(len(T.split(a, 2, 0)))                                # 2  — split into 2 pieces
```

**`apply`** maps a function over every element (the loop runs in native code):

```kirito
var io = import("io")
var T = import("tensor")
io.print(T.Tensor([1, 4, 9]).apply(Function(x): return x ** 0.5))   # [1.0, 2.0, 3.0]
```

## 8. Linear algebra

The module-level functions operate on 2-D tensors:

```kirito
var io = import("io")
var T = import("tensor")
var M = T.Tensor([[4, 3], [6, 3]])

io.print(T.det(M))                       # -6.0   — determinant
io.print(T.trace(M))                     # 7.0    — sum of the diagonal
io.print(T.inv(M).matmul(M))             # the identity (M⁻¹ M)
io.print(T.solve(M, T.Tensor([[1], [1]])))   # solve M x = [1, 1]
io.print(T.norm(T.Tensor([3, 4])))       # 5.0    — Euclidean length
io.print(T.outer(T.Tensor([1, 2]), T.Tensor([3, 4])))     # outer product
io.print(T.cross(T.Tensor([1, 0, 0]), T.Tensor([0, 1, 0])))   # [0.0, 0.0, 1.0]
```

Also available: `inner`, `kron` (the Kronecker product), and `einsum` (Section 5).

## 9. Sorting and searching

```kirito
var io = import("io")
var T = import("tensor")

io.print(T.Tensor([3, 1, 2]).sort())          # [1.0, 2.0, 3.0]
io.print(T.Tensor([3, 1, 2]).argsort())       # [1.0, 2.0, 0.0]  — the sorting order
io.print(T.unique(T.Tensor([3, 1, 3, 2, 1]))) # [1.0, 2.0, 3.0]
io.print(T.searchsorted(T.Tensor([1, 3, 5]), 4))   # 2  — where 4 would be inserted
io.print(T.nonzero(T.Tensor([0, 5, 0, 7])))   # [[1.0, 3.0]]  — indices of non-zeros
```

## 10. Automatic differentiation

This is what sets tensors apart from a plain array library: a tensor can compute the **derivatives**
of a result with respect to its inputs. This is how you train a model by gradient descent.

### The idea

Gradient tracking is **off by default**. You opt a tensor in by making it a *leaf* of the
computation — either at creation with `requiresgrad = True`, or afterwards with `t.requiresgrad(True)`.
From then on, every differentiable operation involving that tensor records how the result was
produced, building a **graph** behind the scenes. Calling `backward()` on a final scalar walks that
graph in reverse and fills in each input's `.grad`.

```kirito
var io = import("io")
var T = import("tensor")

var x = T.Tensor([1, 2, 3], requiresgrad = True)
var y = x.square().sum()       # y = x0² + x1² + x2²
y.backward()
io.print(x.grad)               # [2.0, 4.0, 6.0]   — dy/dx = 2x
```

The gradient is exact: `d(x²)/dx = 2x`, evaluated at `[1, 2, 3]`, is `[2, 4, 6]`.

### What `backward` needs

`backward()` with no argument requires the tensor it's called on to be a **scalar** (a single
element). If you want the gradient of a non-scalar tensor, pass a *seed* gradient of the same shape:

```kirito
var io = import("io")
var T = import("tensor")
var x = T.Tensor([2, 3], requiresgrad = True)
var y = x * x                          # not a scalar
y.backward(T.ones([2]))                # seed of ones
io.print(x.grad)                       # [4.0, 6.0]
```

### Gradients accumulate — clear them

`backward()` **adds** into `.grad` rather than replacing it. In a training loop you must reset the
gradients each step with `zerograd()`, or they pile up:

```kirito
var io = import("io")
var T = import("tensor")
var x = T.Tensor([1, 1], requiresgrad = True)
x.sum().backward()
x.sum().backward()             # called twice without clearing
io.print(x.grad)               # [2.0, 2.0]   — accumulated
x.zerograd()
io.print(x.grad)               # None
```

### Turning gradients off

Two tools stop gradient flow on purpose:

- `t.detach()` returns a copy of `t` that tracks nothing.
- `with T.nograd():` runs a whole block with tracking disabled — for inference, or for computing a
  parameter update (you don't want the update itself recorded; see [the next section](#why-the-update-rebinds-tensors-are-immutable)).

```kirito
var io = import("io")
var T = import("tensor")
var w = T.Tensor([1.0, 2.0], requiresgrad = True)
io.print(w.detach().requiresgrad())    # False
with T.nograd():
    var probe = w * 2
    io.print(probe.requiresgrad())     # False — not tracked
```

### What is differentiable, and what detaches

Differentiable operations include `+ - * / **`, `matmul`, `tensordot`, `sum`/`mean`,
`transpose`/`reshape`/`permute`/`flatten`/`squeeze`/`expanddims`/`swapaxes`/`flip`/`broadcastto`,
`concatenate`/`stack`/`split`, `where`/`clip`/`maximum`/`minimum`, `cumsum`, the gradient-aware
`t.slice(...)` / `t.take(...)`, and a wide element-wise math set:

> `exp`, `log`, `log10`, `log2`, `sqrt`, `cbrt`, `square`, `reciprocal`, `abs`, `sin`, `cos`, `tan`,
> `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `relu`, `sigmoid`,
> `softplus`, `erf`, and `pow(p)`. (`sign`, `floor`, `ceil`, `round`, `trunc` keep the graph but have
> zero gradient.)

Operations that **cannot** carry a gradient — `min`/`max`, `argmin`/`argmax`, `sort`/`argsort`,
`prod`, `cumprod`, `ptp`, `std`/`var`, `median`, `dot`, `unique`/`nonzero`/`searchsorted`, `einsum`,
the linear-algebra functions, `apply`, `astype`, `repeat`/`tile`, `%`/`//`, and `[]`-style
indexing/slicing — **detach** their result. If you call one of these on a tensor that
requires grad while tracking is on, the module prints a one-time warning so the break is never
silent:

```text
warning: tensor min() is not differentiable; its result is detached from the gradient graph ...
```

### A worked example: fitting a line

Here is a complete training loop. It fits `y = 2x + 1` from four data points by minimizing the mean
squared error with gradient descent:

```kirito
var io = import("io")
var T = import("tensor")

var xs = T.Tensor([[0], [1], [2], [3]])
var ys = T.Tensor([[1], [3], [5], [7]])         # the targets: y = 2x + 1
var w = T.zeros([1, 1], requiresgrad = True)    # weight (to learn)
var b = T.zeros([1, 1], requiresgrad = True)    # bias   (to learn)
var step = 0
while step < 600:
    var pred = xs.matmul(w) + b                 # forward pass
    var loss = (pred - ys).square().mean()      # mean squared error
    w.zerograd()                                # clear last step's gradients
    b.zerograd()
    loss.backward()                             # backward pass: fills w.grad, b.grad
    with T.nograd():                            # update without recording it
        w = w - w.grad * 0.05
        b = b - b.grad * 0.05
    w.requiresgrad(True)                         # the new w/b are fresh leaves
    b.requiresgrad(True)
    step = step + 1
io.print(w[0, 0], b[0, 0])                       # ~ 2.0  ~ 1.0
```

Read the loop as five steps repeated: **predict**, measure the **loss**, **clear** old gradients,
**backpropagate**, then **update** the parameters a small step against their gradient. After a few
hundred iterations `w` and `b` converge to `2` and `1`.

### Why the update rebinds: tensors are immutable

Tensor arithmetic is pure — every operation returns a brand-new tensor and never mutates its
operands (`a + b` leaves `a` untouched). Kirito has no `+=`, so an update is always a rebind:
`w = w - w.grad * 0.05` binds `w` to a fresh tensor, and the new value produced under `nograd()`
carries no gradient tracking, so you re-mark it a leaf with `w.requiresgrad(True)` before the next
step. The single in-place exception is element assignment (`t[i, j] = v`).

## 11. Saving tensors

A gradient-free tensor can be serialized — to text with the `serialize` module, or to a compact
binary blob with `dump` — and loaded back later. This is how you persist trained weights.

```kirito
var io = import("io")
var T = import("tensor")
var ser = import("serialize")

var w = T.Tensor([[1.5, 2.5], [3.5, 4.5]])
var saved = ser.dumps(w)               # a portable text snapshot
var loaded = ser.loads(saved)
io.print(loaded == w)                  # True
```

Only **gradient-free** tensors may be saved: trying to serialize a tensor that requires grad throws,
so detach it first.

```kirito
var io = import("io")
var T = import("tensor")
var ser = import("serialize")

var p = T.Tensor([1.0, 2.0], requiresgrad = True)
io.print(ser.loads(ser.dumps(p.detach())) == T.Tensor([1.0, 2.0]))   # True
```

## 12. Complex tensors

Pass `dtype = "Complex"` to hold complex numbers. The array operations apply just as before; `astype`
converts between dtypes, and mixing a Float and a Complex tensor promotes the result to Complex.

```kirito
var io = import("io")
var T = import("tensor")
var C = import("complex")

var z = T.Tensor([[1, 2], [3, 4]], dtype = "Complex")
io.print(z.dtype())            # Complex
io.print(z[0, 0])              # 1.0+0.0i   — elements are Complex values
io.print(z.matmul(z))          # complex matrix product
io.print(z.conj())             # element-wise conjugate
io.print((T.Tensor([1, 2]) + T.Tensor([1, 1], dtype = "Complex")).dtype())   # Complex (promoted)
```

Complex tensors are a numeric container only. **Autograd is Float-only**: a Complex tensor cannot
require gradients (`requiresgrad(True)` throws), and the differentiable element-wise math methods are
Float-only (for complex analytic functions, the `complex` module has them). `min`/`max` also throw —
complex numbers have no ordering. Everything else — arithmetic, `matmul`, `tensordot`, reshaping,
reductions, `real`/`imag`/`conj`/`angle` — works on both dtypes.

## Recap

- A tensor has a **rank** (`ndim`), a **shape**, and a **dtype** (`"Float"` or `"Complex"`); build one
  from a nested list or with `zeros`/`ones`/`full`/`eye`/`arange`/`linspace`/`diag`/`*like`.
- Index with integers (`t[i, j]`), slice the first axis (`t[a:b]`), or select with an index list or a
  boolean mask; the gradient-aware forms are the `slice` / `take` methods.
- `+ - * / ** % //` are **element-wise** with **broadcasting**; **`matmul`**/`dot`/`tensordot`/`einsum`
  do products and contractions.
- Comparisons (`eq`/`lt`/`gt`/…, and `< > …`) give 0/1 masks; combine with `logicaland`/… and select
  with `where`/`clip`/`maximum`/`minimum`.
- Reduce with `sum`/`mean`/`prod`/`min`/`max`/`argmin`/`argmax`/`std`/`var`/`median`/`all`/`any`/
  `cumsum`/… (whole-tensor or per-axis); reshape with `transpose`/`reshape`/`flatten`/`permute`/
  `squeeze`/`expanddims`/`flip`/`concatenate`/`stack`/`split`/…
- Linear algebra: `det`/`inv`/`solve`/`trace`/`norm`/`outer`/`inner`/`kron`/`cross`/`einsum`. Sorting:
  `sort`/`argsort`/`unique`/`nonzero`/`searchsorted`.
- **Autograd** is opt-in (`requiresgrad = True`) and Float-only: differentiable ops record a graph,
  `backward()` fills each leaf's `.grad`, `zerograd()` clears it, `detach()` and `with T.nograd():`
  stop gradient flow. A non-differentiable op on a grad tensor warns and detaches.
- A gradient-free tensor **serializes** (save/load weights); complex tensors are numeric-only.
- Tensor **arithmetic is pure** — every op returns a new tensor and never mutates its operands — so a
  gradient-descent step **rebinds** the parameter (`w = w - w.grad*lr`, re-marked with
  `requiresgrad(True)`) rather than mutating it in place. (`t[i, j] = v` is the one in-place exception.)

This is the final bonus lesson. Back to the **[course index](index.html)**, or go build a model.
