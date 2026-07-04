#ifndef KIRITO_TENSOR_HPP
#define KIRITO_TENSOR_HPP

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// A small, dependency-free N-dimensional tensor engine. It is intentionally separate from anything
// Kirito-specific (no VM, no handles) so it is reusable, unit-testable on its own, and so the real
// `matrix` / `complex` matrix types and the Kirito `tensor` module can all be built on top of one
// implementation of the core operations.
//
// Element type `T` is a template parameter, so the same engine serves Float (double), Complex
// (std::complex<double>) or any other arithmetic-like type. Storage is a single contiguous,
// row-major (C-order) buffer. That single-buffer design is deliberate: it is CPU-only today, but a
// future device backend can replace `data` with a GPU buffer and swap these element loops for
// kernels without changing the public shape/stride contract. This core engine is a plain numeric
// container — reverse-mode autograd lives one layer up, in the VM's TensorVal (stdlib_tensor.hpp).

namespace kirito::tensor {

using Shape = std::vector<std::size_t>;

// Errors are a distinct type so the Kirito module layer can translate them into KiritoError without
// the engine depending on Kirito's error type.
struct TensorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::size_t numel(const Shape& s) {
    std::size_t n = 1;
    for (std::size_t d : s) n *= d;
    return n;
}

// Element-count cap enforced at the single point every tensor allocates from a shape (the fill
// constructor below). It is the backstop for ops whose *result* dwarfs their inputs — outer / kron /
// matmul / tensordot / broadcastto / concatenate / stack — which build their output with this
// constructor and so previously could let `numel` overflow size_t or hit std::bad_alloc. Computing
// the product with the running division check throws a clean, catchable TensorError("Tensor too
// large") instead. The Kirito layer's tns::checkSize delegates here so the cap is single-sourced.
inline constexpr std::size_t kMaxElems = 64ull * 1024 * 1024;
// Maximum rank (number of dimensions). NumPy's ndim ceiling, and critically the guard that keeps a
// tensor's rank bounded so the RECURSIVE per-axis traversals (str/tolist/indexing) can't overflow the
// native stack. Enforced here — the single point every allocated shape passes through — so ops that
// build a high-rank shape from a valid one (reshape/expanddims/broadcastto) are covered too, not just
// the nested-list constructor.
inline constexpr std::size_t kMaxRank = 64;
inline std::size_t checkedNumel(const Shape& s) {
    if (s.size() > kMaxRank) throw TensorError("Tensor has too many dimensions (max 64)");
    // A zero along any axis means zero elements — no allocation — so the shape is fine regardless of
    // the other (possibly huge) dims (np.zeros((10**8, 0)) is valid). Short-circuit before the cap so
    // a large dim paired with a 0 dim isn't falsely rejected as "too large".
    for (std::size_t d : s) if (d == 0) return 0;
    std::size_t n = 1;
    for (std::size_t d : s) {
        if (n > kMaxElems / d) throw TensorError("Tensor too large");
        n *= d;
    }
    return n;
}

// Row-major (C-order) strides for a shape: the step, in elements, to move one index along each axis.
inline Shape rowMajorStrides(const Shape& shape) {
    Shape st(shape.size());
    std::size_t acc = 1;
    for (std::size_t i = shape.size(); i-- > 0;) {
        st[i] = acc;
        acc *= shape[i];
    }
    return st;
}

// NumPy broadcasting: align shapes from the right; each axis must be equal or one of them 1.
inline Shape broadcastShapes(const Shape& a, const Shape& b) {
    std::size_t n = std::max(a.size(), b.size());
    Shape out(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t da = i + a.size() < n ? 1 : a[i + a.size() - n];
        std::size_t db = i + b.size() < n ? 1 : b[i + b.size() - n];
        // A size-1 axis broadcasts to the other extent — including 0: NumPy makes (1)·(0) -> 0, so a
        // zero-length axis wins (`std::max` would wrongly pick 1 and then index an empty buffer -> UB).
        if (da == db) out[i] = da;
        else if (da == 1) out[i] = db;
        else if (db == 1) out[i] = da;
        else throw TensorError("tensors are not broadcastable to a common shape");
    }
    return out;
}

template <class T>
class Tensor {
public:
    Shape shape;
    std::vector<T> data;  // contiguous, row-major (a future device backend would own this buffer)

