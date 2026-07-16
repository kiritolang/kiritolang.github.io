#ifndef KIRITO_STDLIB_TENSOR_HPP
#define KIRITO_STDLIB_TENSOR_HPP

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "fum/unordered_map.hpp"
#include "fum/unordered_set.hpp"
#include "builtins.hpp"
#include "collections.hpp"
#include "module.hpp"
#include "native.hpp"
#include "stdlib_complex.hpp"  // ComplexVal, cdouble, complexToString, cpx::asComplex
#include "tensor.hpp"

namespace kirito {

#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// ---- autograd graph node ----------------------------------------------------------------------
// Reverse-mode automatic differentiation, Float-only. A tensor produced by a differentiable op
// records how it was made: its grad-requiring inputs (`parents`, kept alive for the GC) and a
// `backward` rule that, given dL/d(this), returns dL/d(parent_i) for each parent — already reduced
// to that parent's shape. Leaves (user tensors marked `requiresgrad`) have no node. The graph
// records *operations*, never where the data lives, so a future GPU backend reuses it unchanged.
struct TensorGradNode {
    std::vector<Handle> parents;
    std::function<std::vector<tensor::Tensor<double>>(const tensor::Tensor<double>&)> backward;
};

// An N-dimensional tensor exposed to Kirito. The element type (dtype) is chosen at runtime: Float
// (double, the default) or Complex (std::complex<double>). The numeric work is the shared
// `kirito::tensor` engine; this class is the Kirito-facing wrapper + dispatch + autograd. (The
// engine is generic in T, so a third party can instantiate it for other element types in C++.)
class TensorVal : public NativeClass<TensorVal> {
public:
    static constexpr const char* kTypeName = "Tensor";
    using FT = tensor::Tensor<double>;
    using CT = tensor::Tensor<cdouble>;
    std::variant<FT, CT> store;  // index 0 = Float, 1 = Complex

    // autograd state (Float-only). `requiresGrad` marks a tensor as part of the differentiable
    // graph; `grad` accumulates dL/dself after backward(); `node` records the producing op (null
    // for a leaf). Complex tensors never carry grad.
    bool requiresGrad = false;
    std::optional<FT> grad;
    std::shared_ptr<TensorGradNode> node;

    TensorVal() = default;
    explicit TensorVal(FT t) : store(std::move(t)) {}
    explicit TensorVal(CT t) : store(std::move(t)) {}

    bool isComplex() const { return store.index() == 1; }
    const char* dtypeName() const { return isComplex() ? "Complex" : "Float"; }
    const tensor::Shape& shape() const { return isComplex() ? std::get<CT>(store).shape : std::get<FT>(store).shape; }
    std::size_t ndim() const { return shape().size(); }
    std::size_t size() const { return isComplex() ? std::get<CT>(store).size() : std::get<FT>(store).size(); }
    // every element promoted to a complex value (for cross-dtype comparison / promotion)
    cdouble elemAsComplex(std::size_t i) const {
        return isComplex() ? std::get<CT>(store).data[i] : cdouble(std::get<FT>(store).data[i], 0.0);
    }

    bool truthy() const override { return size() != 0; }

    // The autograd graph keeps its inputs reachable for the GC.
    void children(std::vector<Handle>& out) const override {
        if (node) for (Handle p : node->parents) out.push_back(p);
    }

    std::string str(StringifyCtx&) const override {
        if (isComplex()) return format(std::get<CT>(store), [](cdouble z) { return complexToString(z); });
        return format(std::get<FT>(store), [](double x) { return floatToString(x); });
    }
    bool equals(const ObjectArena&, const Object& other) const override {
        // Whole-tensor `==` is EXACT (same shape, every element bit-equal), like scalar Float ==.
        // Naturally NaN-aware (NaN != NaN). Tensors hold computed floats, so for a tolerant compare
        // (a solve/inv result vs its literal) use the `.compare(other, rel_tol, abs_tol)` method.
        const auto* o = dynamic_cast<const TensorVal*>(&other);
        if (!o || o->shape() != shape()) return false;
        for (std::size_t i = 0; i < size(); ++i)
            if (elemAsComplex(i) != o->elemAsComplex(i)) return false;
        return true;
    }
    std::vector<std::string> inspectMembers() const override {
        return {"shape() -> List", "ndim() -> Integer", "size() -> Integer", "dtype() -> String",
                "item() -> Number", "tolist() -> List",
                "compare(other, rel_tol = 1e-09, abs_tol = 0.0) -> Bool",
                "reshape(shape) -> Tensor", "transpose() -> Tensor", "permute(axes) -> Tensor",
                "flatten() -> Tensor", "apply(fn) -> Tensor", "astype(dtype) -> Tensor",
                "matmul(other) -> Tensor", "dot(other) -> Number",
                "sum(axis = None) -> Tensor", "mean(axis = None) -> Tensor",
                "prod(axis = None) -> Tensor", "min(axis = None) -> Tensor", "max(axis = None) -> Tensor",
                // axis reductions / statistics
                "argmin(axis = None) -> Tensor", "argmax(axis = None) -> Tensor",
                "std(axis = None, ddof = 0) -> Tensor", "var(axis = None, ddof = 0) -> Tensor",
                "all(axis = None) -> Bool", "any(axis = None) -> Bool", "ptp(axis = None) -> Tensor",
                "median(axis = None) -> Tensor", "cumsum(axis = None) -> Tensor", "cumprod(axis = None) -> Tensor",
                // comparisons / logic (return a 0/1 mask)
                "eq(other) -> Tensor", "ne(other) -> Tensor", "lt(other) -> Tensor", "le(other) -> Tensor",
                "gt(other) -> Tensor", "ge(other) -> Tensor",
                "logicaland(other) -> Tensor", "logicalor(other) -> Tensor", "logicalxor(other) -> Tensor", "logicalnot() -> Tensor",
                // selection / structural
                "clip(lo, hi) -> Tensor", "maximum(other) -> Tensor", "minimum(other) -> Tensor",
                "squeeze(axis = None) -> Tensor", "expanddims(axis) -> Tensor", "swapaxes(axis1, axis2) -> Tensor",
                "flip(axis = None) -> Tensor", "broadcastto(shape) -> Tensor", "repeat(count, axis = None) -> Tensor",
                "tile(reps) -> Tensor", "slice(start, stop, step, axis = 0) -> Tensor", "take(indices, axis = 0) -> Tensor",
                "sort(axis = None) -> Tensor", "argsort(axis = None) -> Tensor",
                // complex helpers
                "real() -> Tensor", "imag() -> Tensor", "conj() -> Tensor", "conjugate() -> Tensor", "angle() -> Tensor",
                // autograd
                "requiresgrad(flag = None) -> Bool", "grad: Tensor", "backward(seed = None) -> None",
                "zerograd() -> None", "detach() -> Tensor",
                // differentiable element-wise math
                "exp() -> Tensor", "log() -> Tensor", "log10() -> Tensor", "log2() -> Tensor",
                "sqrt() -> Tensor", "cbrt() -> Tensor", "square() -> Tensor", "pow(p) -> Tensor",
                "reciprocal() -> Tensor", "abs() -> Tensor", "sign() -> Tensor",
                "floor() -> Tensor", "ceil() -> Tensor", "round() -> Tensor", "trunc() -> Tensor",
                "sin() -> Tensor", "cos() -> Tensor", "tan() -> Tensor",
                "asin() -> Tensor", "acos() -> Tensor", "atan() -> Tensor",
                "sinh() -> Tensor", "cosh() -> Tensor", "tanh() -> Tensor",
                "asinh() -> Tensor", "acosh() -> Tensor", "atanh() -> Tensor",
                "relu() -> Tensor", "sigmoid() -> Tensor", "softplus() -> Tensor", "erf() -> Tensor"};
    }

    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override;
    Handle unary(KiritoVM& vm, UnOp op, Handle self) override;
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override;
    void setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) override;
    Handle slice(KiritoVM& vm, Handle start, Handle stop, Handle step) override;

    // N-D nested-bracket formatting, e.g. [[1.0, 2.0], [3.0, 4.0]].
    template <class T, class Fmt>
    static std::string format(const tensor::Tensor<T>& t, Fmt fmt) {
        if (t.ndim() == 0) return fmt(t.data.empty() ? T{} : t.data[0]);
        tensor::Shape st = t.strides();
        std::function<std::string(std::size_t, std::size_t)> rec = [&](std::size_t dim, std::size_t off) {
            std::string s = "[";
            for (std::size_t i = 0; i < t.shape[dim]; ++i) {
                if (i) s += ", ";
                if (dim + 1 == t.ndim()) s += fmt(t.data[off + i * st[dim]]);
                else s += rec(dim + 1, off + i * st[dim]);
            }
            return s + "]";
        };
        return rec(0, 0);
    }
};

// A per-VM grad-tracking flag, stored as a hidden member of the `tensor` module (so it is VM-scoped,
// not a global). `with tensor.nograd():` flips it off; the differentiable ops consult it.
class TensorGradFlag : public NativeClass<TensorGradFlag> {
public:
    static constexpr const char* kTypeName = "TensorGradFlag";
    bool enabled = true;
    fum::unordered_set<std::string> warned;  // op names that have already warned (warn-once per VM)
    std::string str(StringifyCtx&) const override { return "<tensor grad-mode>"; }
};

// The differentiable element-wise math ops. Each is a unary map with a known local derivative, so it
// participates in autograd.
enum class MathOp {
    Exp, Log, Log10, Log2, Sqrt, Cbrt, Square, Reciprocal, Abs, Sign,
    Sin, Cos, Tan, Asin, Acos, Atan,
    Sinh, Cosh, Tanh, Asinh, Acosh, Atanh,
    Relu, Sigmoid, Softplus, Erf,
    Floor, Ceil, Round, Trunc
};

namespace tns {

using FT = TensorVal::FT;
using CT = TensorVal::CT;

inline TensorVal& asT(KiritoVM& vm, Handle h) { return static_cast<TensorVal&>(vm.arena().deref(h)); }
// Checked downcast for public entry points that take a user value directly (no prior dynamic_cast).
inline TensorVal& reqT(KiritoVM& vm, Handle h, const char* who) {
    auto* t = dynamic_cast<TensorVal*>(&vm.arena().deref(h));
    if (!t) throw KiritoError(std::string(who) + " expects a Tensor");
    return *t;
}

inline constexpr std::size_t kMaxElems = tensor::kMaxElems;   // single-sourced from the engine cap
inline void checkSize(const tensor::Shape& s) {
    try { (void)tensor::checkedNumel(s); } catch (const tensor::TensorError& e) { throw KiritoError(e.what()); }
}

// Promote a Float tensor to Complex (reals on the real axis).
inline CT toComplex(const FT& t) {
    CT out(t.shape);
    for (std::size_t i = 0; i < t.data.size(); ++i) out.data[i] = cdouble(t.data[i], 0.0);
    return out;
}

inline Handle make(KiritoVM& vm, FT t) { checkSize(t.shape); return vm.alloc(std::make_unique<TensorVal>(std::move(t))); }
inline Handle make(KiritoVM& vm, CT t) { checkSize(t.shape); return vm.alloc(std::make_unique<TensorVal>(std::move(t))); }

// Wrap an engine call so its TensorError surfaces as a KiritoError.
template <class F>
auto wrap(F&& f) -> decltype(f()) {
    try { return f(); } catch (const tensor::TensorError& e) { throw KiritoError(e.what()); }
}

// Read a shape argument (a List of non-negative Integers).
inline tensor::Shape readShape(Value v) {
    tensor::Shape s;
    for (Value e : v.items()) {
        int64_t d = e.asInt("shape dimension");
        if (d < 0) throw KiritoError("Tensor shape dimensions must be non-negative");
        s.push_back(static_cast<std::size_t>(d));
    }
    return s;
}

// dtype string -> wants-complex flag (default Float).
inline bool wantsComplex(KiritoVM& vm, Handle h) {
    const Object& o = vm.arena().deref(h);
    if (o.kind() == ValueKind::None) return false;
    std::string d = Value(vm, h).asStringRef("dtype");
    if (d == "Float") return false;
    if (d == "Complex") return true;
    throw KiritoError("Tensor dtype must be \"Float\" or \"Complex\"");
}

// ---- grad-mode flag (VM-scoped, lives on the tensor module) ----------------------------------
inline TensorGradFlag& gradMode(KiritoVM& vm) {
    Handle m = vm.importModule("tensor");
    ModuleValue& mod = static_cast<ModuleValue&>(vm.arena().deref(m));
    auto it = mod.members.find("_grad");
    if (it == mod.members.end()) throw KiritoError("tensor: grad-mode flag missing");
    return static_cast<TensorGradFlag&>(vm.arena().deref(it->second));
}
inline bool gradEnabled(KiritoVM& vm) { return gradMode(vm).enabled; }

// Warn (once per op, per VM) when a NON-differentiable op silently detaches a grad-requiring tensor
// from the graph. Stays quiet when grad tracking is already off (inside nograd()) — that is explicit.
inline void warnDetach(KiritoVM& vm, const char* op, const TensorVal& in) {
    if (in.isComplex() || !in.requiresGrad) return;  // no gradient to break
    TensorGradFlag& gm = gradMode(vm);
    if (!gm.enabled) return;                          // tracking already off -> the user opted out
    if (gm.warned.insert(op).second)
        std::cerr << "warning: tensor " << op << " is not differentiable; its result is detached from "
                     "the gradient graph (call detach() or use nograd() to do this intentionally; "
                     "this warning fires once per operation)\n";
}

// Tracking is on, and at least one Float input requires grad.
inline bool wantsGrad(KiritoVM& vm, std::initializer_list<const TensorVal*> ins) {
    if (!gradEnabled(vm)) return false;
    for (const TensorVal* t : ins) if (t && !t->isComplex() && t->requiresGrad) return true;
    return false;
}

// ---- gradient shape helpers ------------------------------------------------------------------
// Sum a gradient back to a (broadcast-from) shape: undo NumPy broadcasting by summing the expanded
// axes (PyTorch's "sum_to"). Leading extra axes are summed away; size-1 axes are summed with keepdim.
inline FT sumTo(FT g, const tensor::Shape& target) {
    auto plus = [](double x, double y) { return x + y; };
    while (g.shape.size() > target.size()) g = tensor::reduceAxis(g, 0, plus);
    for (std::size_t i = 0; i < target.size(); ++i) {
        if (target[i] == 1 && g.shape[i] != 1) {
            FT r = tensor::reduceAxis(g, i, plus);
            tensor::Shape ns = g.shape; ns[i] = 1;
            g = tensor::reshape(r, ns);
        }
    }
    if (g.shape != target) g = tensor::reshape(g, target);
    return g;
}

// Expand a tensor to a (broadcast-compatible) target shape.
inline FT broadcastTo(const FT& t, const tensor::Shape& target) { return tensor::add(FT(target, 0.0), t); }

// Swap the last two axes (the batched matrix transpose).
inline FT transposeLast2(const FT& t) {
    if (t.ndim() < 2) throw tensor::TensorError("transpose of last two axes needs rank >= 2");
    tensor::Shape ax(t.ndim());
    for (std::size_t i = 0; i < t.ndim(); ++i) ax[i] = i;
    std::swap(ax[t.ndim() - 1], ax[t.ndim() - 2]);
    return tensor::permute(t, ax);
}

// Allocate a Float tensor and wire its grad node (only the caller's grad-requiring inputs become
// parents; `bw` must return one gradient per parent, in order).
inline Handle makeAutogradFloat(KiritoVM& vm, FT out, std::vector<Handle> parents,
                                std::function<std::vector<FT>(const FT&)> bw) {
    RootScope rs(vm);
    for (Handle p : parents) rs.add(p);
    Handle h = make(vm, std::move(out));
    TensorVal& tv = asT(vm, h);
    tv.requiresGrad = true;
    tv.node = std::make_shared<TensorGradNode>();
    tv.node->parents = std::move(parents);
    tv.node->backward = std::move(bw);
    return h;
}

// ---- differentiable primitive ops (Float, grad-aware; Complex routes through with no grad) -----

inline Handle g_binop(KiritoVM& vm, char op, Handle ah, Handle bh) {
    TensorVal& A = asT(vm, ah);
    TensorVal& B = asT(vm, bh);
    const FT& a = std::get<FT>(A.store);
    const FT& b = std::get<FT>(B.store);
    FT out = op == '+' ? tensor::add(a, b) : op == '-' ? tensor::sub(a, b)
           : op == '*' ? tensor::mul(a, b) : tensor::div(a, b);
    if (!wantsGrad(vm, {&A, &B})) return make(vm, std::move(out));
    bool ag = A.requiresGrad, bg = B.requiresGrad;
    tensor::Shape ashape = a.shape, bshape = b.shape;
    FT acopy, bcopy;
    if (op == '*' || op == '/') { acopy = a; bcopy = b; }
    std::vector<Handle> parents;
    if (ag) parents.push_back(ah);
    if (bg) parents.push_back(bh);
    auto bw = [op, ag, bg, ashape, bshape, acopy, bcopy](const FT& g) -> std::vector<FT> {
        std::vector<FT> r;
        if (op == '+') {
            if (ag) r.push_back(sumTo(g, ashape));
            if (bg) r.push_back(sumTo(g, bshape));
        } else if (op == '-') {
            if (ag) r.push_back(sumTo(g, ashape));
            if (bg) r.push_back(sumTo(tensor::negate(g), bshape));
        } else if (op == '*') {
            if (ag) r.push_back(sumTo(tensor::mul(g, bcopy), ashape));
            if (bg) r.push_back(sumTo(tensor::mul(g, acopy), bshape));
        } else {  // '/'  d(a/b): da = g/b ; db = -g*a/b^2
            if (ag) r.push_back(sumTo(tensor::div(g, bcopy), ashape));
            if (bg) r.push_back(sumTo(tensor::negate(tensor::div(tensor::mul(g, acopy), tensor::mul(bcopy, bcopy))), bshape));
        }
        return r;
    };
    return makeAutogradFloat(vm, std::move(out), std::move(parents), std::move(bw));
}

inline Handle g_scalar(KiritoVM& vm, char op, Handle ah, double s) {
    TensorVal& A = asT(vm, ah);
    const FT& a = std::get<FT>(A.store);
    FT out = tensor::scalarOp(a, s, op);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [op, s](const FT& g) -> std::vector<FT> {
        FT da = (op == '+' || op == '-') ? g : (op == '*') ? tensor::scalarOp(g, s, '*') : tensor::scalarOp(g, s, '/');
        return {da};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_neg(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    const FT& a = std::get<FT>(A.store);
    FT out = tensor::negate(a);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [](const FT& g) -> std::vector<FT> { return {tensor::negate(g)}; };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_matmul(KiritoVM& vm, Handle ah, Handle bh) {
    TensorVal& A = asT(vm, ah);
    TensorVal& B = asT(vm, bh);
    if (A.isComplex() || B.isComplex()) {  // complex matmul: no grad
        CT x = A.isComplex() ? std::get<CT>(A.store) : toComplex(std::get<FT>(A.store));
        CT y = B.isComplex() ? std::get<CT>(B.store) : toComplex(std::get<FT>(B.store));
        return make(vm, tensor::matmul(x, y));
    }
    const FT& a = std::get<FT>(A.store);
    const FT& b = std::get<FT>(B.store);
    FT out = tensor::matmul(a, b);
    if (!wantsGrad(vm, {&A, &B})) return make(vm, std::move(out));
    bool ag = A.requiresGrad, bg = B.requiresGrad;
    tensor::Shape ashape = a.shape, bshape = b.shape;
    FT acopy = a, bcopy = b;
    std::vector<Handle> parents;
    if (ag) parents.push_back(ah);
    if (bg) parents.push_back(bh);
    auto bw = [ag, bg, ashape, bshape, acopy, bcopy](const FT& g) -> std::vector<FT> {
        std::vector<FT> r;
        if (ag) r.push_back(sumTo(tensor::matmul(g, transposeLast2(bcopy)), ashape));
        if (bg) r.push_back(sumTo(tensor::matmul(transposeLast2(acopy), g), bshape));
        return r;
    };
    return makeAutogradFloat(vm, std::move(out), std::move(parents), std::move(bw));
}

inline Handle g_transpose(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) return make(vm, tensor::transpose(std::get<CT>(A.store)));
    const FT& a = std::get<FT>(A.store);
    FT out = tensor::transpose(a);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [](const FT& g) -> std::vector<FT> { return {tensor::transpose(g)}; };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_permute(KiritoVM& vm, Handle ah, tensor::Shape axes) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) return make(vm, tensor::permute(std::get<CT>(A.store), axes));
    const FT& a = std::get<FT>(A.store);
    FT out = tensor::permute(a, axes);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    tensor::Shape inv(axes.size());
    for (std::size_t i = 0; i < axes.size(); ++i) inv[axes[i]] = i;
    auto bw = [inv](const FT& g) -> std::vector<FT> { return {tensor::permute(g, inv)}; };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_reshape(KiritoVM& vm, Handle ah, tensor::Shape ns) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) return make(vm, tensor::reshape(std::get<CT>(A.store), ns));
    const FT& a = std::get<FT>(A.store);
    tensor::Shape ashape = a.shape;
    FT out = tensor::reshape(a, ns);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [ashape](const FT& g) -> std::vector<FT> { return {tensor::reshape(g, ashape)}; };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_flatten(KiritoVM& vm, Handle ah) { return g_reshape(vm, ah, tensor::Shape{asT(vm, ah).size()}); }

inline Handle g_sum(KiritoVM& vm, Handle ah, int64_t axis) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) {
        const CT& c = std::get<CT>(A.store);
        if (axis < 0) return cpx::make(vm, tensor::sumAll(c));
        return make(vm, tensor::reduceAxis(c, static_cast<std::size_t>(axis), [](cdouble x, cdouble y) { return x + y; }, cdouble(0.0, 0.0)));
    }
    const FT& a = std::get<FT>(A.store);
    tensor::Shape ashape = a.shape;
    if (axis < 0) {  // whole-tensor sum
        double total = tensor::sumAll(a);
        if (!wantsGrad(vm, {&A})) return vm.makeFloat(total);  // non-grad: a scalar Float (as before)
        FT out(tensor::Shape{}, std::vector<double>{total});   // grad: a 0-D tensor, keeps the graph
        auto bw = [ashape](const FT& g) -> std::vector<FT> {
            return {FT(ashape, g.data.empty() ? 0.0 : g.data[0])};
        };
        return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
    }
    std::size_t ax = static_cast<std::size_t>(axis);
    FT out = tensor::reduceAxis(a, ax, [](double x, double y) { return x + y; }, 0.0);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [ashape, ax](const FT& g) -> std::vector<FT> {
        tensor::Shape keep = ashape; keep[ax] = 1;
        return {broadcastTo(tensor::reshape(g, keep), ashape)};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_mean(KiritoVM& vm, Handle ah, int64_t axis) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) {
        const CT& c = std::get<CT>(A.store);
        if (axis < 0) {
            double n = static_cast<double>(c.data.size());
            if (n == 0) throw KiritoError("mean of an empty tensor");
            return cpx::make(vm, tensor::sumAll(c) / cdouble(n, 0.0));
        }
        std::size_t ax = static_cast<std::size_t>(axis);
        double cnt = static_cast<double>(c.shape[ax]);
        CT s = tensor::reduceAxis(c, ax, [](cdouble x, cdouble y) { return x + y; });
        for (auto& z : s.data) z /= cdouble(cnt, 0.0);
        return make(vm, std::move(s));
    }
    const FT& a = std::get<FT>(A.store);
    tensor::Shape ashape = a.shape;
    if (axis < 0) {
        double n = static_cast<double>(a.size());
        if (n == 0) throw KiritoError("mean of an empty tensor");
        double mean = tensor::sumAll(a) / n;
        if (!wantsGrad(vm, {&A})) return vm.makeFloat(mean);
        FT out(tensor::Shape{}, std::vector<double>{mean});
        auto bw = [ashape, n](const FT& g) -> std::vector<FT> {
            return {FT(ashape, (g.data.empty() ? 0.0 : g.data[0]) / n)};
        };
        return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
    }
    std::size_t ax = static_cast<std::size_t>(axis);
    double cnt = static_cast<double>(a.shape[ax]);
    FT s = tensor::reduceAxis(a, ax, [](double x, double y) { return x + y; });
    for (auto& v : s.data) v /= cnt;
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(s));
    auto bw = [ashape, ax, cnt](const FT& g) -> std::vector<FT> {
        tensor::Shape keep = ashape; keep[ax] = 1;
        FT b = broadcastTo(tensor::reshape(g, keep), ashape);
        for (auto& v : b.data) v /= cnt;
        return {b};
    };
    return makeAutogradFloat(vm, std::move(s), {ah}, std::move(bw));
}

