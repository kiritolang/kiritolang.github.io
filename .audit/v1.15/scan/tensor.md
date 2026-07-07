# v1.15 audit — tensor engine & autograd

Source: `src/kirito/tensor.hpp` (602 lines), `src/kirito/stdlib_tensor.hpp` (2506 lines).
Probe: `./build-debug/ki`.

## LOG
- Starting: reading tensor.hpp fully, then stdlib_tensor.hpp.

## FINDINGS

### F1 [SUSPECT/MED] Per-axis `sum`/`prod` over a length-0 axis throws instead of returning the identity (0 / 1)
- where: src/kirito/tensor.hpp:325 (`reduceAxis` zero-size guard) reached from stdlib_tensor g_sum/prod
- repro: `T.zeros([2,0]).sum(1)` -> THROW "zero-size reduction: cannot reduce over an empty axis"
- actual: throws for sum AND prod over an empty axis.  expected (NumPy): `np.zeros((2,0)).sum(axis=1)` -> `[0.,0.]`; prod -> `[1.,1.]`. Only min/max/mean/argmin/argmax legitimately throw on an empty axis.
- note: whole-tensor sum of empty returns 0.0 and prod returns 1.0 (correct), so per-axis is inconsistent with whole-tensor. The code comment "(numpy throws too)" is inaccurate for sum/prod.
- fix idea: reduceAxis takes an identity for the empty-axis case, or g_sum/prod special-case a 0-length axis to fill the identity instead of calling reduceAxis.

### F2 [LOW] `tensor.concatenate` / `tensor.split` reject a negative `axis` (no NumPy-style wrap), unlike reductions/slice/take/stack
- where: src/kirito/stdlib_tensor.hpp:2412 (concatenate) and :2426 (split) cast axis via `static_cast<std::size_t>(asInt(...))`
- repro: `T.concatenate([T.zeros([2,2]), T.zeros([2,2])], -1)` -> THROW "concatenate axis out of range"
- actual: negative axis wraps to a huge size_t then fails the `axis >= nd` check. expected: NumPy allows axis=-1. `stack` DOES handle negatives (works), so this is an internal inconsistency.
- fix idea: normalize a negative axis before the size_t cast, as reductions/take already do.

### F3 [HIGH] `tensor.split` with a negative section size -> size_t wrap bypasses the sum check -> out-of-bounds heap read
- where: src/kirito/stdlib_tensor.hpp:2437 (`std::size_t s = static_cast<std::size_t>(e.asInt("section"))`) then :2438 sum check, then g_split -> g_sliceAxis
- repro: `T.split(T.Tensor([1.0,2.0,3.0,4.0]), [-1, 5])` -> `[[], [2.42092166462211e-322, 1.0, 2.0, 3.0, 4.0]]` (deterministic garbage first element = OOB read)
- mechanism: `-1` casts to SIZE_MAX; sizes=[SIZE_MAX,5]; `total = SIZE_MAX+5` wraps to 4 == axis length, so `if (total != len)` passes. g_split then builds SliceRange with base += SIZE_MAX (ptrdiff -1), and slicePicks(-1, 4, 1) yields picks {-1,0,1,2,3}; index -1 -> `static_cast<size_t>(-1)` -> reads t.data[huge] out of bounds.
- actual: silent OOB read (heap-buffer-overflow under ASan). expected: reject negative section sizes with a clear error.
- fix idea: in the module `split` list branch, require each `e.asInt("section") >= 0` before the size_t cast (and use a checked/overflow-safe sum), mirroring the `n <= 0` guard on the integer branch.

### F4 [MED/SUSPECT] `median` silently ignores NaN in larger arrays but propagates it in others (position-dependent), unlike max/min which uniformly propagate NaN
- where: src/kirito/stdlib_tensor.hpp:1218 (`medianT`'s `med` lambda sorts NaN last, then averages the middle element(s))
- repro: `T.Tensor([1.0,nan]).median()` -> nan ; `T.Tensor([1.0,nan,3.0,2.0]).median()` -> 2.5 (NaN ignored)
- actual: result depends on size/position of NaN after the "NaN sorts last" sort. expected: numpy.median returns NaN whenever any element is NaN. This is exactly the A13-1 inconsistency that was deliberately fixed for max/min/argmax (they now propagate NaN), but median still has it. mean/std also naturally propagate NaN, so median is the odd one out.
- fix idea: in medianT, if any element in the line is NaN, yield NaN (mirror the max/min NaN-propagation policy).
F4 median-NaN = RE-VERIFIED NON-BUG: median sorting NaN-last is a DELIBERATE prior decision (median is sort-defined; sort/argsort/unique also sort NaN last), pinned in r7_regressions.ki. Reverted the propagate-NaN change. NOT a bug.