    Tensor() = default;
    explicit Tensor(Shape s, T fill = T{}) : shape(std::move(s)), data(checkedNumel(shape), fill) {}
    Tensor(Shape s, std::vector<T> d) : shape(std::move(s)), data(std::move(d)) {
        if (data.size() != numel(shape)) throw TensorError("tensor data size does not match its shape");
    }

    std::size_t ndim() const { return shape.size(); }
    std::size_t size() const { return data.size(); }
    Shape strides() const { return rowMajorStrides(shape); }
    bool isScalar() const { return shape.empty(); }

    std::size_t flatIndex(const Shape& idx) const {
        if (idx.size() != shape.size()) throw TensorError("wrong number of indices for this tensor");
        Shape st = strides();
        std::size_t off = 0;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            if (idx[i] >= shape[i]) throw TensorError("tensor index out of range");
            off += idx[i] * st[i];
        }
        return off;
    }
    T& at(const Shape& idx) { return data[flatIndex(idx)]; }
    T at(const Shape& idx) const { return data[flatIndex(idx)]; }
};

// ---- shape ops -------------------------------------------------------------------------------

template <class T>
Tensor<T> reshape(const Tensor<T>& t, const Shape& newshape) {
    if (numel(newshape) != t.size()) throw TensorError("reshape: total number of elements must be unchanged");
    return Tensor<T>(newshape, t.data);
}

template <class T>
Tensor<T> flatten(const Tensor<T>& t) {
    return Tensor<T>(Shape{t.size()}, t.data);
}

// Reorder axes. `axes` is a permutation of 0..ndim-1.
template <class T>
Tensor<T> permute(const Tensor<T>& t, const Shape& axes) {
    if (axes.size() != t.ndim()) throw TensorError("permute: axes count must equal the tensor rank");
    std::vector<bool> seen(t.ndim(), false);
    Shape newshape(t.ndim());
    for (std::size_t i = 0; i < axes.size(); ++i) {
        if (axes[i] >= t.ndim() || seen[axes[i]]) throw TensorError("permute: axes must be a permutation");
        seen[axes[i]] = true;
        newshape[i] = t.shape[axes[i]];
    }
    Tensor<T> out(newshape);
    Shape oldst = t.strides(), nst = out.strides();
    for (std::size_t lin = 0; lin < out.size(); ++lin) {
        std::size_t rem = lin, oldoff = 0;
        for (std::size_t i = 0; i < out.ndim(); ++i) {
            std::size_t coord = rem / nst[i];
            rem %= nst[i];
            oldoff += coord * oldst[axes[i]];
        }
        out.data[lin] = t.data[oldoff];
    }
    return out;
}

// Transpose = reverse every axis (the matrix transpose when the tensor is 2-D).
template <class T>
Tensor<T> transpose(const Tensor<T>& t) {
    Shape axes(t.ndim());
    for (std::size_t i = 0; i < t.ndim(); ++i) axes[i] = t.ndim() - 1 - i;
    return permute(t, axes);
}

// ---- elementwise -----------------------------------------------------------------------------

template <class T, class Fn>
Tensor<T> mapUnary(const Tensor<T>& t, Fn fn) {
    Tensor<T> out(t.shape);
    for (std::size_t i = 0; i < t.size(); ++i) out.data[i] = fn(t.data[i]);
    return out;
}

// Binary elementwise with NumPy broadcasting.
template <class T, class Op>
Tensor<T> elementwise(const Tensor<T>& a, const Tensor<T>& b, Op op) {
    Shape outshape = broadcastShapes(a.shape, b.shape);
    Tensor<T> out(outshape);
    Shape ost = out.strides(), ast = a.strides(), bst = b.strides();
    std::size_t apad = outshape.size() - a.ndim(), bpad = outshape.size() - b.ndim();
    for (std::size_t lin = 0; lin < out.size(); ++lin) {
        std::size_t rem = lin, aoff = 0, boff = 0;
        for (std::size_t i = 0; i < outshape.size(); ++i) {
            std::size_t coord = rem / ost[i];
            rem %= ost[i];
            if (i >= apad) { std::size_t ai = i - apad; aoff += (a.shape[ai] == 1 ? 0 : coord) * ast[ai]; }
            if (i >= bpad) { std::size_t bi = i - bpad; boff += (b.shape[bi] == 1 ? 0 : coord) * bst[bi]; }
        }
        out.data[lin] = op(a.data[aoff], b.data[boff]);
    }
    return out;
}