// Forward value of an element-wise math op.
inline double mathForward(MathOp k, double x) {
    switch (k) {
        case MathOp::Exp: { return std::exp(x); } break;
        case MathOp::Log: { return std::log(x); } break;
        case MathOp::Log10: { return std::log10(x); } break;
        case MathOp::Log2: { return std::log2(x); } break;
        case MathOp::Sqrt: { return std::sqrt(x); } break;
        case MathOp::Cbrt: { return std::cbrt(x); } break;
        case MathOp::Square: { return x * x; } break;
        case MathOp::Reciprocal: { return 1.0 / x; } break;
        case MathOp::Abs: { return std::fabs(x); } break;
        case MathOp::Sign: { return (x > 0) - (x < 0); } break;
        case MathOp::Sin: { return std::sin(x); } break;
        case MathOp::Cos: { return std::cos(x); } break;
        case MathOp::Tan: { return std::tan(x); } break;
        case MathOp::Asin: { return std::asin(x); } break;
        case MathOp::Acos: { return std::acos(x); } break;
        case MathOp::Atan: { return std::atan(x); } break;
        case MathOp::Sinh: { return std::sinh(x); } break;
        case MathOp::Cosh: { return std::cosh(x); } break;
        case MathOp::Tanh: { return std::tanh(x); } break;
        case MathOp::Asinh: { return std::asinh(x); } break;
        case MathOp::Acosh: { return std::acosh(x); } break;
        case MathOp::Atanh: { return std::atanh(x); } break;
        case MathOp::Relu: { return x > 0 ? x : 0.0; } break;
        case MathOp::Sigmoid: { return 1.0 / (1.0 + std::exp(-x)); } break;
        // Numerically stable softplus: naive log1p(exp(x)) overflows to inf for x > ~709, poisoning
        // the forward pass (the derivative is already stable). max(x,0)+log1p(exp(-|x|)) is exact
        // and is what NumPy/PyTorch use.
        case MathOp::Softplus: { return std::max(x, 0.0) + std::log1p(std::exp(-std::abs(x))); } break;
        case MathOp::Erf: { return std::erf(x); } break;
        case MathOp::Floor: { return std::floor(x); } break;
        case MathOp::Ceil: { return std::ceil(x); } break;
        case MathOp::Round: { return std::nearbyint(x); } break;
        case MathOp::Trunc: { return std::trunc(x); } break;
    }
    return x;
}

// Local derivative dout/dx, given x and o = mathForward(k, x).
inline double mathDeriv(MathOp k, double x, double o) {
    static const double kInvSqrtPi2 = 1.1283791670955126;  // 2/sqrt(pi)
    static const double kLn10 = 2.302585092994046, kLn2 = 0.6931471805599453;
    switch (k) {
        case MathOp::Exp: { return o; } break;
        case MathOp::Log: { return 1.0 / x; } break;
        case MathOp::Log10: { return 1.0 / (x * kLn10); } break;
        case MathOp::Log2: { return 1.0 / (x * kLn2); } break;
        case MathOp::Sqrt: { return 0.5 / o; } break;
        case MathOp::Cbrt: { return 1.0 / (3.0 * o * o); } break;
        case MathOp::Square: { return 2.0 * x; } break;
        case MathOp::Reciprocal: { return -o * o; } break;
        case MathOp::Abs: { return (x > 0) - (x < 0); } break;
        case MathOp::Sign: { return 0.0; } break;
        case MathOp::Sin: { return std::cos(x); } break;
        case MathOp::Cos: { return -std::sin(x); } break;
        case MathOp::Tan: { return 1.0 + o * o; } break;
        case MathOp::Asin: { return 1.0 / std::sqrt(1.0 - x * x); } break;
        case MathOp::Acos: { return -1.0 / std::sqrt(1.0 - x * x); } break;
        case MathOp::Atan: { return 1.0 / (1.0 + x * x); } break;
        case MathOp::Sinh: { return std::cosh(x); } break;
        case MathOp::Cosh: { return std::sinh(x); } break;
        case MathOp::Tanh: { return 1.0 - o * o; } break;
        case MathOp::Asinh: { return 1.0 / std::sqrt(x * x + 1.0); } break;
        case MathOp::Acosh: { return 1.0 / std::sqrt(x * x - 1.0); } break;
        case MathOp::Atanh: { return 1.0 / (1.0 - x * x); } break;
        case MathOp::Relu: { return x > 0 ? 1.0 : 0.0; } break;
        case MathOp::Sigmoid: { return o * (1.0 - o); } break;
        case MathOp::Softplus: { return 1.0 / (1.0 + std::exp(-x)); } break;
        case MathOp::Erf: { return kInvSqrtPi2 * std::exp(-x * x); } break;
        case MathOp::Floor: case MathOp::Ceil: case MathOp::Round: case MathOp::Trunc: { return 0.0; } break;
    }
    return 1.0;
}

// Reject out-of-domain elements before an element-wise math op, instead of letting std:: emit a
// silent NaN/inf into the data (and poison gradients). Mirrors the scalar `math` module's policy and
// the tensor engine's own div-by-zero guard, with which the unguarded math ops were inconsistent. A
// NaN element passes through (like the scalar math module); genuine overflow-to-inf is not a domain
// error. Only the ops with a real domain restriction are listed; everything else is total.
inline void checkMathDomain(MathOp k, double x) {
    if (std::isnan(x)) return;
    switch (k) {
        case MathOp::Log: case MathOp::Log10: case MathOp::Log2: {
            if (x <= 0.0) throw KiritoError("tensor log: math domain error (got " + floatToString(x) + ")");
        } break;
        case MathOp::Sqrt: {
            if (x < 0.0) throw KiritoError("tensor sqrt: math domain error (got " + floatToString(x) + ")");
        } break;
        case MathOp::Asin: {
            if (x < -1.0 || x > 1.0) throw KiritoError("tensor asin: math domain error (got " + floatToString(x) + ")");
        } break;
        case MathOp::Acos: {
            if (x < -1.0 || x > 1.0) throw KiritoError("tensor acos: math domain error (got " + floatToString(x) + ")");
        } break;
        case MathOp::Acosh: {
            if (x < 1.0) throw KiritoError("tensor acosh: math domain error (got " + floatToString(x) + ")");
        } break;
        case MathOp::Atanh: {
            if (x <= -1.0 || x >= 1.0) throw KiritoError("tensor atanh: math domain error (got " + floatToString(x) + ")");
        } break;
        case MathOp::Reciprocal: {
            if (x == 0.0) throw KiritoError("tensor division by zero (reciprocal of 0)");
        } break;
        default: { } break;
    }
}

// pow domain: a negative base with a non-integer exponent (a complex result) or zero to a negative
// power (a pole) are domain errors, matching the scalar `math.pow` guard.
inline void checkPowDomain(double base, double p) {
    if (std::isnan(base) || std::isnan(p)) return;
    if (base < 0.0 && p != std::floor(p))
        throw KiritoError("tensor pow: math domain error (negative base " + floatToString(base) +
                          " with a non-integer exponent " + floatToString(p) + ")");
    if (base == 0.0 && p < 0.0)
        throw KiritoError("tensor pow: math domain error (zero to a negative power)");
}

inline Handle g_math(KiritoVM& vm, Handle ah, MathOp k) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex())
        throw KiritoError("this math op is Float-only on tensors (use the complex module for complex math)");
    const FT& a = std::get<FT>(A.store);
    FT out = tensor::mapUnary(a, [k](double x) { checkMathDomain(k, x); return mathForward(k, x); });
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    FT acopy = a, outcopy = out;
    auto bw = [k, acopy, outcopy](const FT& g) -> std::vector<FT> {
        FT d(acopy.shape);
        for (std::size_t i = 0; i < d.data.size(); ++i)
            d.data[i] = g.data[i] * mathDeriv(k, acopy.data[i], outcopy.data[i]);
        return {d};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_pow(KiritoVM& vm, Handle ah, double p) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) throw KiritoError("pow is Float-only on tensors");
    const FT& a = std::get<FT>(A.store);
    FT out = tensor::mapUnary(a, [p](double x) { checkPowDomain(x, p); return std::pow(x, p); });
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    FT acopy = a;
    auto bw = [acopy, p](const FT& g) -> std::vector<FT> {
        FT d(acopy.shape);
        for (std::size_t i = 0; i < d.data.size(); ++i)
            d.data[i] = g.data[i] * p * std::pow(acopy.data[i], p - 1.0);
        return {d};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

// ---- tensordot / contract (built from the differentiable primitives, so it is differentiable) --
inline Handle tensordot(KiritoVM& vm, Handle ah, Handle bh,
                        const std::vector<int64_t>& aaxes, const std::vector<int64_t>& baxes) {
    TensorVal& A = asT(vm, ah);
    TensorVal& B = asT(vm, bh);
    std::size_t an = A.ndim(), bn = B.ndim();
    if (aaxes.size() != baxes.size()) throw KiritoError("tensordot: the two axis lists must have equal length");
    auto norm = [](int64_t ax, std::size_t nd) -> std::size_t {
        if (ax < 0) ax += static_cast<int64_t>(nd);
        if (ax < 0 || static_cast<std::size_t>(ax) >= nd) throw KiritoError("tensordot: axis out of range");
        return static_cast<std::size_t>(ax);
    };
    std::vector<std::size_t> ac, bc;
    for (std::size_t i = 0; i < aaxes.size(); ++i) { ac.push_back(norm(aaxes[i], an)); bc.push_back(norm(baxes[i], bn)); }
    const tensor::Shape& ashape = A.shape();
    const tensor::Shape& bshape = B.shape();
    for (std::size_t i = 0; i < ac.size(); ++i)
        if (ashape[ac[i]] != bshape[bc[i]]) throw KiritoError("tensordot: contracted dimensions do not match");
    std::vector<bool> aIsC(an, false), bIsC(bn, false);
    for (std::size_t x : ac) aIsC[x] = true;
    for (std::size_t x : bc) bIsC[x] = true;
    // a -> [free_a..., contracted...]; b -> [contracted..., free_b...]
    tensor::Shape aperm, bperm;
    std::vector<std::size_t> afree, bfree;
    for (std::size_t i = 0; i < an; ++i) if (!aIsC[i]) { aperm.push_back(i); afree.push_back(i); }
    for (std::size_t x : ac) aperm.push_back(x);
    for (std::size_t x : bc) bperm.push_back(x);
    for (std::size_t i = 0; i < bn; ++i) if (!bIsC[i]) { bperm.push_back(i); bfree.push_back(i); }
    std::size_t M = 1, K = 1, N = 1;
    for (std::size_t i : afree) M *= ashape[i];
    for (std::size_t i : ac) K *= ashape[i];
    for (std::size_t i : bfree) N *= bshape[i];
    RootScope rs(vm);
    Handle ap = rs.add(g_permute(vm, ah, aperm));
    Handle ar = rs.add(g_reshape(vm, ap, tensor::Shape{M, K}));
    Handle bp = rs.add(g_permute(vm, bh, bperm));
    Handle br = rs.add(g_reshape(vm, bp, tensor::Shape{K, N}));
    Handle mm = rs.add(g_matmul(vm, ar, br));  // (M, N)
    tensor::Shape outshape;
    for (std::size_t i : afree) outshape.push_back(ashape[i]);
    for (std::size_t i : bfree) outshape.push_back(bshape[i]);
    return g_reshape(vm, mm, outshape);  // outshape may be empty -> a 0-D scalar tensor
}

// ---- reverse-mode backward pass --------------------------------------------------------------
inline void accumulateGrad(TensorVal& t, const FT& g) {
    if (!t.grad) t.grad = g;
    else t.grad = tensor::add(*t.grad, g);
}

inline void runBackward(KiritoVM& vm, Handle root, std::optional<FT> seed) {
    TensorVal& R = asT(vm, root);
    if (R.isComplex()) throw KiritoError("backward: gradients are Float-only");
    if (!R.requiresGrad) throw KiritoError("backward: this tensor does not require grad");
    FT s;
    if (seed) {
        s = *seed;
        if (s.shape != R.shape()) throw KiritoError("backward: the seed gradient shape must match the tensor");
    } else {
        if (R.size() != 1) throw KiritoError("backward: a seed gradient is required for a non-scalar tensor");
        s = FT(R.shape(), 1.0);
    }
    // reverse-topological order via an ITERATIVE post-order DFS (parents first, node last). An
    // explicit work-stack — instead of native recursion — so a very deep graph (e.g. a long unrolled
    // sequence) can't overflow the C++ stack and segfault uncatchably; matches the parser/VM depth policy.
    std::vector<Handle> order;
    fum::unordered_set<Handle> seen;
    std::vector<std::pair<Handle, std::size_t>> stk;  // (node, index of the next parent to descend into)
    if (seen.insert(root).second) stk.push_back({root, 0});
    while (!stk.empty()) {
        Handle h = stk.back().first;
        std::size_t pi = stk.back().second;
        TensorVal& tv = asT(vm, h);
        if (tv.node && pi < tv.node->parents.size()) {
            stk.back().second = pi + 1;               // advance THIS frame before descending
            Handle p = tv.node->parents[pi];
            if (seen.insert(p).second) stk.push_back({p, 0});
            continue;
        }
        order.push_back(h);                           // all parents emitted -> emit node, pop
        stk.pop_back();
    }
    // Non-leaf nodes use their own `.grad` as the reverse-mode accumulator, so a stale value from a
    // prior backward() through a retained intermediate would be re-seeded and re-propagated (doubling
    // the leaf gradient). Clear grad on every node that HAS a node (all non-leaves, incl. a non-leaf
    // root) before seeding — PyTorch semantics: non-leaves don't retain grad. Leaf tensors (node ==
    // null) are left untouched, so leaf accumulation across calls (relied on by the tests) is intact.
    for (Handle h : order) { TensorVal& tv = asT(vm, h); if (tv.node) tv.grad.reset(); }
    accumulateGrad(R, s);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        TensorVal& tv = asT(vm, *it);
        if (!tv.node || !tv.grad) continue;
        std::vector<FT> contrib = tv.node->backward(*tv.grad);
        for (std::size_t i = 0; i < tv.node->parents.size() && i < contrib.size(); ++i)
            accumulateGrad(asT(vm, tv.node->parents[i]), contrib[i]);
    }
}

