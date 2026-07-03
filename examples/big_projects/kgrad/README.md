# kgrad — a tensor / autodiff / deep-learning library in pure Kirito

A from-scratch tensor + automatic-differentiation + neural-network library written entirely in
Kirito (`.ki`), in the spirit of PyTorch. It exists as a substantial example program **and** as a
heavy stress test for the interpreter: every operation runs through Kirito's classes, operator
overloading, closures, exceptions, and collections, so bugs and rough edges surface fast. (Building
it surfaced and fixed several interpreter issues — see the repo history.)

> Performance note: this runs on the Kirito interpreter (a bytecode VM), so tensor math (nested
> Kirito loops) is slow. Everything here is correctness-first and uses tiny sizes. The architecture, however, is
> designed so the slow CPU kernels could be swapped for a fast (e.g. GPU) backend without touching
> the layers above — see "Backend abstraction".

## Layout

```
lib/backend.ki     Device / Allocator / KernelProvider abstraction + CPU backend (the only loops)
lib/ndarray.ki     strided NdArray: views (reshape/transpose/permute/slice/broadcast), matmul, reductions
lib/autograd.ki    reverse-mode autodiff: Tensor, computational graph, backward, no_grad, detach
lib/nn.ki          Module, Linear, BatchNorm1d, ReLU/Sigmoid/Tanh, Sequential, a custom layer, losses
lib/conv.ki        Conv2d (fused autograd op with exact, gradient-checked backward)
lib/optim.ki       SGD (+momentum), Adam
lib/data.ki        TensorDataset, DataLoader (batching + shuffling + iteration)
lib/checkpoint.ki  weight save/load (graph-independent), via the stdlib `serialize` module
lib/models.ki      PCA (power iteration), LogisticRegression
test_kgrad.ki      end-to-end: losses, trains logreg + an MLP that solves XOR, PCA, dataloader, save/load
test_extra.ki      BatchNorm, custom layer, Conv2d gradient-check, edge + adversarial cases
```

## Running

```sh
cmake --build build                                          # build the interpreter
ki --lib examples/big_projects/kgrad/lib examples/big_projects/kgrad/test_kgrad.ki
ki --lib examples/big_projects/kgrad/lib examples/big_projects/kgrad/test_extra.ki
```

`test_kgrad` trains real models and checks they learn (logistic regression reaches ~39/40 accuracy;
an MLP drives XOR loss from ~0.69 to ~1e-4 with correct predictions; PCA recovers the principal
axis). `test_extra` gradient-checks Conv2d against finite differences (max error ~1e-11).

## Architecture

### Backend abstraction (GPU-forward design)

Tensor operations never contain compute loops — they delegate to a **`Device`**, which exposes a
**`KernelProvider`** (the compute kernels: `binary`, `unary`, `matmul`, `gather`, reductions) and an
**allocator** (`zeros_buffer`/`full_buffer`). Kernels operate on **contiguous flat buffers** plus
shape metadata; the ndarray layer materialises strided/broadcast views (via the `gather` kernel)
before calling a kernel that wants packed data — exactly how real frameworks bridge strided tensors
and kernels. The only CPU-specific code lives in `CpuDevice`.

To add a GPU backend you would implement a new `Device` with the same kernel surface (buffers become
device handles; kernels launch GPU code). What *is* portable by this design: the strided view layer,
autograd, optimizers, and all `nn` layers — none of them mention the CPU. What would still need care
for a real GPU: asynchronous execution / streams, host↔device transfers, and keeping kernels
contiguous-or-explicit-stride (already the contract here). `TensorStorage` is represented today by a
plain buffer (`List`) owned by an `NdArray`; a GPU `Device` would back it with a device pointer.

### Strided tensors and views

An `NdArray` is `(device, buffer, offset, shape, strides)` — the NumPy/PyTorch model. `reshape`
(when contiguous), `transpose`/`permute`, `slice`, and `broadcast_to` (stride-0 expansion) all return
**views that share the buffer** — no copy. `.contiguous()` packs a view when a kernel needs it.

### Reverse-mode autodiff over a computational graph

A `Tensor` wraps an `NdArray` plus `grad`, `requires_grad`, its parent tensors, and a `grad_fn`
closure. Each differentiable op builds the forward value **and** records a graph node. `backward()`
topologically orders the graph and runs the `grad_fn`s in reverse, accumulating gradients (gradient
accumulation is the default — clear with `zero_grad`). Supported: `requires_grad`, `detach()` (a
graph-free view), `no_grad()` (a context manager suspending graph construction), and graph retention
(visit-marks are cleared after each `backward`, so a graph can be traversed again). Correctness is
verified by finite-difference gradient checks for every op (errors ~1e-10).

`compile_graph(root)` performs the topological "build-then-run" compile step (validating/ordering the
graph before execution). On the static-vs-dynamic question: kgrad builds the graph eagerly (a tape)
and executes backward over it — the most practical and testable choice. The same node list returned
by `compile_graph` is what a TensorFlow-1-style ahead-of-time executor would consume; wiring a cached
re-execution engine onto it is the natural extension.

### Modules

Every layer is a `Module` with named **parameters**, **buffers** (e.g. BatchNorm running stats), and
sub-modules; it supports `parameters()`, `zero_grad()`, `train()`/`eval()`, and
`state_dict()`/`load_state_dict()`. Custom layers subclass `Module`, register parameters, and override
`forward` — because `forward` is built from autograd ops, the backward is automatic (see
`nn.AffineSquare`, and `conv.Conv2d` for a fused custom op with a hand-written backward).

### Serialization

`state_dict()` produces a plain `Dict` of dotted-name → nested numbers (no graph, no objects), so
`checkpoint.save`/`load` persist weights independently of any model definition; `load_state_dict`
restores them into any model with a matching parameter layout.

## What's implemented

- Tensor: broadcasting, reshape/view, transpose/permute, slice, reductions (sum/mean/max/min, all &
  per-axis), matmul, elementwise arithmetic, scalar ops, indexing — all view-based where possible.
- Autograd: full reverse mode, gradient accumulation, requires_grad / detach / no_grad, graph compile.
- Layers: Linear, Conv2d, BatchNorm1d, ReLU/Sigmoid/Tanh, Sequential, custom layers.
- Losses: MSELoss, BCEWithLogits, CrossEntropy, NLL (numerically stable).
- Optimizers: SGD (+momentum), Adam.
- Data: TensorDataset, DataLoader (batch/shuffle/iterate). Helpers: PCA, LogisticRegression.
- Serialization of weights, independent of graph definitions.

## Future / not yet done

- A real GPU `Device` (the abstraction is in place; kernels + memory transfers are the work).
- Faster CPU kernels (the interpreter is the bottleneck; a native `KernelProvider` would help most).
- Pooling/dropout layers, more conv variants, lazy iterators for the DataLoader, an ahead-of-time
  graph executor built on `compile_graph`.