template <class T> Tensor<T> add(const Tensor<T>& a, const Tensor<T>& b) { return elementwise(a, b, [](T x, T y) { return x + y; }); }
template <class T> Tensor<T> sub(const Tensor<T>& a, const Tensor<T>& b) { return elementwise(a, b, [](T x, T y) { return x - y; }); }
template <class T> Tensor<T> mul(const Tensor<T>& a, const Tensor<T>& b) { return elementwise(a, b, [](T x, T y) { return x * y; }); }
template <class T> Tensor<T> div(const Tensor<T>& a, const Tensor<T>& b) {
    return elementwise(a, b, [](T x, T y) {
        if (y == T{}) throw TensorError("tensor division by zero");
        return x / y;
    });
}
template <class T> Tensor<T> scalarOp(const Tensor<T>& a, T s, char op) {
    return mapUnary(a, [s, op](T x) -> T {
        switch (op) {
            case '+': { return x + s; } break;
            case '-': { return x - s; } break;
            case '*': { return x * s; } break;
            case '/': { if (s == T{}) throw TensorError("tensor division by zero"); return x / s; } break;
        }
        return x;
    });
}
template <class T> Tensor<T> negate(const Tensor<T>& a) { return mapUnary(a, [](T x) { return -x; }); }

// ---- matmul (2-D, and N-D batched over the leading dims) --------------------------------------

template <class T>
Tensor<T> matmul(const Tensor<T>& a, const Tensor<T>& b) {
    if (a.ndim() < 2 || b.ndim() < 2) throw TensorError("matmul requires tensors of rank >= 2");
    std::size_t m = a.shape[a.ndim() - 2], k = a.shape[a.ndim() - 1];
    std::size_t k2 = b.shape[b.ndim() - 2], n = b.shape[b.ndim() - 1];
    if (k != k2) throw TensorError("matmul: inner dimensions differ");
    Shape abatch(a.shape.begin(), a.shape.end() - 2), bbatch(b.shape.begin(), b.shape.end() - 2);
    Shape batch = broadcastShapes(abatch, bbatch);
    Shape outshape = batch;
    outshape.push_back(m);
    outshape.push_back(n);
    Tensor<T> out(outshape);
    std::size_t batchN = numel(batch);
    std::size_t aMat = m * k, bMat = k * n, oMat = m * n;
    Shape bst = rowMajorStrides(batch), ast = rowMajorStrides(abatch), bbst = rowMajorStrides(bbatch);
    for (std::size_t bi = 0; bi < batchN; ++bi) {
        // map the broadcast batch index back to each operand's batch offset
        std::size_t rem = bi, aBatchOff = 0, bBatchOff = 0;
        for (std::size_t i = 0; i < batch.size(); ++i) {
            std::size_t coord = rem / bst[i];
            rem %= bst[i];
            std::size_t apad = batch.size() - abatch.size(), bpad = batch.size() - bbatch.size();
            if (i >= apad) { std::size_t ai = i - apad; aBatchOff += (abatch[ai] == 1 ? 0 : coord) * ast[ai]; }
            if (i >= bpad) { std::size_t bj = i - bpad; bBatchOff += (bbatch[bj] == 1 ? 0 : coord) * bbst[bj]; }
        }
        const T* A = a.data.data() + aBatchOff * aMat;
        const T* B = b.data.data() + bBatchOff * bMat;
        T* O = out.data.data() + bi * oMat;
        for (std::size_t i = 0; i < m; ++i)
            for (std::size_t p = 0; p < k; ++p) {
                T v = A[i * k + p];
                for (std::size_t j = 0; j < n; ++j) O[i * n + j] += v * B[p * n + j];
            }
    }
    return out;
}