// Parse an axis argument that may be a single Integer or a List of Integers.
inline std::vector<int64_t> readAxisList(Value v) {
    std::vector<int64_t> out;
    if (v.isInt()) { out.push_back(v.asInt("axis")); return out; }
    if (v.isList()) { for (Value e : v.items()) out.push_back(e.asInt("axis")); return out; }
    throw KiritoError("tensordot: axes must be an Integer or a List of Integers");
}

// ========================================================================= NumPy-style extensions

inline const FT& reqFloat(const TensorVal& t, const char* who) {
    if (t.isComplex()) throw KiritoError(std::string(who) + ": Float tensors only (this op is not defined for Complex)");
    return std::get<FT>(t.store);
}

// --- slicing / fancy indexing ---

// A resolved single-axis slice (stop exclusive in the walk direction).
struct SliceRange { std::ptrdiff_t start, stop, step; };
inline SliceRange resolveSlice(KiritoVM& vm, Handle sH, Handle eH, Handle stH, std::size_t len) {
    std::ptrdiff_t n = static_cast<std::ptrdiff_t>(len);
    auto opt = [&](Handle h, bool& has) -> std::ptrdiff_t {
        if (vm.arena().deref(h).kind() == ValueKind::None) { has = false; return 0; }
        has = true; return static_cast<std::ptrdiff_t>(Value(vm, h).asInt("slice bound"));
    };
    bool hasStep, hasS, hasE;
    std::ptrdiff_t step = opt(stH, hasStep); if (!hasStep) step = 1;
    if (step == 0) throw KiritoError("slice step cannot be zero");
    std::ptrdiff_t s = opt(sH, hasS), e = opt(eH, hasE);
    auto wrap = [&](std::ptrdiff_t i) { return i < 0 ? i + n : i; };
    if (step > 0) {
        s = hasS ? std::clamp<std::ptrdiff_t>(wrap(s), 0, n) : 0;
        e = hasE ? std::clamp<std::ptrdiff_t>(wrap(e), 0, n) : n;
    } else {
        s = hasS ? std::clamp<std::ptrdiff_t>(wrap(s), -1, n - 1) : n - 1;
        e = hasE ? std::clamp<std::ptrdiff_t>(wrap(e), -1, n - 1) : -1;
    }
    return {s, e, step};
}

