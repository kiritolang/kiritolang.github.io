#ifndef KIRITO_TENSOR_KERNELS_HPP
#define KIRITO_TENSOR_KERNELS_HPP

#include <cstddef>
#include <vector>

// Compute inner loops of the tensor engine, extracted into named, self-contained kernels that operate
// on raw contiguous buffers only — no Tensor, no VM, no Handles, no shape/stride objects beyond the
// plain geometry each one needs. This is the SIMD/GPU seam: a future backend swaps a kernel body (or
// adds a target-specific overload / `if constexpr` specialization) without touching the Tensor class or
// the public shape/stride contract. It is a COMPILE-TIME specialization seam, not a runtime functor
// swap.
//
// Every kernel is TEMPLATED ON ITS OP FUNCTOR (Op / Comb), never std::function or a function pointer:
// a std::function would add a per-element indirect call and defeat both inlining and autovectorization
// (the op must fuse into the loop body exactly as the pre-extraction one-line lambdas did). The op
// therefore stays zero-overhead and inlined into the counted loop over contiguous data.
//
// Two families, distinguished by their reordering contract:
//   * "embarrassingly parallel" (map/elementwise): independent lanes, no cross-element dependency, so a
//     future SIMD/GPU variant may process lanes in any order — safe to autovectorize.
//   * order-sensitive REFERENCE kernels (reductions, dot, matmul accumulation): they MUST fold strictly
//     left-to-right with the original grouping. Reproducibility and the autograd gradient contract
//     depend on the exact accumulation order (no tree/pairwise sum, no FMA-style reassociation). A
//     pairwise/tree SIMD variant is a FUTURE opt-in, never the default.
//
// These are pure extractions: each loop body is the verbatim arithmetic of the pre-extraction call site
// (same expression, same operand grouping, same iteration order), so results are byte-identical — NaN
// propagation, div-by-zero / math-domain throws (which live in the functor and fire at the first
// offending element, before any later write), and the discard-on-throw behaviour all carry over
// unchanged.

namespace kirito::tensor::kernels {

// ---- embarrassingly parallel (independent lanes) ---------------------------------------------

template <class T, class Op>
void mapUnaryContig(const T* a, T* out, std::size_t n, Op op) {
    for (std::size_t i = 0; i < n; ++i) out[i] = op(a[i]);
}

template <class T, class Op>
void ewiseBinaryContig(const T* a, const T* b, T* out, std::size_t n, Op op) {
    for (std::size_t i = 0; i < n; ++i) out[i] = op(a[i], b[i]);
}

// Ternary elementwise over three contiguous inputs — used by the Float-only math gradient, whose local
// derivative reads the input, the forward output, and the incoming gradient at each element.
template <class T, class Op>
void ewiseTernaryContig(const T* a, const T* b, const T* c, T* out, std::size_t n, Op op) {
    for (std::size_t i = 0; i < n; ++i) out[i] = op(a[i], b[i], c[i]);
}

// ---- order-sensitive reference kernels (strict left-to-right fold) ---------------------------

// Fold `comb` over the whole buffer starting from `init` (sum: init=0; prod: init=1; and min/max whole,
// which seed init on the first element and then fold the whole buffer including it — nanprop-safe).
template <class T, class Comb>
T reduceContig(const T* a, std::size_t n, Comb comb, T init) {
    T acc = init;
    for (std::size_t i = 0; i < n; ++i) acc = comb(acc, a[i]);
    return acc;
}

// Fold one strided axis for a single output cell. With an identity supplied, fold every element from it
// (numerically identical to seeding on the first); without one, seed on the first element and fold the
// rest. `base` points at the axis's element 0 for this cell; consecutive elements are `axisstep` apart.
template <class T, class Comb>
T reduceStrided(const T* base, std::size_t axislen, std::size_t axisstep, Comb comb, const T* identity) {
    T acc;
    std::size_t a;
    if (identity) { acc = *identity; a = 0; }
    else          { acc = base[0];   a = 1; }
    for (; a < axislen; ++a) acc = comb(acc, base[a * axisstep]);
    return acc;
}

// Dot product of two equal-length contiguous vectors (left-to-right accumulation).
template <class T>
T dotContig(const T* a, const T* b, std::size_t n) {
    T acc = T{};
    for (std::size_t i = 0; i < n; ++i) acc += a[i] * b[i];
    return acc;
}

// One matmul batch: O (m×n) += A (m×k) · B (k×n), ikj order so the innermost loop streams contiguous
// rows of B and O. O MUST be zero-initialized by the caller (the Tensor fill ctor does this). The ikj
// order — not ijk — is the accumulation order the gradient contract and the goldens are pinned to.
template <class T>
void matmulIKJ(const T* A, const T* B, T* O, std::size_t m, std::size_t k, std::size_t n) {
    for (std::size_t i = 0; i < m; ++i)
        for (std::size_t p = 0; p < k; ++p) {
            T v = A[i * k + p];
            for (std::size_t j = 0; j < n; ++j) O[i * n + j] += v * B[p * n + j];
        }
}

// ---- strided gather (memory movement) --------------------------------------------------------

// Single-pass strided gather: out[lin] = src[constOff + sum_d coord[d]*outStride[d]], walking an
// odometer over `outShape` (row-major, last axis fastest). Pure linear per-axis strides (no per-axis
// index indirection) — the basic-index read path. `outShape`/`outStride` have `rank` entries.
template <class T>
void gatherStrided(const T* src, T* out, std::size_t n, std::size_t rank,
                   const std::size_t* outShape, const std::ptrdiff_t* outStride,
                   std::ptrdiff_t constOff) {
    std::vector<std::size_t> coord(rank, 0);
    for (std::size_t lin = 0; lin < n; ++lin) {
        std::ptrdiff_t off = constOff;
        for (std::size_t d = 0; d < rank; ++d) off += static_cast<std::ptrdiff_t>(coord[d]) * outStride[d];
        out[lin] = src[static_cast<std::size_t>(off)];
        for (std::size_t d = rank; d-- > 0;) { if (++coord[d] < outShape[d]) break; coord[d] = 0; }
    }
}

}  // namespace kirito::tensor::kernels

#endif