// Dot product of two 1-D tensors of equal length (a scalar).
template <class T>
T dot(const Tensor<T>& a, const Tensor<T>& b) {
    if (a.ndim() != 1 || b.ndim() != 1) throw TensorError("dot requires two 1-D tensors");
    if (a.size() != b.size()) throw TensorError("dot requires vectors of equal length");
    T acc = T{};
    for (std::size_t i = 0; i < a.size(); ++i) acc += a.data[i] * b.data[i];
    return acc;
}

// ---- reductions ------------------------------------------------------------------------------

template <class T> T sumAll(const Tensor<T>& t) { T s = T{}; for (const T& x : t.data) s += x; return s; }
template <class T> T prodAll(const Tensor<T>& t) { T p = T{1}; for (const T& x : t.data) p *= x; return p; }

template <class T> T minAll(const Tensor<T>& t) {
    if constexpr (std::is_arithmetic_v<T>) {
        if (t.data.empty()) throw TensorError("min of an empty tensor");
        T m = t.data[0];
        for (const T& x : t.data) if (x < m) m = x;
        return m;
    } else {
        throw TensorError("min is not defined for this dtype (it is not ordered)");
    }
}
template <class T> T maxAll(const Tensor<T>& t) {
    if constexpr (std::is_arithmetic_v<T>) {
        if (t.data.empty()) throw TensorError("max of an empty tensor");
        T m = t.data[0];
        for (const T& x : t.data) if (m < x) m = x;
        return m;
    } else {
        throw TensorError("max is not defined for this dtype (it is not ordered)");
    }
}

// Reduce one axis with a combiner `comb(acc, x)`, seeded with the first element along the axis.
template <class T, class Comb>
Tensor<T> reduceAxis(const Tensor<T>& t, std::size_t axis, Comb comb) {
    if (axis >= t.ndim()) throw TensorError("reduction axis out of range");
    Shape outshape;
    for (std::size_t i = 0; i < t.ndim(); ++i) if (i != axis) outshape.push_back(t.shape[i]);
    Tensor<T> out(outshape);
    Shape st = t.strides();
    std::size_t axislen = t.shape[axis], axisstep = st[axis];
    if (axislen == 0 && out.size() > 0)  // no first element to seed the reduction (numpy throws too)
        throw TensorError("zero-size reduction: cannot reduce over an empty axis");
    Shape ost = out.strides();
    // For each output cell, walk the reduced axis in the source.
    for (std::size_t olin = 0; olin < out.size(); ++olin) {
        // unravel olin over outshape, then build the source base offset (axis index 0)
        std::size_t rem = olin, base = 0, oi = 0;
        for (std::size_t i = 0; i < t.ndim(); ++i) {
            if (i == axis) continue;
            std::size_t coord = ost.empty() ? 0 : rem / ost[oi];
            if (!ost.empty()) rem %= ost[oi];
            base += coord * st[i];
            ++oi;
        }
        T acc = t.data[base];
        for (std::size_t a = 1; a < axislen; ++a) acc = comb(acc, t.data[base + a * axisstep]);
        out.data[olin] = acc;
    }
    return out;
}

// ---- 2-D linear algebra (square matrices) ----------------------------------------------------
// Pivot selection uses std::abs(T): a double for real, a magnitude for complex — so the same code
// is numerically sound for both. These back the `matrix` and `complex` matrix types.

template <class T>
T trace(const Tensor<T>& t) {
    if (t.ndim() != 2 || t.shape[0] != t.shape[1]) throw TensorError("trace requires a square 2-D tensor");
    std::size_t n = t.shape[0];
    T s = T{};
    for (std::size_t i = 0; i < n; ++i) s += t.data[i * n + i];
    return s;
}

// Largest element magnitude, used to scale the singularity threshold so that a well-conditioned but
// small-magnitude matrix (e.g. diag(1e-200), condition number 1) is not falsely declared singular by
// a fixed absolute tolerance. |·| is real for both real and complex T.
template <class T>
double maxAbsElem(const Tensor<T>& t) {
    double m = 0.0;
    for (const T& v : t.data) { double a = std::abs(v); if (a > m) m = a; }
    return m;
}