// Differentiable single-axis strided slice (backward scatters the gradient back).
inline Handle g_sliceAxis(KiritoVM& vm, Handle ah, std::size_t axis, SliceRange r) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) return make(vm, tensor::sliceAxis(std::get<CT>(A.store), axis, r.start, r.stop, r.step));
    const FT& a = std::get<FT>(A.store);
    tensor::Shape ash = a.shape;
    FT out = tensor::sliceAxis(a, axis, r.start, r.stop, r.step);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    std::vector<std::ptrdiff_t> picks = tensor::slicePicks(r.start, r.stop, r.step);
    auto bw = [ash, axis, picks](const FT& g) -> std::vector<FT> {
        FT d(ash);
        tensor::Shape ost = tensor::rowMajorStrides(ash);
        for (std::size_t lin = 0; lin < g.data.size(); ++lin) {
            tensor::Shape c = tensor::unravel(lin, g.shape);
            std::size_t off = 0;
            for (std::size_t dd = 0; dd < ash.size(); ++dd)
                off += (dd == axis ? static_cast<std::size_t>(picks[c[dd]]) : c[dd]) * ost[dd];
            d.data[off] += g.data[lin];
        }
        return {d};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

// Differentiable gather of rows along axis 0 (fancy / boolean indexing); backward scatter-adds.
inline Handle g_takeAxis0(KiritoVM& vm, Handle ah, const std::vector<std::ptrdiff_t>& idx) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) return make(vm, tensor::takeAxis0(std::get<CT>(A.store), idx));
    const FT& a = std::get<FT>(A.store);
    tensor::Shape ash = a.shape;
    FT out = tensor::takeAxis0(a, idx);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [ash, idx](const FT& g) -> std::vector<FT> {
        FT d(ash);
        std::size_t block = d.data.size() / (ash[0] ? ash[0] : 1);
        std::ptrdiff_t dim = static_cast<std::ptrdiff_t>(ash[0]);
        for (std::size_t i = 0; i < idx.size(); ++i) {
            std::ptrdiff_t r = idx[i];
            if (r < 0) r += dim;   // mirror takeAxis0's negative-index normalization — else a far-OOB scatter
            for (std::size_t j = 0; j < block; ++j)
                d.data[static_cast<std::size_t>(r) * block + j] += g.data[i * block + j];
        }
        return {d};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

// --- elementwise comparisons / logic (Float-only, return a 0/1 mask) ---
inline double cmp(BinOp op, double x, double y) {
    switch (op) {
        case BinOp::Lt: { return x < y; } break; case BinOp::Le: { return x <= y; } break;
        case BinOp::Gt: { return x > y; } break; case BinOp::Ge: { return x >= y; } break;
        case BinOp::Eq: { return x == y; } break; case BinOp::Ne: { return x != y; } break;
        default: { return 0.0; } break;
    }
}
inline Handle compareTensors(KiritoVM& vm, BinOp op, const FT& a, const FT& b) {
    return make(vm, tensor::elementwise(a, b, [op](double x, double y) { return cmp(op, x, y); }));
}
inline Handle compareScalar(KiritoVM& vm, BinOp op, const FT& a, double s) {
    return make(vm, tensor::mapUnary(a, [op, s](double x) { return cmp(op, x, s); }));
}

// --- where / clip / maximum / minimum (differentiable) ---
inline Handle g_where(KiritoVM& vm, Handle cH, Handle aH, Handle bH) {
    TensorVal& C = asT(vm, cH); TensorVal& A = asT(vm, aH); TensorVal& B = asT(vm, bH);
    const FT& c = reqFloat(C, "where"); const FT& a = reqFloat(A, "where"); const FT& b = reqFloat(B, "where");
    tensor::Shape os = tensor::broadcastShapes(tensor::broadcastShapes(a.shape, b.shape), c.shape);
    FT ce = broadcastTo(c, os), ae = broadcastTo(a, os), be = broadcastTo(b, os);
    FT out(os);
    for (std::size_t i = 0; i < out.data.size(); ++i) out.data[i] = ce.data[i] != 0.0 ? ae.data[i] : be.data[i];
    bool ag = A.requiresGrad, bg = B.requiresGrad;
    if (!wantsGrad(vm, {&A, &B})) return make(vm, std::move(out));
    tensor::Shape ash = a.shape, bsh = b.shape;
    FT mask = ce; for (auto& m : mask.data) m = m != 0.0 ? 1.0 : 0.0;
    std::vector<Handle> parents; if (ag) parents.push_back(aH); if (bg) parents.push_back(bH);
    auto bw = [ag, bg, ash, bsh, mask](const FT& g) -> std::vector<FT> {
        std::vector<FT> r;
        if (ag) r.push_back(sumTo(tensor::mul(g, mask), ash));
        if (bg) { FT inv = mask; for (auto& m : inv.data) m = 1.0 - m; r.push_back(sumTo(tensor::mul(g, inv), bsh)); }
        return r;
    };
    return makeAutogradFloat(vm, std::move(out), std::move(parents), std::move(bw));
}

inline Handle g_clip(KiritoVM& vm, Handle ah, double lo, double hi) {
    TensorVal& A = asT(vm, ah);
    const FT& a = reqFloat(A, "clip");
    if (lo > hi) throw KiritoError("tensor clip: lower bound " + floatToString(lo) +
                                   " is greater than upper bound " + floatToString(hi));
    FT out = tensor::mapUnary(a, [lo, hi](double x) { return x < lo ? lo : (x > hi ? hi : x); });
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    FT acopy = a;
    auto bw = [acopy, lo, hi](const FT& g) -> std::vector<FT> {
        FT d(acopy.shape);
        for (std::size_t i = 0; i < d.data.size(); ++i) { double x = acopy.data[i]; d.data[i] = (x >= lo && x <= hi) ? g.data[i] : 0.0; }
        return {d};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

inline Handle g_maxmin(KiritoVM& vm, Handle ah, Handle bh, bool isMax) {
    TensorVal& A = asT(vm, ah); TensorVal& B = asT(vm, bh);
    const FT& a = reqFloat(A, "maximum/minimum"); const FT& b = reqFloat(B, "maximum/minimum");
    FT out = tensor::elementwise(a, b, [isMax](double x, double y) { return isMax ? std::max(x, y) : std::min(x, y); });
    if (!wantsGrad(vm, {&A, &B})) return make(vm, std::move(out));
    bool ag = A.requiresGrad, bg = B.requiresGrad;
    tensor::Shape ash = a.shape, bsh = b.shape;
    FT acopy = a, bcopy = b;
    std::vector<Handle> parents; if (ag) parents.push_back(ah); if (bg) parents.push_back(bh);
    auto bw = [ag, bg, ash, bsh, acopy, bcopy, isMax](const FT& g) -> std::vector<FT> {
        FT ae = broadcastTo(acopy, g.shape), be = broadcastTo(bcopy, g.shape);
        FT da(g.shape), db(g.shape);
        for (std::size_t i = 0; i < g.data.size(); ++i) {
            bool aWins = isMax ? (ae.data[i] >= be.data[i]) : (ae.data[i] <= be.data[i]);
            da.data[i] = aWins ? g.data[i] : 0.0;
            db.data[i] = aWins ? 0.0 : g.data[i];
        }
        std::vector<FT> r;
        if (ag) r.push_back(sumTo(da, ash));
        if (bg) r.push_back(sumTo(db, bsh));
        return r;
    };
    return makeAutogradFloat(vm, std::move(out), std::move(parents), std::move(bw));
}

// elementwise tensor ** tensor (differentiable)
inline Handle g_powT(KiritoVM& vm, Handle ah, Handle bh) {
    TensorVal& A = asT(vm, ah); TensorVal& B = asT(vm, bh);
    const FT& a = reqFloat(A, "pow"); const FT& b = reqFloat(B, "pow");
    FT out = tensor::elementwise(a, b, [](double x, double y) { checkPowDomain(x, y); return std::pow(x, y); });
    if (!wantsGrad(vm, {&A, &B})) return make(vm, std::move(out));
    bool ag = A.requiresGrad, bg = B.requiresGrad;
    tensor::Shape ash = a.shape, bsh = b.shape;
    FT acopy = a, bcopy = b;
    std::vector<Handle> parents; if (ag) parents.push_back(ah); if (bg) parents.push_back(bh);
    auto bw = [ag, bg, ash, bsh, acopy, bcopy](const FT& g) -> std::vector<FT> {
        FT ae = broadcastTo(acopy, g.shape), be = broadcastTo(bcopy, g.shape);
        std::vector<FT> r;
        if (ag) { FT da(g.shape); for (std::size_t i = 0; i < g.data.size(); ++i) da.data[i] = g.data[i] * be.data[i] * std::pow(ae.data[i], be.data[i] - 1.0); r.push_back(sumTo(da, ash)); }
        if (bg) { FT db(g.shape); for (std::size_t i = 0; i < g.data.size(); ++i) {
            // d/db (a**b) = a**b * ln(a); ln(a) is undefined for a <= 0, so the exponent gradient is
            // 0 there (avoids 0 * -inf = NaN at a zero base), matching the NumPy/PyTorch convention.
            db.data[i] = ae.data[i] > 0.0 ? g.data[i] * std::pow(ae.data[i], be.data[i]) * std::log(ae.data[i]) : 0.0;
        } r.push_back(sumTo(db, bsh)); }
        return r;
    };
    return makeAutogradFloat(vm, std::move(out), std::move(parents), std::move(bw));
}

// elementwise non-grad Float binary (mod / floordiv)
inline Handle ewFloat(KiritoVM& vm, const FT& a, const FT& b, char kind) {
    return make(vm, tensor::elementwise(a, b, [kind](double x, double y) {
        if (y == 0.0) throw KiritoError(kind == '%' ? "tensor modulo by zero" : "tensor floor-division by zero");
        return kind == '%' ? std::fmod(x, y) : std::floor(x / y);
    }));
}
inline Handle ewFloatScalar(KiritoVM& vm, const FT& a, double s, char kind) {
    if (s == 0.0) throw KiritoError(kind == '%' ? "tensor modulo by zero" : "tensor floor-division by zero");
    return make(vm, tensor::mapUnary(a, [kind, s](double x) {
        return kind == '%' ? std::fmod(x, s) : std::floor(x / s);
    }));
}

// --- structural ops (reshape-family reuse the differentiable reshape/permute) ---
inline Handle g_squeeze(KiritoVM& vm, Handle ah, int64_t axis) {
    tensor::Shape sh = asT(vm, ah).shape(), ns;
    if (axis < 0) { for (std::size_t d : sh) if (d != 1) ns.push_back(d); }
    else {
        std::size_t ax = static_cast<std::size_t>(axis);
        if (ax >= sh.size()) throw KiritoError("squeeze axis out of range");
        if (sh[ax] != 1) throw KiritoError("cannot squeeze an axis whose size is not 1");
        for (std::size_t i = 0; i < sh.size(); ++i) if (i != ax) ns.push_back(sh[i]);
    }
    return g_reshape(vm, ah, ns);
}
inline Handle g_expandDims(KiritoVM& vm, Handle ah, int64_t axis) {
    tensor::Shape sh = asT(vm, ah).shape();
    std::ptrdiff_t nd = static_cast<std::ptrdiff_t>(sh.size());
    if (axis < 0) axis += nd + 1;
    if (axis < 0 || axis > nd) throw KiritoError("expanddims axis out of range");
    tensor::Shape ns = sh; ns.insert(ns.begin() + axis, 1);
    return g_reshape(vm, ah, ns);
}
inline Handle g_swapaxes(KiritoVM& vm, Handle ah, int64_t a1, int64_t a2) {
    std::ptrdiff_t nd = static_cast<std::ptrdiff_t>(asT(vm, ah).ndim());
    auto nm = [&](int64_t a) { if (a < 0) a += nd; if (a < 0 || a >= nd) throw KiritoError("swapaxes axis out of range"); return static_cast<std::size_t>(a); };
    std::size_t x = nm(a1), y = nm(a2);
    tensor::Shape ax(static_cast<std::size_t>(nd));
    for (std::size_t i = 0; i < ax.size(); ++i) ax[i] = i;
    std::swap(ax[x], ax[y]);
    return g_permute(vm, ah, ax);
}
inline Handle g_flip(KiritoVM& vm, Handle ah, int64_t axis) {
    TensorVal& A = asT(vm, ah);
    std::size_t nd = A.ndim();
    std::vector<std::size_t> axes;
    if (axis >= 0) { if (static_cast<std::size_t>(axis) >= nd) throw KiritoError("flip axis out of range"); axes.push_back(static_cast<std::size_t>(axis)); }
    else for (std::size_t d = 0; d < nd; ++d) axes.push_back(d);
    if (A.isComplex()) { CT t = std::get<CT>(A.store); for (std::size_t ax : axes) t = tensor::flip(t, ax); return make(vm, std::move(t)); }
    const FT& a = std::get<FT>(A.store);
    FT out = a; for (std::size_t ax : axes) out = tensor::flip(out, ax);
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [axes](const FT& g) -> std::vector<FT> { FT d = g; for (std::size_t ax : axes) d = tensor::flip(d, ax); return {d}; };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}
inline Handle g_broadcastToShape(KiritoVM& vm, Handle ah, tensor::Shape target) {
    TensorVal& A = asT(vm, ah);
    if (A.isComplex()) {
        CT out = tensor::add(CT(target, cdouble(0, 0)), std::get<CT>(A.store));
        if (out.shape != target) throw KiritoError("broadcastto: cannot broadcast to the requested shape");
        return make(vm, std::move(out));
    }
    const FT& a = std::get<FT>(A.store);
    tensor::Shape ash = a.shape;
    FT out = broadcastTo(a, target);
    if (out.shape != target)  // a source larger than the target must NOT silently succeed with the bigger shape
        throw KiritoError("broadcastto: cannot broadcast to the requested shape");
    if (!wantsGrad(vm, {&A})) return make(vm, std::move(out));
    auto bw = [ash](const FT& g) -> std::vector<FT> { return {sumTo(g, ash)}; };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

// concatenate along an axis (differentiable: each grad-requiring part gets its slice back)
inline Handle g_concat(KiritoVM& vm, const std::vector<Handle>& hs, std::size_t axis) {
    if (hs.empty()) throw KiritoError("concatenate needs at least one tensor");
    bool anyComplex = false, anyGrad = false;
    for (Handle h : hs) { if (asT(vm, h).isComplex()) anyComplex = true; if (asT(vm, h).requiresGrad) anyGrad = true; }
    if (anyComplex) {
        std::vector<CT> parts; parts.reserve(hs.size());
        for (Handle h : hs) { TensorVal& t = asT(vm, h); parts.push_back(t.isComplex() ? std::get<CT>(t.store) : toComplex(std::get<FT>(t.store))); }
        std::vector<const CT*> ptrs; for (auto& p : parts) ptrs.push_back(&p);
        return make(vm, tensor::concatenate(ptrs, axis));
    }
    std::vector<const FT*> ptrs;
    for (Handle h : hs) ptrs.push_back(&std::get<FT>(asT(vm, h).store));
    FT out = tensor::concatenate(ptrs, axis);
    if (!gradEnabled(vm) || !anyGrad) return make(vm, std::move(out));
    std::vector<std::size_t> sizes; std::vector<bool> isg; std::vector<Handle> parents;
    for (Handle h : hs) { bool g = asT(vm, h).requiresGrad; sizes.push_back(asT(vm, h).shape()[axis]); isg.push_back(g); if (g) parents.push_back(h); }
    auto bw = [axis, sizes, isg](const FT& g) -> std::vector<FT> {
        std::vector<FT> r; std::ptrdiff_t base = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (isg[i]) r.push_back(tensor::sliceAxis(g, axis, base, base + static_cast<std::ptrdiff_t>(sizes[i]), 1));
            base += static_cast<std::ptrdiff_t>(sizes[i]);
        }
        return r;
    };
    return makeAutogradFloat(vm, std::move(out), std::move(parents), std::move(bw));
}
// stack along a new axis = expand-dims each, then concatenate (differentiable)
inline Handle g_stack(KiritoVM& vm, const std::vector<Handle>& hs, int64_t axis) {
    if (hs.empty()) throw KiritoError("stack needs at least one tensor");
    std::ptrdiff_t nd = static_cast<std::ptrdiff_t>(asT(vm, hs[0]).ndim());
    if (axis < 0) axis += nd + 1;
    if (axis < 0 || axis > nd) throw KiritoError("stack axis out of range");
    RootScope rs(vm);
    std::vector<Handle> expanded;
    for (Handle h : hs) expanded.push_back(rs.add(g_expandDims(vm, h, axis)));
    return g_concat(vm, expanded, static_cast<std::size_t>(axis));
}
// split into pieces of the given sizes along an axis -> a List of tensors (differentiable)
inline Handle g_split(KiritoVM& vm, Handle ah, const std::vector<std::size_t>& sizes, std::size_t axis) {
    RootScope rs(vm);
    auto list = std::make_unique<ListVal>();
    std::ptrdiff_t base = 0;
    for (std::size_t s : sizes) {
        list->elems.push_back(rs.add(g_sliceAxis(vm, ah, axis, {base, base + static_cast<std::ptrdiff_t>(s), 1})));
        base += static_cast<std::ptrdiff_t>(s);
    }
    return vm.alloc(std::move(list));
}

// repeat / tile (forward only) — templated over the element type
template <class T>
tensor::Tensor<T> repeatAlong(const tensor::Tensor<T>& t, std::size_t count, int64_t axis) {
    if (axis < 0) {
        tensor::Tensor<T> f = tensor::flatten(t);
        checkSize(tensor::Shape{f.size(), count});  // bound f.size()*count BEFORE it can wrap size_t
        tensor::Tensor<T> out(tensor::Shape{f.size() * count});
        for (std::size_t i = 0; i < f.size(); ++i) for (std::size_t k = 0; k < count; ++k) out.data[i * count + k] = f.data[i];
        return out;
    }
    std::size_t ax = static_cast<std::size_t>(axis);
    if (ax >= t.ndim()) throw tensor::TensorError("repeat axis out of range");
    tensor::Shape os = t.shape;
    checkSize(tensor::Shape{os[ax], count});        // bound os[ax]*count BEFORE the multiply wraps
    os[ax] *= count;
    tensor::Tensor<T> out(os);
    tensor::Shape ist = t.strides();
    for (std::size_t lin = 0; lin < out.size(); ++lin) {
        tensor::Shape c = tensor::unravel(lin, os);
        c[ax] /= count;
        std::size_t off = 0;
        for (std::size_t d = 0; d < t.ndim(); ++d) off += c[d] * ist[d];
        out.data[lin] = t.data[off];
    }
    return out;
}
template <class T>
tensor::Tensor<T> tileAlong(const tensor::Tensor<T>& t, tensor::Shape reps) {
    std::size_t nd = std::max(t.ndim(), reps.size());
    tensor::Shape sh = t.shape; while (sh.size() < nd) sh.insert(sh.begin(), 1);
    while (reps.size() < nd) reps.insert(reps.begin(), 1);
    tensor::Shape os(nd);
    for (std::size_t i = 0; i < nd; ++i) { checkSize(tensor::Shape{sh[i], reps[i]}); os[i] = sh[i] * reps[i]; }  // each product, then the total
    checkSize(os);
    tensor::Tensor<T> base = (t.shape == sh) ? t : tensor::reshape(t, sh);
    tensor::Tensor<T> out(os);
    tensor::Shape ist = base.strides();
    for (std::size_t lin = 0; lin < out.size(); ++lin) {
        tensor::Shape c = tensor::unravel(lin, os);
        std::size_t off = 0;
        for (std::size_t d = 0; d < nd; ++d) off += (c[d] % sh[d]) * ist[d];
        out.data[lin] = base.data[off];
    }
    return out;
}

// --- axis reductions (extra) ---
inline Handle reduceMinMax(KiritoVM& vm, Handle ah, int64_t axis, bool isMax) {
    warnDetach(vm, isMax ? "max()" : "min()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "min/max");
    if (axis < 0) return vm.makeFloat(isMax ? tensor::maxAll(a) : tensor::minAll(a));
    std::size_t ax = static_cast<std::size_t>(axis);
    // NaN-propagating combiners (numpy amax/amin): a NaN anywhere in the reduced axis -> NaN, so the
    // per-axis result matches maxAll/minAll and is independent of the NaN's position (A13-1).
    return make(vm, tensor::reduceAxis(a, ax, [isMax](double x, double y) {
        return isMax ? tensor::nanpropMax(x, y) : tensor::nanpropMin(x, y); }));
}
// iterate over each 1-D line along `axis`, calling fn(baseOffset, step, len).
template <class LineFn>
void forEachLine(const tensor::Shape& shape, std::size_t axis, LineFn fn) {
    tensor::Shape st = tensor::rowMajorStrides(shape);
    std::size_t len = shape[axis], step = st[axis];
    tensor::Shape outshape; for (std::size_t i = 0; i < shape.size(); ++i) if (i != axis) outshape.push_back(shape[i]);
    std::size_t nlines = tensor::numel(outshape);
    for (std::size_t l = 0; l < nlines; ++l) {
        tensor::Shape oc = tensor::unravel(l, outshape);
        std::size_t base = 0, oi = 0;
        for (std::size_t d = 0; d < shape.size(); ++d) { if (d == axis) continue; base += oc[oi] * st[d]; ++oi; }
        fn(base, step, len);
    }
}
inline Handle argMinMax(KiritoVM& vm, Handle ah, int64_t axis, bool isMax) {
    warnDetach(vm, isMax ? "argmax()" : "argmin()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "argmin/argmax");
    if (a.data.empty()) throw KiritoError("argmin/argmax of an empty tensor");
    // NaN handling matches numpy: a NaN is "larger" than everything, so argmax/argmin both return
    // the index of the FIRST NaN when one is present (order-independent in the same sense max/min
    // are now NaN-propagating; A13-1). A bare >/< comparison would silently SKIP a non-seed NaN.
    if (axis < 0) {
        std::size_t best = 0; double bv = a.data[0];
        for (std::size_t i = 0; i < a.data.size(); ++i) {
            if (std::isnan(a.data[i])) { best = i; break; }
            if (i > 0 && (isMax ? a.data[i] > bv : a.data[i] < bv)) { bv = a.data[i]; best = i; }
        }
        return vm.makeInt(static_cast<int64_t>(best));
    }
    std::size_t ax = static_cast<std::size_t>(axis);
    if (ax >= a.ndim()) throw KiritoError("argmin/argmax axis out of range");
    tensor::Shape os; for (std::size_t i = 0; i < a.ndim(); ++i) if (i != ax) os.push_back(a.shape[i]);
    FT out(os);
    std::size_t olin = 0;
    forEachLine(a.shape, ax, [&](std::size_t base, std::size_t step, std::size_t len) {
        std::size_t best = 0; double bv = a.data[base];
        for (std::size_t k = 0; k < len; ++k) {
            double v = a.data[base + k * step];
            if (std::isnan(v)) { best = k; break; }
            if (k > 0 && (isMax ? v > bv : v < bv)) { bv = v; best = k; }
        }
        out.data[olin++] = static_cast<double>(best);
    });
    return make(vm, std::move(out));
}
inline Handle stdVar(KiritoVM& vm, Handle ah, int64_t axis, bool wantStd, int64_t ddof) {
    warnDetach(vm, wantStd ? "std()" : "var()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "std/var");
    if (axis < 0) {
        double n = static_cast<double>(a.data.size());
        if (n - static_cast<double>(ddof) <= 0) throw KiritoError("std/var: not enough elements for the given ddof");
        double m = tensor::sumAll(a) / n, ss = 0;
        for (double x : a.data) ss += (x - m) * (x - m);
        double v = ss / (n - static_cast<double>(ddof));
        return vm.makeFloat(wantStd ? std::sqrt(v) : v);
    }
    std::size_t ax = static_cast<std::size_t>(axis);
    double cnt = static_cast<double>(a.shape[ax]);
    if (cnt - static_cast<double>(ddof) <= 0) throw KiritoError("std/var: not enough elements for the given ddof");
    tensor::Shape os; for (std::size_t i = 0; i < a.ndim(); ++i) if (i != ax) os.push_back(a.shape[i]);
    FT out(os);
    std::size_t olin = 0;
    forEachLine(a.shape, ax, [&](std::size_t base, std::size_t step, std::size_t len) {
        double m = 0; for (std::size_t k = 0; k < len; ++k) m += a.data[base + k * step]; m /= static_cast<double>(len);
        double ss = 0; for (std::size_t k = 0; k < len; ++k) { double x = a.data[base + k * step]; ss += (x - m) * (x - m); }
        double v = ss / (cnt - static_cast<double>(ddof));
        out.data[olin++] = wantStd ? std::sqrt(v) : v;
    });
    return make(vm, std::move(out));
}
inline Handle allAny(KiritoVM& vm, Handle ah, int64_t axis, bool isAll) {
    warnDetach(vm, isAll ? "all()" : "any()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "all/any");
    if (axis < 0) {
        bool r = isAll;
        for (double x : a.data) { bool nz = x != 0.0; if (isAll && !nz) { r = false; break; } if (!isAll && nz) { r = true; break; } }
        return vm.makeBool(r);
    }
    std::size_t ax = static_cast<std::size_t>(axis);
    // Pass the identity (all→True, any→False) so a per-axis reduction over an EMPTY axis yields it,
    // exactly as the whole-tensor path already does and as NumPy specifies — instead of throwing
    // "zero-size reduction". sum(0.0)/prod(1.0) already seed reduceAxis this way; all/any have just
    // as well-defined an identity, so the two paths agree.
    FT out = tensor::reduceAxis(a, ax, [isAll](double x, double y) { bool xb = x != 0, yb = y != 0; return static_cast<double>(isAll ? (xb && yb) : (xb || yb)); }, isAll ? 1.0 : 0.0);
    for (auto& v : out.data) v = v != 0.0 ? 1.0 : 0.0;  // normalize the single-length seed case
    return make(vm, std::move(out));
}
inline Handle ptpT(KiritoVM& vm, Handle ah, int64_t axis) {
    warnDetach(vm, "ptp()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "ptp");
    if (axis < 0) return vm.makeFloat(tensor::maxAll(a) - tensor::minAll(a));
    std::size_t ax = static_cast<std::size_t>(axis);
    FT mx = tensor::reduceAxis(a, ax, [](double x, double y) { return tensor::nanpropMax(x, y); });
    FT mn = tensor::reduceAxis(a, ax, [](double x, double y) { return tensor::nanpropMin(x, y); });
    return make(vm, tensor::sub(mx, mn));
}
inline Handle medianT(KiritoVM& vm, Handle ah, int64_t axis) {
    warnDetach(vm, "median()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "median");
    auto med = [](std::vector<double> v) { if (v.empty()) throw KiritoError("median of an empty axis"); std::sort(v.begin(), v.end(), [](double x, double y) { return x < y || (y != y && x == x); }); std::size_t n = v.size(); return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]); };  // NaN sorts last, like sort/argsort/unique (a deliberate prior decision, pinned in r7_regressions)
    if (axis < 0) { if (a.data.empty()) throw KiritoError("median of an empty tensor"); return vm.makeFloat(med(a.data)); }
    std::size_t ax = static_cast<std::size_t>(axis);
    tensor::Shape os; for (std::size_t i = 0; i < a.ndim(); ++i) if (i != ax) os.push_back(a.shape[i]);
    FT out(os);
    std::size_t olin = 0;
    forEachLine(a.shape, ax, [&](std::size_t base, std::size_t step, std::size_t len) {
        std::vector<double> line(len); for (std::size_t k = 0; k < len; ++k) line[k] = a.data[base + k * step];
        out.data[olin++] = med(std::move(line));
    });
    return make(vm, std::move(out));
}
// cumsum (differentiable) / cumprod (forward only); axis<0 flattens first.
inline Handle cumOp(KiritoVM& vm, Handle ah, int64_t axis, bool isSum) {
    TensorVal& A = asT(vm, ah);
    if (!isSum) warnDetach(vm, "cumprod()", A);
    bool flat = axis < 0;
    std::size_t ax = flat ? 0 : static_cast<std::size_t>(axis);
    if (A.isComplex()) {
        CT t = flat ? tensor::flatten(std::get<CT>(A.store)) : std::get<CT>(A.store);
        return make(vm, tensor::cumulative(t, ax, [isSum](cdouble x, cdouble y) { return isSum ? x + y : x * y; }));
    }
    const FT& src = std::get<FT>(A.store);
    FT t = flat ? tensor::flatten(src) : src;
    FT out = tensor::cumulative(t, ax, [isSum](double x, double y) { return isSum ? x + y : x * y; });
    if (!isSum || !wantsGrad(vm, {&A})) return make(vm, std::move(out));
    tensor::Shape ash = A.shape();
    auto bw = [ax, ash, flat](const FT& g) -> std::vector<FT> {
        FT rc = tensor::flip(tensor::cumulative(tensor::flip(g, ax), ax, [](double x, double y) { return x + y; }), ax);
        if (flat) rc = tensor::reshape(rc, ash);
        return {rc};
    };
    return makeAutogradFloat(vm, std::move(out), {ah}, std::move(bw));
}

// --- sorting / searching (forward only) ---
inline Handle sortT(KiritoVM& vm, Handle ah, int64_t axis) {
    warnDetach(vm, "sort()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "sort");
    if (a.ndim() == 0) return make(vm, a);
    std::size_t ax = axis < 0 ? a.ndim() - 1 : static_cast<std::size_t>(axis);
    FT out = a;
    forEachLine(a.shape, ax, [&](std::size_t base, std::size_t step, std::size_t len) {
        std::vector<double> line(len); for (std::size_t k = 0; k < len; ++k) line[k] = out.data[base + k * step];
        std::sort(line.begin(), line.end(), [](double x, double y) { return x < y || (y != y && x == x); });  // NaN sorts last
        for (std::size_t k = 0; k < len; ++k) out.data[base + k * step] = line[k];
    });
    return make(vm, std::move(out));
}
inline Handle argsortT(KiritoVM& vm, Handle ah, int64_t axis) {
    warnDetach(vm, "argsort()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "argsort");
    FT out(a.shape.empty() ? tensor::Shape{1} : a.shape);
    if (a.ndim() == 0) { out.data[0] = 0; return make(vm, std::move(out)); }
    std::size_t ax = axis < 0 ? a.ndim() - 1 : static_cast<std::size_t>(axis);
    forEachLine(a.shape, ax, [&](std::size_t base, std::size_t step, std::size_t len) {
        std::vector<std::size_t> idx(len); for (std::size_t k = 0; k < len; ++k) idx[k] = k;
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t i, std::size_t j) { double x = a.data[base + i * step], y = a.data[base + j * step]; return x < y || (y != y && x == x); });  // NaN sorts last
        for (std::size_t k = 0; k < len; ++k) out.data[base + k * step] = static_cast<double>(idx[k]);
    });
    return make(vm, std::move(out));
}
inline Handle uniqueT(KiritoVM& vm, Handle ah) {
    warnDetach(vm, "unique()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "unique");
    std::vector<double> v = a.data;
    std::sort(v.begin(), v.end(), [](double x, double y) { return x < y || (y != y && x == x); });  // NaN sorts last
    v.erase(std::unique(v.begin(), v.end()), v.end());
    // std::unique can't fold NaNs (NaN != NaN); they all sort to the end, so collapse that run to one.
    std::size_t k = 0;
    while (k < v.size() && v[v.size() - 1 - k] != v[v.size() - 1 - k]) ++k;
    if (k > 1) v.resize(v.size() - (k - 1));
    std::size_t n = v.size();
    return make(vm, FT(tensor::Shape{n}, std::move(v)));
}
inline Handle nonzeroT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    warnDetach(vm, "nonzero()", A);
    const tensor::Shape& sh = A.shape();
    std::size_t nd = sh.size();
    std::vector<std::vector<double>> coords(nd);
    for (std::size_t lin = 0; lin < A.size(); ++lin) {
        bool nz = A.isComplex() ? (A.elemAsComplex(lin) != cdouble(0, 0)) : (std::get<FT>(A.store).data[lin] != 0.0);
        if (nz) { tensor::Shape c = tensor::unravel(lin, sh); for (std::size_t d = 0; d < nd; ++d) coords[d].push_back(static_cast<double>(c[d])); }
    }
    // The List isn't in the arena until the alloc below, so nothing traces what it holds: root each
    // per-axis tensor as it is made, or allocating the next one collects the last. (v1.15 A19-1.)
    RootScope rs(vm);
    auto list = std::make_unique<ListVal>();
    for (std::size_t d = 0; d < nd; ++d) { std::size_t m = coords[d].size(); list->elems.push_back(rs.add(make(vm, FT(tensor::Shape{m}, std::move(coords[d]))))); }
    return vm.alloc(std::move(list));
}
inline Handle searchsortedT(KiritoVM& vm, Handle aH, Handle vH) {
    warnDetach(vm, "searchsorted()", asT(vm, aH));
    const FT& a = reqFloat(asT(vm, aH), "searchsorted");
    if (a.ndim() != 1) throw KiritoError("searchsorted: the first tensor must be 1-D and sorted");
    const Object& vo = vm.arena().deref(vH);
    if (const auto* tv = dynamic_cast<const TensorVal*>(&vo)) {
        const FT& v = reqFloat(*tv, "searchsorted");
        FT out(v.shape);
        for (std::size_t i = 0; i < v.data.size(); ++i) out.data[i] = static_cast<double>(std::lower_bound(a.data.begin(), a.data.end(), v.data[i]) - a.data.begin());
        return make(vm, std::move(out));
    }
    double s = Value(vm, vH).asFloat("searchsorted value");
    return vm.makeInt(static_cast<int64_t>(std::lower_bound(a.data.begin(), a.data.end(), s) - a.data.begin()));
}

// --- linear algebra (forward only; matmul/tensordot already cover the differentiable cases) ---
inline Handle detT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    warnDetach(vm, "det()", A);
    if (A.isComplex()) return cpx::make(vm, tensor::determinant(std::get<CT>(A.store)));
    return vm.makeFloat(tensor::determinant(std::get<FT>(A.store)));
}
inline Handle traceT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    warnDetach(vm, "trace()", A);
    if (A.isComplex()) return cpx::make(vm, tensor::trace(std::get<CT>(A.store)));
    return vm.makeFloat(tensor::trace(std::get<FT>(A.store)));
}
inline Handle invT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    warnDetach(vm, "inv()", A);
    if (A.isComplex()) return make(vm, tensor::inverse(std::get<CT>(A.store)));
    return make(vm, tensor::inverse(std::get<FT>(A.store)));
}
inline Handle solveT(KiritoVM& vm, Handle aH, Handle bH) {
    TensorVal& A = asT(vm, aH); TensorVal& B = asT(vm, bH);
    warnDetach(vm, "solve()", A); warnDetach(vm, "solve()", B);
    if (A.isComplex() || B.isComplex()) {
        CT a = A.isComplex() ? std::get<CT>(A.store) : toComplex(std::get<FT>(A.store));
        CT b = B.isComplex() ? std::get<CT>(B.store) : toComplex(std::get<FT>(B.store));
        return make(vm, tensor::solve(a, b));
    }
    return make(vm, tensor::solve(std::get<FT>(A.store), std::get<FT>(B.store)));
}
inline Handle normT(KiritoVM& vm, Handle ah, double ord) {
    warnDetach(vm, "norm()", asT(vm, ah));
    const FT& a = reqFloat(asT(vm, ah), "norm");
    if (ord == 0.0) { double c = 0; for (double x : a.data) c += (x != 0.0); return vm.makeFloat(c); }  // NumPy: count of nonzeros
    if (ord == 2.0) { double s = 0; for (double x : a.data) s += x * x; return vm.makeFloat(std::sqrt(s)); }
    if (std::isinf(ord)) {
        if (ord > 0) { double m = 0; for (double x : a.data) m = std::max(m, std::fabs(x)); return vm.makeFloat(m); }
        double m = HUGE_VAL; for (double x : a.data) m = std::min(m, std::fabs(x));  // ord == -inf: NumPy min|x|
        return vm.makeFloat(a.data.empty() ? 0.0 : m);
    }
    if (ord == 1.0) { double s = 0; for (double x : a.data) s += std::fabs(x); return vm.makeFloat(s); }
    double s = 0; for (double x : a.data) s += std::pow(std::fabs(x), ord);
    return vm.makeFloat(std::pow(s, 1.0 / ord));
}
inline Handle outerT(KiritoVM& vm, Handle aH, Handle bH) {
    warnDetach(vm, "outer()", asT(vm, aH)); warnDetach(vm, "outer()", asT(vm, bH));
    const FT& a = reqFloat(asT(vm, aH), "outer"); const FT& b = reqFloat(asT(vm, bH), "outer");
    FT af = tensor::flatten(a), bf = tensor::flatten(b);
    return make(vm, tensor::outer(af, bf));
}
inline Handle kronT(KiritoVM& vm, Handle aH, Handle bH) {
    warnDetach(vm, "kron()", asT(vm, aH)); warnDetach(vm, "kron()", asT(vm, bH));
    const FT& a = reqFloat(asT(vm, aH), "kron"); const FT& b = reqFloat(asT(vm, bH), "kron");
    return make(vm, tensor::kron(a, b));
}
inline Handle crossT(KiritoVM& vm, Handle aH, Handle bH) {
    warnDetach(vm, "cross()", asT(vm, aH)); warnDetach(vm, "cross()", asT(vm, bH));
    const FT& a = reqFloat(asT(vm, aH), "cross"); const FT& b = reqFloat(asT(vm, bH), "cross");
    FT af = tensor::flatten(a), bf = tensor::flatten(b);
    if (af.size() != 3 || bf.size() != 3) throw KiritoError("cross requires two 3-element vectors");
    FT out(tensor::Shape{3});
    out.data[0] = af.data[1] * bf.data[2] - af.data[2] * bf.data[1];
    out.data[1] = af.data[2] * bf.data[0] - af.data[0] * bf.data[2];
    out.data[2] = af.data[0] * bf.data[1] - af.data[1] * bf.data[0];
    return make(vm, std::move(out));
}
// A naive but general two/one-operand einsum (forward only). Handles transpose, diagonal, trace,
// sum, matmul, outer, batched contraction — any subscript string over the provided Float operands.
inline Handle einsumT(KiritoVM& vm, const std::string& spec, const std::vector<Handle>& ops) {
    std::string s; for (char ch : spec) if (!std::isspace(static_cast<unsigned char>(ch))) s += ch;
    std::string lhs = s, rhs; bool explicitOut = false;
    if (auto arrow = s.find("->"); arrow != std::string::npos) { explicitOut = true; lhs = s.substr(0, arrow); rhs = s.substr(arrow + 2); }
    std::vector<std::string> insub; { std::string cur; for (char ch : lhs) { if (ch == ',') { insub.push_back(cur); cur.clear(); } else cur += ch; } insub.push_back(cur); }
    if (insub.size() != ops.size()) throw KiritoError("einsum: the number of operands does not match the subscripts");
    std::vector<const FT*> arrs; std::vector<tensor::Shape> shps;
    fum::unordered_map<char, std::size_t> sz;
    for (std::size_t o = 0; o < ops.size(); ++o) {
        TensorVal& tv = reqT(vm, ops[o], "einsum");   // checked downcast (a non-tensor operand was UB)
        warnDetach(vm, "einsum()", tv);
        const FT& a = reqFloat(tv, "einsum");
        if (insub[o].size() != a.ndim()) throw KiritoError("einsum: a subscript length does not match its operand rank");
        arrs.push_back(&a); shps.push_back(a.shape);
        for (std::size_t i = 0; i < insub[o].size(); ++i) { char L = insub[o][i]; auto it = sz.find(L); if (it == sz.end()) sz[L] = a.shape[i]; else if (it->second != a.shape[i]) throw KiritoError("einsum: inconsistent dimension for an index"); }
    }
    std::string outl;
    if (explicitOut) outl = rhs;
    else { std::map<char, int> cnt; for (auto& su : insub) for (char ch : su) cnt[ch]++; for (auto& kv : cnt) if (kv.second == 1) outl += kv.first; }
    // Every label in the output spec must appear in at least one input, otherwise its dimension is
    // undefined and the silent zero-shape fall-through produces an empty tensor (NumPy throws here).
    if (explicitOut)
        for (char ch : outl)
            if (!sz.count(ch))
                throw KiritoError(std::string("einsum: output label '") + ch
                                  + "' does not appear in any input");
    // A repeated output label (e.g. "ii->ii", "i->ii") has an undefined contraction and previously
    // overran the output buffer (oshape counts every output char, but `coord` is keyed by the
    // deduplicated label set). Reject it up front, as NumPy does.
    if (explicitOut) {
        fum::unordered_set<char> outSeen;
        for (char ch : outl)
            if (!outSeen.insert(ch).second)
                throw KiritoError(std::string("einsum: output label '") + ch
                                  + "' appears more than once");
    }
    std::vector<char> labels; fum::unordered_set<char> seen;
    for (char ch : outl) if (seen.insert(ch).second) labels.push_back(ch);
    { std::set<char> all; for (auto& su : insub) for (char ch : su) all.insert(ch); fum::unordered_set<char> os(outl.begin(), outl.end()); for (char ch : all) if (!os.count(ch)) labels.push_back(ch); }
    fum::unordered_map<char, std::size_t> pos; for (std::size_t i = 0; i < labels.size(); ++i) pos[labels[i]] = i;
    std::vector<std::size_t> lsz; for (char ch : labels) lsz.push_back(sz[ch]);
    tensor::Shape oshape; for (char ch : outl) oshape.push_back(sz[ch]);
    FT out(oshape);
    std::size_t nOut = outl.size();
    tensor::Shape ost = oshape.empty() ? tensor::Shape{} : tensor::rowMajorStrides(oshape);
    std::vector<tensor::Shape> ists; for (auto& sh : shps) ists.push_back(tensor::rowMajorStrides(sh));
    // The contraction iterates `total` = the product of ALL label sizes (output + contracted dims).
    // Computing it unchecked overflowed size_t, wrapping to a small count and silently truncating the
    // result. Guard ONLY against that overflow — do NOT impose an element cap: the output tensor is
    // already element-bounded by its own shape, and the contraction WORK is left uncapped for parity
    // with the native matmul path (a normal `"ij,jk->ik"` on 1000x1000 is 1e9 MACs, exactly what
    // `.matmul()` runs unbounded). A pathologically large but non-overflowing einsum is merely slow,
    // like any unbounded computation — never a wrong answer or an OOB.
    std::size_t total = 1;
    for (std::size_t d : lsz) {
        if (d != 0 && total > SIZE_MAX / d) throw KiritoError("einsum: contraction size overflows");
        total *= d;
    }
    std::vector<std::size_t> coord(labels.size(), 0);
    for (std::size_t t = 0; t < total; ++t) {
        double prod = 1.0;
        for (std::size_t o = 0; o < ops.size(); ++o) {
            std::size_t off = 0;
            for (std::size_t i = 0; i < insub[o].size(); ++i) off += coord[pos[insub[o][i]]] * ists[o][i];
            prod *= arrs[o]->data[off];
        }
        std::size_t ooff = 0; for (std::size_t i = 0; i < nOut; ++i) ooff += coord[i] * ost[i];
        out.data[ooff] += prod;
        for (std::size_t i = labels.size(); i-- > 0;) { if (++coord[i] < lsz[i]) break; coord[i] = 0; }
    }
    return make(vm, std::move(out));
}

// --- complex helpers ---
inline Handle realT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    if (!A.isComplex()) return make(vm, std::get<FT>(A.store));
    const CT& c = std::get<CT>(A.store); FT out(c.shape);
    for (std::size_t i = 0; i < c.data.size(); ++i) out.data[i] = c.data[i].real();
    return make(vm, std::move(out));
}
inline Handle imagT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    if (!A.isComplex()) return make(vm, FT(A.shape(), 0.0));
    const CT& c = std::get<CT>(A.store); FT out(c.shape);
    for (std::size_t i = 0; i < c.data.size(); ++i) out.data[i] = c.data[i].imag();
    return make(vm, std::move(out));
}
inline Handle conjT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    if (!A.isComplex()) return make(vm, std::get<FT>(A.store));
    return make(vm, tensor::mapUnary(std::get<CT>(A.store), [](cdouble z) { return std::conj(z); }));
}
inline Handle angleT(KiritoVM& vm, Handle ah) {
    TensorVal& A = asT(vm, ah);
    constexpr double kPi = 3.14159265358979323846;
    if (!A.isComplex()) { const FT& a = std::get<FT>(A.store); return make(vm, tensor::mapUnary(a, [](double x) { return x < 0 ? kPi : 0.0; })); }
    const CT& c = std::get<CT>(A.store); FT out(c.shape);
    for (std::size_t i = 0; i < c.data.size(); ++i) out.data[i] = std::arg(c.data[i]);
    return make(vm, std::move(out));
}

}  // namespace tns

inline Handle TensorVal::binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) {
    const Object& b = vm.arena().deref(rhs);
    const auto* ot = dynamic_cast<const TensorVal*>(&b);
    bool scalarComplex = dynamic_cast<const ComplexVal*>(&b) != nullptr;
    return tns::wrap([&]() -> Handle {
        // --- elementwise comparisons (Float-only) -> a 0/1 mask ---
        if (op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge) {
            if (isComplex() || (ot && ot->isComplex()) || scalarComplex)
                throw KiritoError("ordering comparisons are not defined for Complex tensors (complex is unordered)");
            const FT& x = std::get<FT>(store);
            if (ot) return tns::compareTensors(vm, op, x, std::get<FT>(ot->store));
            return tns::compareScalar(vm, op, x, Value(vm, rhs).asFloat("scalar"));
        }
        // --- power (differentiable, Float) ---
        if (op == BinOp::Pow) {
            if (isComplex() || (ot && ot->isComplex()) || scalarComplex)
                throw KiritoError("** is Float-only on tensors (use the complex module for complex powers)");
            if (ot) return tns::g_powT(vm, self, rhs);
            return tns::g_pow(vm, self, Value(vm, rhs).asFloat("exponent"));
        }
        // --- modulo / floor-division (Float-only, no grad) ---
        if (op == BinOp::Mod || op == BinOp::FloorDiv) {
            if (isComplex() || (ot && ot->isComplex()) || scalarComplex)
                throw KiritoError("% and // are Float-only on tensors");
            char kind = op == BinOp::Mod ? '%' : '/';
            tns::warnDetach(vm, op == BinOp::Mod ? "%" : "//", *this);
            const FT& x = std::get<FT>(store);
            if (ot) return tns::ewFloat(vm, x, std::get<FT>(ot->store), kind);
            return tns::ewFloatScalar(vm, x, Value(vm, rhs).asFloat("scalar"), kind);
        }
        // --- arithmetic + - * / ---
        char c = op == BinOp::Add ? '+' : op == BinOp::Sub ? '-' : op == BinOp::Mul ? '*' : op == BinOp::Div ? '/' : 0;
        if (!c) throw KiritoError("Tensor does not support this operator (use .matmul for matrix products)");
        if (ot) {  // tensor OP tensor
            if (!isComplex() && !ot->isComplex()) return tns::g_binop(vm, c, self, rhs);
            CT a = isComplex() ? std::get<CT>(store) : tns::toComplex(std::get<FT>(store));
            CT d = ot->isComplex() ? std::get<CT>(ot->store) : tns::toComplex(std::get<FT>(ot->store));
            switch (c) {
                case '+': { return tns::make(vm, tensor::add(a, d)); } break;
                case '-': { return tns::make(vm, tensor::sub(a, d)); } break;
                case '*': { return tns::make(vm, tensor::mul(a, d)); } break;
                default: { return tns::make(vm, tensor::div(a, d)); } break;
            }
        }
        // tensor OP scalar
        if (!isComplex() && !scalarComplex) return tns::g_scalar(vm, c, self, Value(vm, rhs).asFloat("scalar"));
        CT a = isComplex() ? std::get<CT>(store) : tns::toComplex(std::get<FT>(store));
        return tns::make(vm, tensor::scalarOp(a, cpx::asComplex(vm, rhs, "scalar"), c));
    });
}

// Single-axis slice `t[start:stop:step]` (NumPy slices the first axis). The slice protocol gives no
// self-handle, so this returns a detached copy; use the `slice(start, stop, step[, axis])` method for
// the autograd-aware form.
inline Handle TensorVal::slice(KiritoVM& vm, Handle start, Handle stop, Handle step) {
    if (ndim() == 0) throw KiritoError("cannot slice a 0-D tensor");
    tns::warnDetach(vm, "[] slicing (use .slice() to keep gradients)", *this);
    return tns::wrap([&]() -> Handle {
        tns::SliceRange r = tns::resolveSlice(vm, start, stop, step, shape()[0]);
        if (isComplex()) return tns::make(vm, tensor::sliceAxis(std::get<CT>(store), 0, r.start, r.stop, r.step));
        return tns::make(vm, tensor::sliceAxis(std::get<FT>(store), 0, r.start, r.stop, r.step));
    });
}

inline Handle TensorVal::unary(KiritoVM& vm, UnOp op, Handle self) {
    if (op != UnOp::Neg) throw KiritoError("Tensor does not support this unary operator");
    if (isComplex()) return tns::make(vm, tensor::negate(std::get<CT>(store)));
    return tns::g_neg(vm, self);
}