// Determinant via Gaussian elimination with partial pivoting.
template <class T>
T determinant(const Tensor<T>& t) {
    if (t.ndim() != 2 || t.shape[0] != t.shape[1]) throw TensorError("determinant requires a square 2-D tensor");
    std::size_t n = t.shape[0];
    std::vector<T> a = t.data;
    double scale = maxAbsElem(t);
    // An infinite element makes the scale-relative tol infinite, so the first finite pivot would
    // satisfy `pmag < tol` and wrongly return 0.0 before the non-finite pivot is ever reached. Reject
    // it up front (a NaN element is still caught later when it propagates into a pivot).
    if (!std::isfinite(scale)) throw TensorError("matrix contains a non-finite value (inf or NaN)");
    double tol = (scale > 0.0 ? scale : 1.0) * 1e-15;  // scale-relative singularity threshold
    T det = T{1};
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t piv = k;
        for (std::size_t i = k + 1; i < n; ++i)
            if (std::abs(a[i * n + k]) > std::abs(a[piv * n + k])) piv = i;
        double pmag = std::abs(a[piv * n + k]);   // |pivot| is real for both real and complex T
        if (!std::isfinite(pmag)) throw TensorError("matrix contains a non-finite value (inf or NaN)");
        if (pmag < tol) return T{};
        if (piv != k) {
            for (std::size_t j = 0; j < n; ++j) std::swap(a[k * n + j], a[piv * n + j]);
            det = -det;
        }
        det = det * a[k * n + k];
        for (std::size_t i = k + 1; i < n; ++i) {
            T f = a[i * n + k] / a[k * n + k];
            for (std::size_t j = k; j < n; ++j) a[i * n + j] = a[i * n + j] - f * a[k * n + j];
        }
    }
    return det;
}

// Inverse via Gauss-Jordan elimination on [A | I] — O(n^3), the fast method.
template <class T>
Tensor<T> inverse(const Tensor<T>& t) {
    if (t.ndim() != 2 || t.shape[0] != t.shape[1]) throw TensorError("inverse requires a square 2-D tensor");
    std::size_t n = t.shape[0];
    std::vector<T> a = t.data;
    double scale = maxAbsElem(t);
    if (!std::isfinite(scale)) throw TensorError("matrix contains a non-finite value (inf or NaN)");  // else an inf element throws the misleading "singular"
    double tol = (scale > 0.0 ? scale : 1.0) * 1e-15;  // scale-relative singularity threshold
    Tensor<T> inv(Shape{n, n});
    for (std::size_t i = 0; i < n; ++i) inv.data[i * n + i] = T{1};
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t piv = k;
        for (std::size_t i = k + 1; i < n; ++i)
            if (std::abs(a[i * n + k]) > std::abs(a[piv * n + k])) piv = i;
        double pmag = std::abs(a[piv * n + k]);   // |pivot| is real for both real and complex T
        if (!std::isfinite(pmag)) throw TensorError("matrix contains a non-finite value (inf or NaN)");
        if (pmag < tol) throw TensorError("matrix is singular (no inverse)");
        if (piv != k)
            for (std::size_t j = 0; j < n; ++j) {
                std::swap(a[k * n + j], a[piv * n + j]);
                std::swap(inv.data[k * n + j], inv.data[piv * n + j]);
            }
        T d = a[k * n + k];
        for (std::size_t j = 0; j < n; ++j) { a[k * n + j] = a[k * n + j] / d; inv.data[k * n + j] = inv.data[k * n + j] / d; }
        for (std::size_t i = 0; i < n; ++i) {
            if (i == k) continue;
            T f = a[i * n + k];
            for (std::size_t j = 0; j < n; ++j) {
                a[i * n + j] = a[i * n + j] - f * a[k * n + j];
                inv.data[i * n + j] = inv.data[i * n + j] - f * inv.data[k * n + j];
            }
        }
    }
    return inv;
}

// ---- coordinate helpers ----------------------------------------------------------------------
// Unravel a flat (row-major) index into per-axis coordinates for `shape`.
inline Shape unravel(std::size_t lin, const Shape& shape) {
    Shape st = rowMajorStrides(shape), coord(shape.size());
    for (std::size_t i = 0; i < shape.size(); ++i) { coord[i] = lin / st[i]; lin %= st[i]; }
    return coord;
}

// ---- structural ops --------------------------------------------------------------------------