inline Handle TensorVal::getItem(KiritoVM& vm, std::span<const Handle> keys) {
    // A single non-Integer key is either a boolean mask (a same-shape Tensor of 0/1) or a fancy index
    // (a List of integers selecting rows along axis 0). Both return a detached copy (the index protocol
    // gives no self-handle for autograd; use the `take(indices, axis)` method for the grad-aware form).
    if (keys.size() == 1) {
        const Object& k0 = vm.arena().deref(keys[0]);
        if (const auto* mask = dynamic_cast<const TensorVal*>(&k0)) {  // boolean mask -> 1-D selection
            if (mask->shape() != shape()) throw KiritoError("boolean mask must match the tensor shape");
            tns::warnDetach(vm, "[] boolean indexing", *this);
            if (isComplex()) {
                const auto& d = std::get<CT>(store).data; std::vector<cdouble> out;
                for (std::size_t i = 0; i < d.size(); ++i) if (mask->elemAsComplex(i) != cdouble(0, 0)) out.push_back(d[i]);
                std::size_t n = out.size();
                return tns::make(vm, TensorVal::CT(tensor::Shape{n}, std::move(out)));
            }
            const auto& d = std::get<FT>(store).data; std::vector<double> out;
            for (std::size_t i = 0; i < d.size(); ++i) if (mask->elemAsComplex(i) != cdouble(0, 0)) out.push_back(d[i]);
            std::size_t n = out.size();
            return tns::make(vm, TensorVal::FT(tensor::Shape{n}, std::move(out)));
        }
        if (k0.kind() == ValueKind::List || k0.kind() == ValueKind::Array) {  // fancy index along axis 0
            tns::warnDetach(vm, "[] fancy indexing (use .take() to keep gradients)", *this);
            std::vector<std::ptrdiff_t> idxs;
            for (Value e : Value(vm, keys[0]).items()) idxs.push_back(static_cast<std::ptrdiff_t>(e.asInt("index")));
            return tns::wrap([&]() -> Handle {
                if (isComplex()) return tns::make(vm, tensor::takeAxis0(std::get<CT>(store), idxs));
                return tns::make(vm, tensor::takeAxis0(std::get<FT>(store), idxs));
            });
        }
    }
    if (keys.size() > ndim()) throw KiritoError("too many indices for tensor");
    const tensor::Shape& sh = shape();
    tensor::Shape idx;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const Object& o = vm.arena().deref(keys[i]);
        if (o.kind() != ValueKind::Integer) throw KiritoError("Tensor index must be Integer");
        int64_t v = static_cast<const IntVal&>(o).value();
        if (v < 0) v += static_cast<int64_t>(sh[i]);   // negative indexing (consistent with slicing)
        if (v < 0 || v >= static_cast<int64_t>(sh[i])) throw KiritoError("Tensor index out of range");
        idx.push_back(static_cast<std::size_t>(v));
    }
    tensor::Shape st = tensor::rowMajorStrides(sh);
    std::size_t off = 0;
    for (std::size_t i = 0; i < idx.size(); ++i) off += idx[i] * st[i];
    if (idx.size() == ndim()) {  // a single element -> a scalar value
        if (isComplex()) return cpx::make(vm, std::get<CT>(store).data[off]);
        return vm.makeFloat(std::get<FT>(store).data[off]);
    }
    // a partial index -> a sub-tensor (the contiguous block of the remaining axes)
    tensor::Shape sub(sh.begin() + static_cast<std::ptrdiff_t>(idx.size()), sh.end());
    std::size_t subN = tensor::numel(sub);
    if (isComplex()) {
        const auto& d = std::get<CT>(store).data;
        return tns::make(vm, TensorVal::CT(sub, std::vector<cdouble>(d.begin() + static_cast<std::ptrdiff_t>(off), d.begin() + static_cast<std::ptrdiff_t>(off + subN))));
    }
    const auto& d = std::get<FT>(store).data;
    return tns::make(vm, TensorVal::FT(sub, std::vector<double>(d.begin() + static_cast<std::ptrdiff_t>(off), d.begin() + static_cast<std::ptrdiff_t>(off + subN))));
}

inline void TensorVal::setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) {
    // Refuse in-place mutation of a grad-tracking tensor. The autograd graph caches forward values,
    // so writing an element (the sole in-place op) without touching the graph makes a later backward()
    // silently return gradients computed against the STALE pre-mutation values. PyTorch hard-errors on
    // the same; Kirito's documented style is to rebind functionally (w = w - w.grad*lr), not mutate.
    if (requiresGrad || node)
        throw KiritoError("Tensor element assignment is not allowed on a grad-tracking tensor "
                          "(it would desync the autograd graph); detach() first, or rebind functionally");
    if (keys.size() != ndim()) throw KiritoError("Tensor element assignment needs a full index (one per dimension)");
    const tensor::Shape& sh = shape();
    tensor::Shape idx;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const Object& o = vm.arena().deref(keys[i]);
        if (o.kind() != ValueKind::Integer) throw KiritoError("Tensor index must be Integer");
        int64_t v = static_cast<const IntVal&>(o).value();
        if (v < 0) v += static_cast<int64_t>(sh[i]);   // negative indexing
        if (v < 0 || v >= static_cast<int64_t>(sh[i])) throw KiritoError("Tensor index out of range");
        idx.push_back(static_cast<std::size_t>(v));
    }
    if (isComplex()) std::get<CT>(store).at(idx) = cpx::asComplex(vm, value, "Tensor element");
    else std::get<FT>(store).at(idx) = Value(vm, value).asFloat("Tensor element");
}

inline Handle TensorVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto self_t = [](KiritoVM& vm, Handle self) -> TensorVal& { return static_cast<TensorVal&>(vm.arena().deref(self)); };
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    auto axisOf = [self, self_t](KiritoVM& vm, std::span<const Handle> a) -> int64_t {  // -1 = whole tensor (no axis given)
        if (a.empty() || vm.arena().deref(a[0]).kind() == ValueKind::None) return -1;
        int64_t ax = Value(vm, a[0]).asInt("axis");
        int64_t nd = static_cast<int64_t>(self_t(vm, self).shape().size());
        if (ax < 0) ax += nd;                       // NumPy-style: -1 is the last axis, -2 the next, ...
        if (ax < 0 || ax >= nd) throw KiritoError("axis out of range");
        return ax;                                  // always a valid 0..nd-1 once an axis is given
    };
    // compare(other, rel_tol=1e-9, abs_tol=0.0) -> Bool — tolerant whole-tensor comparison (same
    // shape + every element within tolerance), since `==` is now exact. Signatured for
    // keyword args/defaults + inspect.
    if (name == "compare") {
        RootScope rs(vm);
        return vm.alloc(std::make_unique<NativeFunction>(
            "compare", toleranceSig(vm, rs), "Bool",
            [self](KiritoVM& v, std::span<const Handle> a) -> Handle {
                auto& t = static_cast<TensorVal&>(v.arena().deref(self));
                const auto* o = dynamic_cast<const TensorVal*>(&v.arena().deref(a[0]));
                if (!o) throw KiritoError("compare expects a Tensor");
                if (o->shape() != t.shape()) return v.makeBool(false);
                double rel = Value(v, a[1]).asFloat("rel_tol"), abst = Value(v, a[2]).asFloat("abs_tol");
                for (std::size_t i = 0; i < t.size(); ++i)
                    if (!cClose(t.elemAsComplex(i), o->elemAsComplex(i), rel, abst)) return v.makeBool(false);
                return v.makeBool(true);
            },
            std::vector<Handle>{self}));
    }
    // item() — extract a one-element tensor's single value as a plain Float (or Complex).
    if (name == "item") return bind("item", {}, [self, self_t](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& t = self_t(vm, self);
        if (t.size() != 1) throw KiritoError("item() requires a tensor with exactly one element, got " +
                                             std::to_string(t.size()));
        if (t.isComplex()) return cpx::make(vm, std::get<CT>(t.store).data[0]);
        return vm.makeFloat(std::get<FT>(t.store).data[0]);
    });
    // tolist() — convert to a nested Kirito List (Float/Complex leaves), mirroring the shape.
    if (name == "tolist") return bind("tolist", {}, [self, self_t](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& t = self_t(vm, self);
        RootScope rs(vm);
        const tensor::Shape& shp = t.shape();
        std::function<Handle(std::size_t, std::size_t)> build = [&](std::size_t dim, std::size_t off) -> Handle {
            if (dim == shp.size()) {  // a scalar leaf
                if (t.isComplex()) return rs.add(cpx::make(vm, std::get<CT>(t.store).data[off]));
                // Root the leaf: it sits in a parent ListVal that isn't in the arena yet, so a GC
                // triggered by a sibling's allocation would otherwise reclaim it (stale handle).
                return rs.add(vm.makeFloat(std::get<FT>(t.store).data[off]));
            }
            tensor::Shape st = t.isComplex() ? std::get<CT>(t.store).strides() : std::get<FT>(t.store).strides();
            auto list = std::make_unique<ListVal>();
            for (std::size_t i = 0; i < shp[dim]; ++i)
                list->elems.push_back(build(dim + 1, off + i * st[dim]));
            return rs.add(vm.alloc(std::move(list)));
        };
        return build(0, 0);
    });
    if (name == "shape") return bind("shape", {}, [self, self_t](KiritoVM& vm, std::span<const Handle>) -> Handle {
        // A dimension past the small-int intern range is a fresh allocation, and the List is not
        // arena-reachable yet — so root each one. (Only ever bit shapes with a dim > 255.)
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        for (std::size_t d : self_t(vm, self).shape()) list->elems.push_back(rs.add(vm.makeInt(static_cast<int64_t>(d))));
        return vm.alloc(std::move(list));
    });
    if (name == "ndim") return bind("ndim", {}, [self, self_t](KiritoVM& vm, std::span<const Handle>) { return vm.makeInt(static_cast<int64_t>(self_t(vm, self).ndim())); });
    if (name == "size") return bind("size", {}, [self, self_t](KiritoVM& vm, std::span<const Handle>) { return vm.makeInt(static_cast<int64_t>(self_t(vm, self).size())); });
    if (name == "dtype") return bind("dtype", {}, [self, self_t](KiritoVM& vm, std::span<const Handle>) { return vm.makeString(self_t(vm, self).dtypeName()); });
    if (name == "reshape") return bind("reshape", {"shape"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "reshape").require(1);
        tensor::Shape ns = tns::readShape(Value(vm, a[0]));
        return tns::wrap([&]() { return tns::g_reshape(vm, self, ns); });
    });
    if (name == "transpose") return bind("transpose", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
        return tns::wrap([&]() { return tns::g_transpose(vm, self); });
    });
    if (name == "permute") return bind("permute", {"axes"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "permute").require(1);
        tensor::Shape ax = tns::readShape(Value(vm, a[0]));
        return tns::wrap([&]() { return tns::g_permute(vm, self, ax); });
    });
    if (name == "flatten") return bind("flatten", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
        return tns::wrap([&]() { return tns::g_flatten(vm, self); });
    });
    if (name == "apply") return bind("apply", {"fn"}, [self, self_t](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "apply").require(1);
        Handle fn = a[0];
        auto& t = self_t(vm, self);
        tns::warnDetach(vm, "apply()", t);
        if (t.isComplex()) {
            CT out = std::get<CT>(t.store);
            for (std::size_t i = 0; i < out.data.size(); ++i) {
                RootScope rs(vm);
                std::array<Handle, 1> args{rs.add(cpx::make(vm, out.data[i]))};
                out.data[i] = cpx::asComplex(vm, vm.arena().deref(fn).call(vm, args), "apply result");
            }
            return tns::make(vm, std::move(out));
        }
        FT out = std::get<FT>(t.store);
        for (std::size_t i = 0; i < out.data.size(); ++i) {
            // Root the argument: `fn` is user code and will allocate, and an unrooted Float would be
            // collected out from under the callee. (The Complex branch above always did; this one
            // didn't. v1.15 A19-1.)
            RootScope rs(vm);
            std::array<Handle, 1> args{rs.add(vm.makeFloat(out.data[i]))};
            out.data[i] = Value(vm, vm.arena().deref(fn).call(vm, args)).asFloat("apply result");
        }
        return tns::make(vm, std::move(out));
    });
    if (name == "astype") return bind("astype", {"dtype"}, [self, self_t](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "astype").require(1);
        auto& t = self_t(vm, self);
        tns::warnDetach(vm, "astype()", t);
        bool toComplex = tns::wantsComplex(vm, a[0]);
        if (toComplex) {
            if (t.isComplex()) return tns::make(vm, std::get<CT>(t.store));
            return tns::make(vm, tns::toComplex(std::get<FT>(t.store)));
        }
        if (!t.isComplex()) return tns::make(vm, std::get<FT>(t.store));
        const CT& c = std::get<CT>(t.store);  // Complex -> Float keeps the real part
        FT out(c.shape);
        for (std::size_t i = 0; i < c.data.size(); ++i) out.data[i] = c.data[i].real();
        return tns::make(vm, std::move(out));
    });
    if (name == "matmul") return bind("matmul", {"other"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "matmul").require(1);
        const auto* o = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
        if (!o) throw KiritoError("matmul expects a Tensor");
        return tns::wrap([&]() { return tns::g_matmul(vm, self, a[0]); });
    });
    if (name == "dot") return bind("dot", {"other"}, [self, self_t](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "dot").require(1);
        auto& t = self_t(vm, self);
        const auto* o = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
        if (!o) throw KiritoError("dot expects a Tensor");
        tns::warnDetach(vm, "dot()", t);
        tns::warnDetach(vm, "dot()", *o);
        return tns::wrap([&]() -> Handle {
            if (!t.isComplex() && !o->isComplex()) return vm.makeFloat(tensor::dot(std::get<FT>(t.store), std::get<FT>(o->store)));
            CT x = t.isComplex() ? std::get<CT>(t.store) : tns::toComplex(std::get<FT>(t.store));
            CT y = o->isComplex() ? std::get<CT>(o->store) : tns::toComplex(std::get<FT>(o->store));
            return cpx::make(vm, tensor::dot(x, y));
        });
    });
    if (name == "sum") return bind("sum", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::g_sum(vm, self, axis); });
    });
    if (name == "mean") return bind("mean", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::g_mean(vm, self, axis); });
    });
    if (name == "prod") return bind("prod", {"axis"}, [self, self_t, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        auto& t = self_t(vm, self);
        tns::warnDetach(vm, "prod()", t);
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() -> Handle {
            if (axis < 0) {
                if (t.isComplex()) return cpx::make(vm, tensor::prodAll(std::get<CT>(t.store)));
                return vm.makeFloat(tensor::prodAll(std::get<FT>(t.store)));
            }
            std::size_t ax = static_cast<std::size_t>(axis);
            if (t.isComplex()) return tns::make(vm, tensor::reduceAxis(std::get<CT>(t.store), ax, [](cdouble x, cdouble y) { return x * y; }, cdouble(1.0, 0.0)));
            return tns::make(vm, tensor::reduceAxis(std::get<FT>(t.store), ax, [](double x, double y) { return x * y; }, 1.0));
        });
    });
    if (name == "min" || name == "max") {
        bool wantMax = name == "max";
        return bind(wantMax ? "max" : "min", {"axis"}, [self, axisOf, wantMax](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t axis = axisOf(vm, a);
            return tns::wrap([&]() { return tns::reduceMinMax(vm, self, axis, wantMax); });
        });
    }
    // axis-wise reductions / argument reductions / statistics
    if (name == "argmin" || name == "argmax") {
        bool wantMax = name == "argmax";
        return bind(wantMax ? "argmax" : "argmin", {"axis"}, [self, axisOf, wantMax](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t axis = axisOf(vm, a);
            return tns::wrap([&]() { return tns::argMinMax(vm, self, axis, wantMax); });
        });
    }
    if (name == "std" || name == "var") {
        bool wantStd = name == "std";
        return bind(name == "std" ? "std" : "var", {"axis", "ddof"}, [self, axisOf, wantStd](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t axis = axisOf(vm, a);
            int64_t ddof = (a.size() > 1 && vm.arena().deref(a[1]).kind() != ValueKind::None) ? Value(vm, a[1]).asInt("ddof") : 0;
            return tns::wrap([&]() { return tns::stdVar(vm, self, axis, wantStd, ddof); });
        });
    }
    if (name == "all" || name == "any") {
        bool isAll = name == "all";
        return bind(isAll ? "all" : "any", {"axis"}, [self, axisOf, isAll](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t axis = axisOf(vm, a);
            return tns::wrap([&]() { return tns::allAny(vm, self, axis, isAll); });
        });
    }
    if (name == "ptp") return bind("ptp", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::ptpT(vm, self, axis); });
    });
    if (name == "median") return bind("median", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::medianT(vm, self, axis); });
    });
    if (name == "cumsum" || name == "cumprod") {
        bool isSum = name == "cumsum";
        return bind(isSum ? "cumsum" : "cumprod", {"axis"}, [self, axisOf, isSum](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t axis = axisOf(vm, a);
            return tns::wrap([&]() { return tns::cumOp(vm, self, axis, isSum); });
        });
    }
    // elementwise comparisons (return a 0/1 Float mask) and logic
    {
        static const fum::unordered_map<std::string, BinOp> kCmp = {
            {"eq", BinOp::Eq}, {"ne", BinOp::Ne}, {"lt", BinOp::Lt}, {"le", BinOp::Le}, {"gt", BinOp::Gt}, {"ge", BinOp::Ge}};
        if (auto it = kCmp.find(std::string(name)); it != kCmp.end()) {
            BinOp cop = it->second;
            return bind(std::string(name).c_str(), {"other"}, [self, cop](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "comparison").require(1);
                const FT& x = tns::reqFloat(tns::asT(vm, self), "comparison");
                return tns::wrap([&]() -> Handle {
                    const auto* o = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
                    if (o) return tns::compareTensors(vm, cop, x, tns::reqFloat(*o, "comparison"));
                    return tns::compareScalar(vm, cop, x, Value(vm, a[0]).asFloat("scalar"));
                });
            });
        }
    }
    if (name == "logicaland" || name == "logicalor" || name == "logicalxor") {
        std::string nm(name);
        return bind(nm.c_str(), {"other"}, [self, nm](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args(vm, a, nm.c_str()).require(1);
            const FT& x = tns::reqFloat(tns::asT(vm, self), "logical op");
            const auto* o = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
            if (!o) throw KiritoError("logical op expects a Tensor");
            const FT& y = tns::reqFloat(*o, "logical op");
            return tns::wrap([&]() -> Handle {
                return tns::make(vm, tensor::elementwise(x, y, [nm](double p, double q) {
                    bool pb = p != 0, qb = q != 0;
                    bool r = nm == "logicaland" ? (pb && qb) : nm == "logicalor" ? (pb || qb) : (pb != qb);
                    return static_cast<double>(r);
                }));
            });
        });
    }
    if (name == "logicalnot") return bind("logicalnot", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
        const FT& x = tns::reqFloat(tns::asT(vm, self), "logicalnot");
        return tns::make(vm, tensor::mapUnary(x, [](double p) { return static_cast<double>(p == 0.0); }));
    });
    // elementwise maximum/minimum (differentiable) and clip
    if (name == "maximum" || name == "minimum") {
        bool isMax = name == "maximum";
        return bind(isMax ? "maximum" : "minimum", {"other"}, [self, isMax](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args(vm, a, isMax ? "maximum" : "minimum").require(1);
            const auto* o = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
            if (!o) throw KiritoError("maximum/minimum expects a Tensor");
            return tns::wrap([&]() { return tns::g_maxmin(vm, self, a[0], isMax); });
        });
    }
    if (name == "clip") return bind("clip", {"lo", "hi"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "clip").require(2);
        double lo = Value(vm, a[0]).asFloat("lo"), hi = Value(vm, a[1]).asFloat("hi");
        return tns::wrap([&]() { return tns::g_clip(vm, self, lo, hi); });
    });
    // structural ops
    if (name == "squeeze") return bind("squeeze", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::g_squeeze(vm, self, axis); });
    });
    if (name == "expanddims") return bind("expanddims", {"axis"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "expanddims").require(1);
        int64_t axis = Value(vm, a[0]).asInt("axis");
        return tns::wrap([&]() { return tns::g_expandDims(vm, self, axis); });
    });
    if (name == "swapaxes") return bind("swapaxes", {"axis1", "axis2"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "swapaxes").require(2);
        int64_t a1 = Value(vm, a[0]).asInt("axis1"), a2 = Value(vm, a[1]).asInt("axis2");
        return tns::wrap([&]() { return tns::g_swapaxes(vm, self, a1, a2); });
    });
    if (name == "flip") return bind("flip", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::g_flip(vm, self, axis); });
    });
    if (name == "broadcastto") return bind("broadcastto", {"shape"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "broadcastto").require(1);
        tensor::Shape ns = tns::readShape(Value(vm, a[0]));
        return tns::wrap([&]() { return tns::g_broadcastToShape(vm, self, ns); });
    });
    if (name == "repeat") return bind("repeat", {"count", "axis"}, [self, self_t, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "repeat").require(1);
        int64_t count = Value(vm, a[0]).asInt("count");
        if (count < 0) throw KiritoError("repeat count must be non-negative");
        tns::warnDetach(vm, "repeat()", self_t(vm, self));
        int64_t axis = (a.size() > 1) ? axisOf(vm, std::span<const Handle>(a.data() + 1, a.size() - 1)) : -1;
        auto& t = self_t(vm, self);
        return tns::wrap([&]() -> Handle {
            if (t.isComplex()) return tns::make(vm, tns::repeatAlong(std::get<CT>(t.store), static_cast<std::size_t>(count), axis));
            return tns::make(vm, tns::repeatAlong(std::get<FT>(t.store), static_cast<std::size_t>(count), axis));
        });
    });
    if (name == "tile") return bind("tile", {"reps"}, [self, self_t](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "tile").require(1);
        tensor::Shape reps = tns::readShape(Value(vm, a[0]));
        auto& t = self_t(vm, self);
        tns::warnDetach(vm, "tile()", t);
        return tns::wrap([&]() -> Handle {
            if (t.isComplex()) return tns::make(vm, tns::tileAlong(std::get<CT>(t.store), reps));
            return tns::make(vm, tns::tileAlong(std::get<FT>(t.store), reps));
        });
    });
    // autograd-aware slice / gather (these carry the self-handle, unlike the [] protocol)
    if (name == "slice") return bind("slice", {"start", "stop", "step", "axis"}, [self, self_t](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        auto& t = self_t(vm, self);
        int64_t ax = (a.size() > 3 && vm.arena().deref(a[3]).kind() != ValueKind::None) ? Value(vm, a[3]).asInt("axis") : 0;
        if (ax < 0) ax += static_cast<int64_t>(t.ndim());   // NumPy-style negative axis (consistent with take/reductions)
        if (ax < 0 || ax >= static_cast<int64_t>(t.ndim())) throw KiritoError("slice axis out of range");
        std::size_t axis = static_cast<std::size_t>(ax);
        Handle sH = a.size() > 0 ? a[0] : vm.none();
        Handle eH = a.size() > 1 ? a[1] : vm.none();
        Handle stH = a.size() > 2 ? a[2] : vm.none();
        return tns::wrap([&]() { return tns::g_sliceAxis(vm, self, axis, tns::resolveSlice(vm, sH, eH, stH, t.shape()[axis])); });
    });
    if (name == "take") return bind("take", {"indices", "axis"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "take").require(1);   // makeMethod's positional fast path forwards args verbatim; guard before a[0]
        std::vector<std::ptrdiff_t> idxs;
        for (Value e : Value(vm, a[0]).items()) idxs.push_back(static_cast<std::ptrdiff_t>(e.asInt("index")));
        int64_t axis = 0;
        if (a.size() > 1 && vm.arena().deref(a[1]).kind() != ValueKind::None)
            axis = Value(vm, a[1]).asInt("axis");
        auto& t = static_cast<TensorVal&>(vm.arena().deref(self));
        int64_t nd = static_cast<int64_t>(t.shape().size());
        if (nd == 0) throw KiritoError("take: tensor has no axes");
        if (axis < 0) axis += nd;
        if (axis < 0 || axis >= nd) throw KiritoError("take: axis out of range");
        if (axis == 0)
            return tns::wrap([&]() { return tns::g_takeAxis0(vm, self, idxs); });
        // axis > 0: permute the target axis to 0, take along 0, then permute back.
        tensor::Shape perm(static_cast<std::size_t>(nd));
        perm[0] = static_cast<std::size_t>(axis);
        std::size_t fill = 0;
        for (std::size_t i = 1; i < perm.size(); ++i) {
            if (fill == static_cast<std::size_t>(axis)) ++fill;
            perm[i] = fill++;
        }
        tensor::Shape inv(static_cast<std::size_t>(nd));
        for (std::size_t i = 0; i < perm.size(); ++i) inv[perm[i]] = i;
        return tns::wrap([&]() {
            RootScope rs(vm);
            Handle permuted = rs.add(tns::g_permute(vm, self, perm));
            Handle taken = rs.add(tns::g_takeAxis0(vm, permuted, idxs));
            return tns::g_permute(vm, taken, inv);
        });
    });
    // sorting
    if (name == "sort") return bind("sort", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::sortT(vm, self, axis); });
    });
    if (name == "argsort") return bind("argsort", {"axis"}, [self, axisOf](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        int64_t axis = axisOf(vm, a);
        return tns::wrap([&]() { return tns::argsortT(vm, self, axis); });
    });
    // complex helpers
    if (name == "real") return bind("real", {}, [self](KiritoVM& vm, std::span<const Handle>) { return tns::realT(vm, self); });
    if (name == "imag") return bind("imag", {}, [self](KiritoVM& vm, std::span<const Handle>) { return tns::imagT(vm, self); });
    if (name == "conj" || name == "conjugate") return bind("conj", {}, [self](KiritoVM& vm, std::span<const Handle>) { return tns::conjT(vm, self); });
    if (name == "angle") return bind("angle", {}, [self](KiritoVM& vm, std::span<const Handle>) { return tns::wrap([&]() { return tns::angleT(vm, self); }); });

    // ---- autograd surface ----
    if (name == "grad") {  // the accumulated gradient (a Float Tensor), or None
        auto& t = self_t(vm, self);
        if (t.grad) return tns::make(vm, *t.grad);
        return vm.none();
    }
    if (name == "requiresgrad") return bind("requiresgrad", {"flag"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        auto& t = tns::asT(vm, self);
        if (a.empty()) return vm.makeBool(t.requiresGrad);  // getter
        bool f = vm.arena().deref(a[0]).truthy();
        if (f && t.isComplex()) throw KiritoError("gradients are Float-only (a Complex tensor cannot require grad)");
        t.requiresGrad = f;
        if (!f) t.node.reset();  // turning grad off detaches from the graph
        return vm.none();
    });
    if (name == "backward") return bind("backward", {"seed"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        std::optional<FT> seed;
        if (!a.empty() && vm.arena().deref(a[0]).kind() != ValueKind::None) {
            const auto* st = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
            if (!st || st->isComplex()) throw KiritoError("backward: the seed must be a Float Tensor");
            seed = std::get<FT>(st->store);
        }
        tns::runBackward(vm, self, seed);
        return vm.none();
    });
    if (name == "zerograd") return bind("zerograd", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
        tns::asT(vm, self).grad.reset();
        return vm.none();
    });
    if (name == "detach") return bind("detach", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& t = tns::asT(vm, self);
        if (t.isComplex()) return tns::make(vm, std::get<CT>(t.store));
        return tns::make(vm, std::get<FT>(t.store));
    });
    // --- serialization (serialize / dump): only gradient-free tensors may be serialized ---
    if (name == "_getstate_") return bind("_getstate_", {}, [self, self_t](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& t = self_t(vm, self);
        if (t.requiresGrad)
            throw KiritoError("cannot serialize a Tensor that requires grad; call detach() first "
                              "(only gradient-free tensors are serializable)");
        List st(vm);
        st.push(Value(vm, std::string(t.dtypeName())));                 // [0] dtype
        List shapeL(vm);
        for (std::size_t d : t.shape()) shapeL.push(static_cast<int64_t>(d));
        st.push(shapeL);                                      // [1] shape
        List dataL(vm);
        if (t.isComplex()) {
            for (cdouble z : std::get<CT>(t.store).data) { List p(vm); p.push(z.real()); p.push(z.imag()); dataL.push(p); }
        } else {
            for (double x : std::get<FT>(t.store).data) dataL.push(x);
        }
        st.push(dataL);                                       // [2] data (Floats, or [re, im] pairs)
        return st.handle();
    });
    if (name == "_setstate_") return bind("_setstate_", {"state"}, [self, self_t](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "_setstate_").require(1);
        auto& t = self_t(vm, self);
        auto items = Value(vm, a[0]).items();
        if (items.size() < 3) throw KiritoError("Tensor _setstate_: malformed state");
        std::string dtype = items[0].asStringRef("dtype");
        tensor::Shape shape;
        for (Value e : items[1].items()) shape.push_back(static_cast<std::size_t>(e.asInt("dim")));
        tns::checkSize(shape);
        if (dtype == "Complex") {
            std::vector<cdouble> data;
            for (Value e : items[2].items()) {
                auto p = e.items();   // guard: a corrupt/hand-crafted state could give a short pair
                if (p.size() < 2) throw KiritoError("Tensor _setstate_: complex element must be [re, im]");
                data.push_back(cdouble(p[0].asFloat("re"), p[1].asFloat("im")));
            }
            t.store = TensorVal::CT(shape, std::move(data));
        } else {
            std::vector<double> data;
            for (Value e : items[2].items()) data.push_back(e.asFloat("x"));
            t.store = TensorVal::FT(shape, std::move(data));
        }
        t.requiresGrad = false; t.grad.reset(); t.node.reset();      // a loaded tensor is gradient-free
        return vm.none();
    });
    if (name == "pow") return bind("pow", {"p"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "pow").require(1);
        double p = Value(vm, a[0]).asFloat("p");
        return tns::wrap([&]() { return tns::g_pow(vm, self, p); });
    });

    // ---- differentiable element-wise math (a generous unary set) ----
    static const fum::unordered_map<std::string, MathOp> kMath = {
        {"exp", MathOp::Exp}, {"log", MathOp::Log}, {"log10", MathOp::Log10}, {"log2", MathOp::Log2},
        {"sqrt", MathOp::Sqrt}, {"cbrt", MathOp::Cbrt}, {"square", MathOp::Square},
        {"reciprocal", MathOp::Reciprocal}, {"abs", MathOp::Abs}, {"sign", MathOp::Sign},
        {"sin", MathOp::Sin}, {"cos", MathOp::Cos}, {"tan", MathOp::Tan},
        {"asin", MathOp::Asin}, {"acos", MathOp::Acos}, {"atan", MathOp::Atan},
        {"sinh", MathOp::Sinh}, {"cosh", MathOp::Cosh}, {"tanh", MathOp::Tanh},
        {"asinh", MathOp::Asinh}, {"acosh", MathOp::Acosh}, {"atanh", MathOp::Atanh},
        {"relu", MathOp::Relu}, {"sigmoid", MathOp::Sigmoid}, {"softplus", MathOp::Softplus},
        {"erf", MathOp::Erf},
        {"floor", MathOp::Floor}, {"ceil", MathOp::Ceil}, {"round", MathOp::Round}, {"trunc", MathOp::Trunc}};
    if (auto mit = kMath.find(std::string(name)); mit != kMath.end()) {
        MathOp k = mit->second;
        return makeMethod(vm, std::string(name), {}, [self, k](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return tns::wrap([&]() { return tns::g_math(vm, self, k); });
        }, std::vector<Handle>{self});
    }
    return Object::getAttr(vm, self, name);
}