// The index sequence for a resolved slice. Count-driven (not `i += step`): a near-INT64_MAX step
// would signed-overflow that increment (UB) and fail to terminate. start/stop are clamped to the axis
// length by resolveSlice, so the span and element count can't overflow.
inline std::vector<std::ptrdiff_t> slicePicks(std::ptrdiff_t start, std::ptrdiff_t stop, std::ptrdiff_t step) {
    std::vector<std::ptrdiff_t> picks;
    if (step > 0 && stop > start) {
        std::ptrdiff_t count = (stop - start - 1) / step + 1;
        picks.reserve(static_cast<std::size_t>(count));
        for (std::ptrdiff_t k = 0; k < count; ++k) picks.push_back(start + k * step);
    } else if (step < 0 && start > stop) {
        uint64_t span = static_cast<uint64_t>(start - stop);
        uint64_t mag = static_cast<uint64_t>(-(step + 1)) + 1ULL;  // |step|, safe even at INT64_MIN
        std::ptrdiff_t count = static_cast<std::ptrdiff_t>((span - 1) / mag + 1);
        picks.reserve(static_cast<std::size_t>(count));
        for (std::ptrdiff_t k = 0; k < count; ++k) picks.push_back(start + k * step);
    }
    return picks;
}

// A single-axis strided slice. start/stop are already resolved (0 <= start, stop within [0,len],
// stop exclusive in the walk direction); step != 0 (may be negative).
template <class T>
Tensor<T> sliceAxis(const Tensor<T>& t, std::size_t axis, std::ptrdiff_t start, std::ptrdiff_t stop, std::ptrdiff_t step) {
    if (axis >= t.ndim()) throw TensorError("slice axis out of range");
    if (step == 0) throw TensorError("slice step cannot be zero");
    std::vector<std::ptrdiff_t> picks = slicePicks(start, stop, step);
    Shape outshape = t.shape;
    outshape[axis] = picks.size();
    Tensor<T> out(outshape);
    Shape ist = t.strides(), ost = out.strides();
    for (std::size_t lin = 0; lin < out.size(); ++lin) {
        Shape c = unravel(lin, outshape);
        std::size_t off = 0;
        for (std::size_t d = 0; d < t.ndim(); ++d)
            off += (d == axis ? static_cast<std::size_t>(picks[c[d]]) : c[d]) * ist[d];
        out.data[lin] = t.data[off];
    }
    return out;
}

// Reverse the order of elements along one axis.
template <class T>
Tensor<T> flip(const Tensor<T>& t, std::size_t axis) {
    if (axis >= t.ndim()) throw TensorError("flip axis out of range");
    return sliceAxis(t, axis, static_cast<std::ptrdiff_t>(t.shape[axis]) - 1, -1, -1);
}

// Join tensors along an existing axis; all must share shape off that axis.
template <class T>
Tensor<T> concatenate(const std::vector<const Tensor<T>*>& parts, std::size_t axis) {
    if (parts.empty()) throw TensorError("concatenate needs at least one tensor");
    std::size_t nd = parts[0]->ndim();
    if (axis >= nd) throw TensorError("concatenate axis out of range");
    Shape outshape = parts[0]->shape;
    std::size_t axisTotal = 0;
    for (const Tensor<T>* p : parts) {
        if (p->ndim() != nd) throw TensorError("concatenate: tensors must have the same rank");
        for (std::size_t d = 0; d < nd; ++d)
            if (d != axis && p->shape[d] != outshape[d]) throw TensorError("concatenate: shapes differ off the join axis");
        axisTotal += p->shape[axis];
    }
    outshape[axis] = axisTotal;
    Tensor<T> out(outshape);
    Shape ost = out.strides();
    std::size_t base = 0;  // running offset along `axis` in the output
    for (const Tensor<T>* p : parts) {
        Shape ist = p->strides();
        for (std::size_t lin = 0; lin < p->size(); ++lin) {
            Shape c = unravel(lin, p->shape);
            std::size_t off = 0;
            for (std::size_t d = 0; d < nd; ++d) off += (d == axis ? c[d] + base : c[d]) * ost[d];
            out.data[off] = p->data[lin];
        }
        base += p->shape[axis];
    }
    return out;
}

// Gather rows along axis 0 by an index list (fancy indexing / np.take with axis 0).
template <class T>
Tensor<T> takeAxis0(const Tensor<T>& t, const std::vector<std::ptrdiff_t>& idx) {
    if (t.ndim() == 0) throw TensorError("cannot index a 0-D tensor");
    Shape outshape = t.shape;
    outshape[0] = idx.size();
    Tensor<T> out(outshape);
    std::size_t block = t.size() / (t.shape[0] == 0 ? 1 : t.shape[0]);
    std::ptrdiff_t dim = static_cast<std::ptrdiff_t>(t.shape[0]);
    for (std::size_t i = 0; i < idx.size(); ++i) {
        std::ptrdiff_t r = idx[i];
        if (r < 0) r += dim;   // NumPy-style: negatives count from the end, like scalar/slice indexing
        if (r < 0 || static_cast<std::size_t>(r) >= t.shape[0]) throw TensorError("index out of range");
        std::copy(t.data.begin() + static_cast<std::ptrdiff_t>(r) * static_cast<std::ptrdiff_t>(block),
                  t.data.begin() + (static_cast<std::ptrdiff_t>(r) + 1) * static_cast<std::ptrdiff_t>(block),
                  out.data.begin() + static_cast<std::ptrdiff_t>(i) * static_cast<std::ptrdiff_t>(block));
    }
    return out;
}

// Cumulative scan along an axis with a binary op (cumsum/cumprod).
template <class T, class Op>
Tensor<T> cumulative(const Tensor<T>& t, std::size_t axis, Op op) {
    if (axis >= t.ndim()) throw TensorError("cumulative axis out of range");
    Tensor<T> out = t;
    Shape st = t.strides();
    std::size_t len = t.shape[axis], step = st[axis];
    for (std::size_t lin = 0; lin < out.size(); ++lin) {
        Shape c = unravel(lin, t.shape);
        if (c[axis] == 0) continue;  // first along the axis stays as-is
        out.data[lin] = op(out.data[lin - step], out.data[lin]);
    }
    (void)len;
    return out;
}

// ---- linear algebra (extra) ------------------------------------------------------------------

// Solve A x = B for a square A (A: n×n, B: n×m or n) via the inverse.
template <class T>
Tensor<T> solve(const Tensor<T>& A, const Tensor<T>& B) {
    if (A.ndim() != 2 || A.shape[0] != A.shape[1]) throw TensorError("solve requires a square 2-D A");
    bool vec = B.ndim() == 1;
    Tensor<T> b2 = vec ? reshape(B, Shape{B.shape[0], 1}) : B;
    if (b2.ndim() != 2 || b2.shape[0] != A.shape[0]) throw TensorError("solve: A and B shapes are incompatible");
    Tensor<T> x = matmul(inverse(A), b2);
    return vec ? reshape(x, Shape{x.shape[0]}) : x;
}

// Outer product of two 1-D tensors -> a 2-D tensor.
template <class T>
Tensor<T> outer(const Tensor<T>& a, const Tensor<T>& b) {
    if (a.ndim() != 1 || b.ndim() != 1) throw TensorError("outer requires two 1-D tensors");
    Tensor<T> out(Shape{a.size(), b.size()});
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < b.size(); ++j) out.data[i * b.size() + j] = a.data[i] * b.data[j];
    return out;
}

// Kronecker product of two 2-D tensors.
template <class T>
Tensor<T> kron(const Tensor<T>& a, const Tensor<T>& b) {
    if (a.ndim() != 2 || b.ndim() != 2) throw TensorError("kron requires two 2-D tensors");
    std::size_t ar = a.shape[0], ac = a.shape[1], br = b.shape[0], bc = b.shape[1];
    Tensor<T> out(Shape{ar * br, ac * bc});
    for (std::size_t i = 0; i < ar; ++i)
        for (std::size_t j = 0; j < ac; ++j)
            for (std::size_t k = 0; k < br; ++k)
                for (std::size_t l = 0; l < bc; ++l)
                    out.data[(i * br + k) * (ac * bc) + (j * bc + l)] = a.data[i * ac + j] * b.data[k * bc + l];
    return out;
}

}  // namespace kirito::tensor

#endif