// `with tensor.nograd():` — disable grad tracking for the duration of the block (PyTorch's no_grad).
class NoGradCtx : public NativeClass<NoGradCtx> {
public:
    static constexpr const char* kTypeName = "NoGradContext";
    bool prev = true;
    std::string str(StringifyCtx&) const override { return "<nograd>"; }
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "_enter_") return makeMethod(vm, "_enter_", {}, [self](KiritoVM& v, std::span<const Handle>) -> Handle {
            auto& c = static_cast<NoGradCtx&>(v.arena().deref(self));
            auto& gm = tns::gradMode(v);
            c.prev = gm.enabled;
            gm.enabled = false;
            return self;
        }, std::vector<Handle>{self});
        if (name == "_exit_") return makeMethod(vm, "_exit_", {}, [self](KiritoVM& v, std::span<const Handle>) -> Handle {
            auto& c = static_cast<NoGradCtx&>(v.arena().deref(self));
            tns::gradMode(v).enabled = c.prev;
            return v.none();
        }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);
    }
};

// ----------------------------------------------------------------------------------- the module
class TensorModule : public NativeModule {
public:
    std::string name() const override { return "tensor"; }
    void setup(ModuleBuilder& m) override {
        // The VM-scoped grad-tracking flag (hidden module member consulted by the autograd ops).
        m.value("_grad", m.vm().alloc(std::make_unique<TensorGradFlag>()));
        // Let serialize/dump reconstruct a Tensor: build an empty one; _setstate_ fills it in.
        m.vm().registerDeserializer("Tensor", [](KiritoVM& vm, Handle) -> Handle {
            return vm.alloc(std::make_unique<TensorVal>());
        });

        // Tensor(nested[, dtype][, requiresgrad]) — build from a (rectangular) nested list.
        m.fn("Tensor", {{"data"}, {"dtype", "", m.vm().none()}, {"requiresgrad", "", m.vm().makeBool(false)}}, "Tensor",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "Tensor");
            bool wantC = a.size() > 1 && tns::wantsComplex(vm, a[1]);
            bool reqGrad = a.size() > 2 && vm.arena().deref(a[2]).truthy();
            Value arg = args[0];
            tensor::Shape shape;
            Value cur = arg;
            while (cur.isList()) {
                auto it = cur.items();
                shape.push_back(it.size());
                if (it.empty()) break;
                cur = it[0];
            }
            if (shape.size() > tensor::kMaxRank)  // bound rec() depth up front (before building `shape`)
                throw KiritoError("Tensor: too many dimensions (max 64)");
            tns::checkSize(shape);  // also enforces the rank cap (single-sourced in checkedNumel)
            std::vector<Handle> flat;
            std::function<void(Value, std::size_t)> rec = [&](Value v, std::size_t d) {
                if (d == shape.size()) { flat.push_back(v.handle()); return; }
                if (!v.isList()) throw KiritoError("Tensor: nested list is ragged or irregular");
                auto it = v.items();
                if (it.size() != shape[d]) throw KiritoError("Tensor: nested list is ragged (rows differ in length)");
                for (Value e : it) rec(e, d + 1);
            };
            rec(arg, 0);
            Handle h;
            if (wantC) {
                std::vector<cdouble> data;
                for (Handle hh : flat) data.push_back(cpx::asComplex(vm, hh, "Tensor element"));
                h = tns::make(vm, TensorVal::CT(shape, std::move(data)));
            } else {
                std::vector<double> data;
                for (Handle hh : flat) data.push_back(Value(vm, hh).asFloat("Tensor element"));
                h = tns::make(vm, TensorVal::FT(shape, std::move(data)));
            }
            if (reqGrad) {
                if (wantC) throw KiritoError("gradients are Float-only (a Complex tensor cannot require grad)");
                tns::asT(vm, h).requiresGrad = true;
            }
            return h;
        });
        m.alias("tensor", "Tensor");

        auto filled = [](const char* nm, double fillReal) {
            return [nm, fillReal](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, nm);
                tensor::Shape shape = tns::readShape(args[0]);
                tns::checkSize(shape);
                bool wantC = a.size() > 1 && tns::wantsComplex(vm, a[1]);
                bool reqGrad = a.size() > 2 && vm.arena().deref(a[2]).truthy();
                Handle h;
                if (wantC) h = tns::make(vm, TensorVal::CT(shape, cdouble(fillReal, 0.0)));
                else h = tns::make(vm, TensorVal::FT(shape, fillReal));
                if (reqGrad) {
                    if (wantC) throw KiritoError("gradients are Float-only (a Complex tensor cannot require grad)");
                    tns::asT(vm, h).requiresGrad = true;
                }
                return h;
            };
        };
        m.fn("zeros", {{"shape", "List"}, {"dtype", "", m.vm().none()}, {"requiresgrad", "", m.vm().makeBool(false)}}, "Tensor", filled("zeros", 0.0));
        m.fn("ones", {{"shape", "List"}, {"dtype", "", m.vm().none()}, {"requiresgrad", "", m.vm().makeBool(false)}}, "Tensor", filled("ones", 1.0));
        m.fn("full", {{"shape", "List"}, {"value", "Number"}, {"dtype", "", m.vm().none()}, {"requiresgrad", "", m.vm().makeBool(false)}}, "Tensor",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "full");
            tensor::Shape shape = tns::readShape(args[0]);
            tns::checkSize(shape);
            bool wantC = a.size() > 2 && tns::wantsComplex(vm, a[2]);
            bool reqGrad = a.size() > 3 && vm.arena().deref(a[3]).truthy();
            Handle h;
            if (wantC) h = tns::make(vm, TensorVal::CT(shape, cpx::asComplex(vm, a[1], "full value")));
            else h = tns::make(vm, TensorVal::FT(shape, args[1].asFloat("full value")));
            if (reqGrad) {
                if (wantC) throw KiritoError("gradients are Float-only (a Complex tensor cannot require grad)");
                tns::asT(vm, h).requiresGrad = true;
            }
            return h;
        });
        m.fn("eye", {{"n", "Integer"}, {"dtype", "", m.vm().none()}, {"requiresgrad", "", m.vm().makeBool(false)}}, "Tensor",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            int64_t ni = Args(vm, a, "eye")[0].asInt("n");
            if (ni < 0) throw KiritoError("eye: n must be non-negative");
            std::size_t n = static_cast<std::size_t>(ni);
            tns::checkSize(tensor::Shape{n, n});  // before filling: n*n can overflow size_t -> tiny buffer, OOB write
            bool wantC = a.size() > 1 && tns::wantsComplex(vm, a[1]);
            bool reqGrad = a.size() > 2 && vm.arena().deref(a[2]).truthy();
            Handle h;
            if (wantC) {
                TensorVal::CT t(tensor::Shape{n, n});
                for (std::size_t i = 0; i < n; ++i) t.data[i * n + i] = cdouble(1.0, 0.0);
                h = tns::make(vm, std::move(t));
            } else {
                TensorVal::FT t(tensor::Shape{n, n});
                for (std::size_t i = 0; i < n; ++i) t.data[i * n + i] = 1.0;
                h = tns::make(vm, std::move(t));
            }
            if (reqGrad) {
                if (wantC) throw KiritoError("gradients are Float-only (a Complex tensor cannot require grad)");
                tns::asT(vm, h).requiresGrad = true;
            }
            return h;
        });
        // arange(stop) or arange(start, stop[, step]) -> a 1-D Float tensor. Variadic-by-position like the
        // builtin range, but it also names its parameters so keywords work: arange(stop=5), arange(start=1, stop=9).
        m.kwfn("arange", [](KiritoVM& vm, std::span<const Handle> a, std::span<const NamedArg> named) -> Handle {
            double start = 0, stop = 0, step = 1;
            bool hasStart = false, hasStop = false, hasStep = false;
            auto fv = [&](Handle h, const char* w) { return Value(vm, h).asFloat(w); };
            for (const auto& na : named) {
                if (na.name == "start") { if (hasStart) throw KiritoError("arange() got multiple values for 'start'"); start = fv(na.value, "start"); hasStart = true; }
                else if (na.name == "stop") { if (hasStop) throw KiritoError("arange() got multiple values for 'stop'"); stop = fv(na.value, "stop"); hasStop = true; }
                else if (na.name == "step") { if (hasStep) throw KiritoError("arange() got multiple values for 'step'"); step = fv(na.value, "step"); hasStep = true; }
                else throw KiritoError("arange() got an unexpected keyword argument '" + na.name + "'");
            }
            if (a.size() > 3) throw KiritoError("arange expects 1 to 3 positional arguments");
            if (a.size() == 1 && !hasStop) { stop = fv(a[0], "stop"); hasStop = true; }  // arange(stop) overload
            else {
                if (a.size() >= 1) { if (hasStart) throw KiritoError("arange() got multiple values for 'start'"); start = fv(a[0], "start"); hasStart = true; }
                if (a.size() >= 2) { if (hasStop) throw KiritoError("arange() got multiple values for 'stop'"); stop = fv(a[1], "stop"); hasStop = true; }
                if (a.size() >= 3) { if (hasStep) throw KiritoError("arange() got multiple values for 'step'"); step = fv(a[2], "step"); hasStep = true; }
            }
            if (!hasStop) throw KiritoError("arange: missing stop");
            if (step == 0) throw KiritoError("arange step must be non-zero");
            // Bound the element count BEFORE allocating: the old in-loop guard first grew the vector to
            // ~1 GB. ceil((stop-start)/step) is the count; a non-finite span (inf stop) is rejected
            // instantly, and a NaN span collapses to 0 (an empty tensor, the existing NaN behaviour).
            double span = stop - start;
            double countf = ((span > 0) == (step > 0)) ? std::ceil(span / step) : 0.0;
            if (!std::isfinite(countf) || countf > static_cast<double>(tns::kMaxElems))
                throw KiritoError("Tensor too large");
            std::vector<double> data;
            data.reserve(countf > 0 ? static_cast<std::size_t>(countf) : 0);
            if (step > 0) for (double x = start; x < stop; x += step) { data.push_back(x); if (data.size() > tns::kMaxElems) throw KiritoError("Tensor too large"); }
            else for (double x = start; x > stop; x += step) { data.push_back(x); if (data.size() > tns::kMaxElems) throw KiritoError("Tensor too large"); }
            std::size_t n = data.size();
            return tns::make(vm, TensorVal::FT(tensor::Shape{n}, std::move(data)));
        });

        // tensordot(a, b, axes): contract over axes. `axes` is an Integer N (the last N axes of `a`
        // with the first N of `b`) or a [a-axes, b-axes] pair (each an Integer or a List). Built from
        // permute/reshape/matmul, so it is differentiable under autograd.
        m.fn("tensordot", {{"a", "Tensor"}, {"b", "Tensor"}, {"axes", "", m.vm().makeInt(2)}}, "Tensor",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "tensordot");
            if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0])) || !dynamic_cast<const TensorVal*>(&vm.arena().deref(a[1])))
                throw KiritoError("tensordot expects two Tensors");
            Value ax = args[2];
            std::vector<int64_t> aaxes, baxes;
            if (ax.isInt()) {
                int64_t n = ax.asInt("axes");
                if (n < 0) throw KiritoError("tensordot: the axis count must be non-negative");
                std::size_t an = tns::asT(vm, a[0]).ndim();
                if (static_cast<std::size_t>(n) > an) throw KiritoError("tensordot: axis count exceeds the tensor rank");
                for (int64_t i = 0; i < n; ++i) { aaxes.push_back(static_cast<int64_t>(an) - n + i); baxes.push_back(i); }
            } else if (ax.isList()) {
                auto it = ax.items();
                if (it.size() != 2) throw KiritoError("tensordot: axes must be an Integer or a [a-axes, b-axes] pair");
                aaxes = tns::readAxisList(it[0]);
                baxes = tns::readAxisList(it[1]);
            } else {
                throw KiritoError("tensordot: axes must be an Integer or a [a-axes, b-axes] pair");
            }
            return tns::wrap([&]() { return tns::tensordot(vm, a[0], a[1], aaxes, baxes); });
        });
        // contract(a, b, aaxes, baxes): tensordot with the two axis lists given explicitly.
        m.fn("contract", {{"a", "Tensor"}, {"b", "Tensor"}, {"aaxes"}, {"baxes"}}, "Tensor",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "contract");
            if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0])) || !dynamic_cast<const TensorVal*>(&vm.arena().deref(a[1])))
                throw KiritoError("contract expects two Tensors");
            std::vector<int64_t> aaxes = tns::readAxisList(args[2]);
            std::vector<int64_t> baxes = tns::readAxisList(args[3]);
            return tns::wrap([&]() { return tns::tensordot(vm, a[0], a[1], aaxes, baxes); });
        });

        // nograd(): a context manager that disables grad tracking inside a `with` block.
        m.fn("nograd", {}, "NoGradContext", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return vm.alloc(std::make_unique<NoGradCtx>());
        });

        // ---- creation helpers ----
        m.fn("linspace", {{"start", "Number"}, {"stop", "Number"}, {"num", "Integer", m.vm().makeInt(50)}}, "Tensor",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "linspace");
            double start = args[0].asFloat("start"), stop = args[1].asFloat("stop");
            int64_t num = args[2].asInt("num");
            if (num < 0) throw KiritoError("linspace: num must be non-negative");
            std::size_t n = static_cast<std::size_t>(num);
            tns::checkSize({n});
            std::vector<double> data(n);
            for (std::size_t i = 0; i < n; ++i) data[i] = n == 1 ? start : start + (stop - start) * static_cast<double>(i) / static_cast<double>(n - 1);
            return tns::make(vm, TensorVal::FT(tensor::Shape{n}, std::move(data)));
        });
        auto likeFn = [](const char* nm, double fill, bool useVal) {
            return [nm, fill, useVal](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, nm);
                const auto* t = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
                if (!t) throw KiritoError(std::string(nm) + " expects a Tensor");
                double v = useVal ? args[1].asFloat("value") : fill;
                if (t->isComplex()) return tns::make(vm, TensorVal::CT(t->shape(), cdouble(v, 0.0)));
                return tns::make(vm, TensorVal::FT(t->shape(), v));
            };
        };
        m.fn("zeroslike", {{"t", "Tensor"}}, "Tensor", likeFn("zeroslike", 0.0, false));
        m.fn("oneslike", {{"t", "Tensor"}}, "Tensor", likeFn("oneslike", 1.0, false));
        m.fn("fulllike", {{"t", "Tensor"}, {"value", "Number"}}, "Tensor", likeFn("fulllike", 0.0, true));
        m.alias("identity", "eye");
        // diag: a 1-D tensor -> a diagonal matrix; a 2-D tensor -> its diagonal (Float only)
        m.fn("diag", {{"t", "Tensor"}, {"k", "Integer", m.vm().makeInt(0)}}, "Tensor", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const TensorVal::FT& t = tns::reqFloat(tns::reqT(vm, a[0], "diag"), "diag");
            int64_t k = Args(vm, a, "diag")[1].asInt("k");
            // |k| as an unsigned magnitude — negating INT64_MIN would be UB. A huge |k| makes the
            // result shape exceed the size cap and throw "Tensor too large" cleanly.
            std::size_t koff = static_cast<std::size_t>(k < 0 ? (0ull - static_cast<uint64_t>(k)) : static_cast<uint64_t>(k));
            return tns::wrap([&]() -> Handle {
                if (t.ndim() == 1) {
                    std::size_t n = t.size() + koff;
                    tns::checkSize(tensor::Shape{n, n});  // bound n*n -> clean "Tensor too large", not bad_alloc
                    TensorVal::FT out(tensor::Shape{n, n});
                    for (std::size_t i = 0; i < t.size(); ++i) {
                        std::size_t r = k >= 0 ? i : i + koff;
                        std::size_t c = k >= 0 ? i + koff : i;
                        out.data[r * n + c] = t.data[i];
                    }
                    return tns::make(vm, std::move(out));
                }
                if (t.ndim() == 2) {
                    std::size_t rows = t.shape[0], cols = t.shape[1];
                    std::vector<double> d;
                    for (std::size_t i = 0;; ++i) {
                        std::size_t r = k >= 0 ? i : i + koff;
                        std::size_t c = k >= 0 ? i + koff : i;
                        if (r >= rows || c >= cols) break;
                        d.push_back(t.data[r * cols + c]);
                    }
                    std::size_t n = d.size();
                    return tns::make(vm, TensorVal::FT(tensor::Shape{n}, std::move(d)));
                }
                throw KiritoError("diag expects a 1-D or 2-D tensor");
            });
        });
        auto triFn = [](const char* nm, bool lower) {
            return [nm, lower](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                const TensorVal::FT& t = tns::reqFloat(tns::reqT(vm, a[0], nm), nm);
                int64_t k = Args(vm, a, nm)[1].asInt("k");
                if (t.ndim() != 2) throw KiritoError(std::string(nm) + " expects a 2-D tensor");
                std::size_t rows = t.shape[0], cols = t.shape[1];
                TensorVal::FT out = t;
                for (std::size_t i = 0; i < rows; ++i)
                    for (std::size_t j = 0; j < cols; ++j) {
                        int64_t diff = static_cast<int64_t>(j) - static_cast<int64_t>(i);  // bounded by the matrix dims; avoids `i + k` overflow for extreme k
                        bool keep = lower ? (diff <= k) : (diff >= k);
                        if (!keep) out.data[i * cols + j] = 0.0;
                    }
                return tns::make(vm, std::move(out));
            };
        };
        m.fn("tril", {{"t", "Tensor"}, {"k", "Integer", m.vm().makeInt(0)}}, "Tensor", triFn("tril", true));
        m.fn("triu", {{"t", "Tensor"}, {"k", "Integer", m.vm().makeInt(0)}}, "Tensor", triFn("triu", false));

        // ---- structural ----
        auto readTensorList = [](KiritoVM& vm, Handle h) -> std::vector<Handle> {
            std::vector<Handle> hs;
            for (Value e : Value(vm, h).items()) {
                if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(e.handle()))) throw KiritoError("expected a List of Tensors");
                hs.push_back(e.handle());
            }
            if (hs.empty()) throw KiritoError("expected at least one Tensor");
            return hs;
        };
        m.fn("concatenate", {{"tensors", "List"}, {"axis", "Integer", m.vm().makeInt(0)}}, "Tensor",
             [readTensorList](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::vector<Handle> hs = readTensorList(vm, a[0]);
            int64_t axisRaw = Args(vm, a, "concatenate")[1].asInt("axis");
            std::size_t nd = static_cast<const TensorVal&>(vm.arena().deref(hs[0])).ndim();
            if (axisRaw < 0) axisRaw += static_cast<int64_t>(nd);  // NumPy-style negative-axis wrap (as stack does)
            if (axisRaw < 0 || static_cast<std::size_t>(axisRaw) >= nd) throw KiritoError("concatenate axis out of range");
            std::size_t axis = static_cast<std::size_t>(axisRaw);
            return tns::wrap([&]() { return tns::g_concat(vm, hs, axis); });
        });
        m.alias("concat", "concatenate");
        m.fn("stack", {{"tensors", "List"}, {"axis", "Integer", m.vm().makeInt(0)}}, "Tensor",
             [readTensorList](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::vector<Handle> hs = readTensorList(vm, a[0]);
            int64_t axis = Args(vm, a, "stack")[1].asInt("axis");
            return tns::wrap([&]() { return tns::g_stack(vm, hs, axis); });
        });
        m.fn("split", {{"t", "Tensor"}, {"sections", "List"}, {"axis", "Integer", m.vm().makeInt(0)}}, "List",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const auto* t = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
            if (!t) throw KiritoError("split expects a Tensor");
            int64_t axisRaw = Args(vm, a, "split")[2].asInt("axis");
            if (axisRaw < 0) axisRaw += static_cast<int64_t>(t->ndim());  // NumPy-style negative-axis wrap (as stack does)
            if (axisRaw < 0 || static_cast<std::size_t>(axisRaw) >= t->ndim()) throw KiritoError("split axis out of range");
            std::size_t axis = static_cast<std::size_t>(axisRaw);
            std::vector<std::size_t> sizes;
            Value secs = Value(vm, a[1]);
            if (secs.isInt()) {  // split into N equal parts
                int64_t n = secs.asInt("sections");
                if (n <= 0 || t->shape()[axis] % static_cast<std::size_t>(n) != 0) throw KiritoError("split: axis length is not divisible into that many sections");
                std::size_t each = t->shape()[axis] / static_cast<std::size_t>(n);
                for (int64_t i = 0; i < n; ++i) sizes.push_back(each);
            } else {
                std::size_t total = 0;
                for (Value e : secs.items()) {
                    int64_t si = e.asInt("section");
                    if (si < 0) throw KiritoError("split: section sizes must be non-negative");  // else the
                        // size_t cast wraps to ~SIZE_MAX, the sum overflows past the axis length, and
                        // g_split reads out of bounds (heap-buffer-overflow).
                    std::size_t s = static_cast<std::size_t>(si);
                    sizes.push_back(s); total += s;
                }
                if (total != t->shape()[axis]) throw KiritoError("split: section sizes must sum to the axis length");
            }
            return tns::wrap([&]() { return tns::g_split(vm, a[0], sizes, axis); });
        });
        m.fn("where", {{"cond", "Tensor"}, {"a", "Tensor"}, {"b", "Tensor"}}, "Tensor", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            for (int i = 0; i < 3; ++i) if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[i]))) throw KiritoError("where expects three Tensors");
            return tns::wrap([&]() { return tns::g_where(vm, a[0], a[1], a[2]); });
        });

        // ---- linear algebra ----
        auto la1 = [](const char* nm, Handle (*fn)(KiritoVM&, Handle)) {
            return [nm, fn](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]))) throw KiritoError(std::string(nm) + " expects a Tensor");
                return tns::wrap([&]() { return fn(vm, a[0]); });
            };
        };
        m.fn("det", {{"t", "Tensor"}}, "Number", la1("det", tns::detT));
        m.fn("inv", {{"t", "Tensor"}}, "Tensor", la1("inv", tns::invT));
        m.fn("trace", {{"t", "Tensor"}}, "Number", la1("trace", tns::traceT));
        m.fn("solve", {{"a", "Tensor"}, {"b", "Tensor"}}, "Tensor", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            for (int i = 0; i < 2; ++i) if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[i]))) throw KiritoError("solve expects two Tensors");
            return tns::wrap([&]() { return tns::solveT(vm, a[0], a[1]); });
        });
        m.fn("norm", {{"t", "Tensor"}, {"ord", "Number", m.vm().makeInt(2)}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]))) throw KiritoError("norm expects a Tensor");
            double ord = Args(vm, a, "norm")[1].asFloat("ord");
            return tns::wrap([&]() { return tns::normT(vm, a[0], ord); });
        });
        auto la2 = [](const char* nm, Handle (*fn)(KiritoVM&, Handle, Handle)) {
            return [nm, fn](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                for (int i = 0; i < 2; ++i) if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[i]))) throw KiritoError(std::string(nm) + " expects two Tensors");
                return tns::wrap([&]() { return fn(vm, a[0], a[1]); });
            };
        };
        m.fn("outer", {{"a", "Tensor"}, {"b", "Tensor"}}, "Tensor", la2("outer", tns::outerT));
        m.fn("kron", {{"a", "Tensor"}, {"b", "Tensor"}}, "Tensor", la2("kron", tns::kronT));
        m.fn("cross", {{"a", "Tensor"}, {"b", "Tensor"}}, "Tensor", la2("cross", tns::crossT));
        m.fn("inner", {{"a", "Tensor"}, {"b", "Tensor"}}, "Tensor", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const auto* x = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]));
            const auto* y = dynamic_cast<const TensorVal*>(&vm.arena().deref(a[1]));
            if (!x || !y) throw KiritoError("inner expects two Tensors");
            return tns::wrap([&]() -> Handle {
                std::vector<int64_t> ax{static_cast<int64_t>(x->ndim()) - 1}, bx{static_cast<int64_t>(y->ndim()) - 1};
                return tns::tensordot(vm, a[0], a[1], ax, bx);
            });
        });
        m.fn("einsum", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {  // variadic: (spec, *tensors)
            if (a.empty()) throw KiritoError("einsum needs a subscripts string and at least one tensor");
            std::string spec = Value(vm, a[0]).asStringRef("einsum subscripts");
            std::vector<Handle> ops(a.begin() + 1, a.end());
            return tns::wrap([&]() { return tns::einsumT(vm, spec, ops); });
        });

        // ---- sorting / searching ----
        m.fn("unique", {{"t", "Tensor"}}, "Tensor", la1("unique", tns::uniqueT));
        m.fn("nonzero", {{"t", "Tensor"}}, "List", la1("nonzero", tns::nonzeroT));
        m.fn("searchsorted", {{"a", "Tensor"}, {"v"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {  // Integer for a scalar v, Tensor for a tensor v
            if (!dynamic_cast<const TensorVal*>(&vm.arena().deref(a[0]))) throw KiritoError("searchsorted expects a sorted 1-D Tensor");
            return tns::wrap([&]() { return tns::searchsortedT(vm, a[0], a[1]); });
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
