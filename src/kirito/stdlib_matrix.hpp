#ifndef KIRITO_STDLIB_MATRIX_HPP
#define KIRITO_STDLIB_MATRIX_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "builtins.hpp"
#include "collections.hpp"
#include "native.hpp"
#include "tensor.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// A dense real-valued matrix: a rank-2 `tensor::Tensor<double>`. The heavy operations (multiply,
// transpose, determinant, inverse) are the tensor engine's — this class is just the Kirito-facing
// 2-D view, with the familiar matrix API and the `*`-means-matrix-multiply convention.
class MatrixVal : public NativeClass<MatrixVal> {
public:
    static constexpr const char* kTypeName = "Matrix";
    tensor::Tensor<double> t;  // t.shape == {rows, cols}

    MatrixVal() = default;
    MatrixVal(std::size_t r, std::size_t c, double fill = 0.0) : t(tensor::Shape{r, c}, fill) {}
    explicit MatrixVal(tensor::Tensor<double> tt) : t(std::move(tt)) {}

    std::size_t rows() const { return t.shape.empty() ? 0 : t.shape[0]; }
    std::size_t cols() const { return t.shape.size() < 2 ? 0 : t.shape[1]; }
    std::vector<double>& data() { return t.data; }
    const std::vector<double>& data() const { return t.data; }
    double& at(std::size_t r, std::size_t c) { return t.data[r * cols() + c]; }
    double at(std::size_t r, std::size_t c) const { return t.data[r * cols() + c]; }
    bool isVector() const { return rows() == 1 || cols() == 1; }

    std::string str(StringifyCtx&) const override {
        std::string s = "[";
        for (std::size_t r = 0; r < rows(); ++r) {
            if (r) s += ", ";
            s += "[";
            for (std::size_t c = 0; c < cols(); ++c) {
                if (c) s += ", ";
                s += floatToString(at(r, c));
            }
            s += "]";
        }
        return s + "]";
    }
    bool equals(const ObjectArena&, const Object& other) const override {
        // EXACT (same shape, every element bit-equal), like scalar Float ==. NaN never equal.
        // Matrices hold computed floats, so for a tolerant compare use `.compare(other, rel_tol, abs_tol)`.
        const auto* m = dynamic_cast<const MatrixVal*>(&other);
        if (!m || m->t.shape != t.shape) return false;
        for (std::size_t i = 0; i < t.data.size(); ++i)
            if (t.data[i] != m->t.data[i]) return false;
        return true;
    }

    std::vector<std::string> inspectMembers() const override {
        return {
            "rows() -> Integer", "cols() -> Integer", "shape() -> List",
            "compare(other, rel_tol = 1e-09, abs_tol = 0.0) -> Bool",
            "get(row, col) -> Float", "set(row, col, value)", "row(i) -> List",
            "transpose() -> Matrix", "determinant() -> Float", "inverse() -> Matrix",
            "trace() -> Float", "sum() -> Float", "apply(fn) -> Matrix",
            "dot(other) -> Float", "cross(other) -> Matrix", "norm() -> Float",
        };
    }

    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override;
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
    // m[i] -> a row (List); m[i, j] -> an element; m[i, j] = v -> set element.
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override;
    void setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) override;

    static std::size_t indexOf(KiritoVM& vm, Handle h) {
        const Object& o = vm.arena().deref(h);
        if (o.kind() != ValueKind::Integer) throw KiritoError("Matrix index must be Integer");
        int64_t v = static_cast<const IntVal&>(o).value();
        if (v < 0) throw KiritoError("Matrix index out of range");
        return static_cast<std::size_t>(v);
    }
};

namespace mat {

inline double numOf(KiritoVM& vm, Handle h) {
    return Value(vm, h).asFloat("Matrix");
}

// Cap total element count so a hostile/absurd dimension throws a catchable error instead of an
// uncaught std::bad_alloc (which would terminate the VM). ~16M doubles = 128 MB is plenty for CPU.
inline constexpr std::size_t kMaxMatrixElems = 16ull * 1024 * 1024;
inline std::unique_ptr<MatrixVal> make(std::size_t r, std::size_t c, double fill = 0.0) {
    if (c != 0 && r > kMaxMatrixElems / c) throw KiritoError("Matrix too large");
    return std::make_unique<MatrixVal>(r, c, fill);
}
inline std::unique_ptr<MatrixVal> fromTensor(tensor::Tensor<double> tt) {
    if (tt.ndim() == 2 && tt.shape[1] != 0 && tt.shape[0] > kMaxMatrixElems / tt.shape[1])
        throw KiritoError("Matrix too large");
    return std::make_unique<MatrixVal>(std::move(tt));
}

}  // namespace mat

inline Handle MatrixVal::binary(KiritoVM& vm, BinOp op, Handle, Handle rhs) {
    const Object& b = vm.arena().deref(rhs);
    const auto* other = dynamic_cast<const MatrixVal*>(&b);
    try {
        if (op == BinOp::Add || op == BinOp::Sub) {
            if (!other || other->t.shape != t.shape)
                throw KiritoError("Matrix +/- requires Matrices of equal shape");
            return vm.alloc(mat::fromTensor(op == BinOp::Add ? tensor::add(t, other->t)
                                                             : tensor::sub(t, other->t)));
        }
        if (op == BinOp::Mul) {
            if (other) {  // matrix multiply (the dot product of two vectors is `u.dot(v)`, not `u * v`)
                if (cols() != other->rows()) throw KiritoError("Matrix multiply: inner dimensions differ");
                return vm.alloc(mat::fromTensor(tensor::matmul(t, other->t)));
            }
            return vm.alloc(mat::fromTensor(tensor::scalarOp(t, mat::numOf(vm, rhs), '*')));  // scalar
        }
    } catch (const tensor::TensorError& e) {
        throw KiritoError(e.what());
    }
    throw KiritoError("Matrix does not support this operator");
}

inline Handle MatrixVal::getItem(KiritoVM& vm, std::span<const Handle> keys) {
    if (keys.size() == 1) {  // a whole row, as a List
        std::size_t r = indexOf(vm, keys[0]);
        if (r >= rows()) throw KiritoError("Matrix row index out of range");
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        for (std::size_t c = 0; c < cols(); ++c) list->elems.push_back(rs.add(vm.makeFloat(at(r, c))));
        return vm.alloc(std::move(list));
    }
    if (keys.size() == 2) {  // a single element
        std::size_t r = indexOf(vm, keys[0]), c = indexOf(vm, keys[1]);
        if (r >= rows() || c >= cols()) throw KiritoError("Matrix index out of range");
        return vm.makeFloat(at(r, c));
    }
    throw KiritoError("Matrix index needs 1 (row) or 2 (element) indices");
}

inline void MatrixVal::setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) {
    if (keys.size() != 2) throw KiritoError("Matrix element assignment needs two indices: m[i, j] = v");
    std::size_t r = indexOf(vm, keys[0]), c = indexOf(vm, keys[1]);
    if (r >= rows() || c >= cols()) throw KiritoError("Matrix index out of range");
    at(r, c) = mat::numOf(vm, value);
}

inline Handle MatrixVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    auto self_m = [](KiritoVM& vm, Handle self) -> MatrixVal& {
        return static_cast<MatrixVal&>(vm.arena().deref(self));
    };
    auto idx = [](KiritoVM& vm, Handle h) -> std::size_t { return MatrixVal::indexOf(vm, h); };
    if (name == "rows") return bind("rows", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) { return vm.makeInt(static_cast<int64_t>(self_m(vm, self).rows())); });
    if (name == "cols") return bind("cols", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) { return vm.makeInt(static_cast<int64_t>(self_m(vm, self).cols())); });
    if (name == "shape") return bind("shape", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        // rows()/cols() past the small-int intern range allocate, and the List is not arena-reachable
        // until the alloc below — root them, or the second collects the first. (v1.15 A19-1.)
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        list->elems.push_back(rs.add(vm.makeInt(static_cast<int64_t>(m.rows()))));
        list->elems.push_back(rs.add(vm.makeInt(static_cast<int64_t>(m.cols()))));
        return vm.alloc(std::move(list));
    });
    // compare(other, rel_tol=1e-9, abs_tol=0.0) -> Bool — tolerant whole-matrix comparison
    // (rel/abs tolerance per element), since `==` is now exact. Signatured: keyword args/defaults + inspect.
    if (name == "compare") {
        RootScope rs(vm);
        return vm.alloc(std::make_unique<NativeFunction>(
            "compare", toleranceSig(vm, rs), "Bool",
            [self](KiritoVM& v, std::span<const Handle> a) -> Handle {
                auto& m = static_cast<MatrixVal&>(v.arena().deref(self));
                const auto* o = dynamic_cast<const MatrixVal*>(&v.arena().deref(a[0]));
                if (!o) throw KiritoError("compare expects a Matrix");
                if (o->t.shape != m.t.shape) return v.makeBool(false);
                double rel = Value(v, a[1]).asFloat("rel_tol"), abst = Value(v, a[2]).asFloat("abs_tol");
                for (std::size_t i = 0; i < m.t.data.size(); ++i)
                    if (!floatClose(m.t.data[i], o->t.data[i], rel, abst)) return v.makeBool(false);
                return v.makeBool(true);
            },
            std::vector<Handle>{self}));
    }
    if (name == "get") return bind("get", {"row", "col"}, [self, self_m, idx](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "get").require(2);
        auto& m = self_m(vm, self);
        std::size_t r = idx(vm, a[0]), c = idx(vm, a[1]);
        if (r >= m.rows() || c >= m.cols()) throw KiritoError("Matrix index out of range");
        return vm.makeFloat(m.at(r, c));
    });
    if (name == "set") return bind("set", {"row", "col", "value"}, [self, self_m, idx](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "set").require(3);
        auto& m = self_m(vm, self);
        std::size_t r = idx(vm, a[0]), c = idx(vm, a[1]);
        if (r >= m.rows() || c >= m.cols()) throw KiritoError("Matrix index out of range");
        m.at(r, c) = mat::numOf(vm, a[2]);
        return vm.none();
    });
    if (name == "transpose") return bind("transpose", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) {
        return vm.alloc(mat::fromTensor(tensor::transpose(self_m(vm, self).t)));
    });
    if (name == "determinant") return bind("determinant", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        if (m.rows() != m.cols()) throw KiritoError("determinant requires a square Matrix");
        try { return vm.makeFloat(tensor::determinant(m.t)); }        // translate the engine error, as `inverse` does
        catch (const tensor::TensorError& e) { throw KiritoError(e.what()); }
    });
    if (name == "inverse") return bind("inverse", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        if (m.rows() != m.cols()) throw KiritoError("inverse requires a square Matrix");
        try { return vm.alloc(mat::fromTensor(tensor::inverse(m.t))); }
        catch (const tensor::TensorError& e) { throw KiritoError(e.what()); }
    });
    if (name == "sum") return bind("sum", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) { return vm.makeFloat(tensor::sumAll(self_m(vm, self).t)); });
    if (name == "trace") return bind("trace", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        if (m.rows() != m.cols()) throw KiritoError("trace requires a square Matrix");
        return vm.makeFloat(tensor::trace(m.t));
    });
    if (name == "apply") return bind("apply", {"fn"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "apply").require(1);
        Handle fn = a[0];
        auto& m = self_m(vm, self);
        auto out = mat::make(m.rows(), m.cols());
        for (std::size_t i = 0; i < m.data().size(); ++i) {
            RootScope rs(vm);  // root the arg across the callback — call() allocates a scope, so an
            std::array<Handle, 1> args{rs.add(vm.makeFloat(m.data()[i]))};  // un-rooted arg could be swept (A16-1)
            out->data()[i] = mat::numOf(vm, vm.arena().deref(fn).call(vm, args));
        }
        return vm.alloc(std::move(out));
    });
    if (name == "row") return bind("row", {"i"}, [self, self_m, idx](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "row").require(1);
        auto& m = self_m(vm, self);
        std::size_t r = idx(vm, a[0]);
        if (r >= m.rows()) throw KiritoError("Matrix row index out of range");  // match getItem's text
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        for (std::size_t c = 0; c < m.cols(); ++c) list->elems.push_back(rs.add(vm.makeFloat(m.at(r, c))));
        return vm.alloc(std::move(list));
    });
    // --- vector operations (a Matrix with one dimension == 1 is a vector) ---
    if (name == "dot") return bind("dot", {"other"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "dot").require(1);
        auto& m = self_m(vm, self);
        const auto* o = dynamic_cast<const MatrixVal*>(&vm.arena().deref(a[0]));
        if (!o) throw KiritoError("dot expects a Matrix vector");
        if (!m.isVector() || !o->isVector()) throw KiritoError("dot requires vectors (a 1×n or n×1 Matrix)");
        if (m.data().size() != o->data().size()) throw KiritoError("dot requires vectors of equal length");
        double acc = 0.0;
        for (std::size_t i = 0; i < m.data().size(); ++i) acc += m.data()[i] * o->data()[i];
        return vm.makeFloat(acc);
    });
    if (name == "cross") return bind("cross", {"other"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "cross").require(1);
        auto& m = self_m(vm, self);
        const auto* o = dynamic_cast<const MatrixVal*>(&vm.arena().deref(a[0]));
        if (!o) throw KiritoError("cross expects a Matrix vector");
        if (!m.isVector() || !o->isVector() || m.data().size() != 3 || o->data().size() != 3)
            throw KiritoError("cross is only defined for two 3-element vectors");
        const auto& u = m.data(); const auto& v = o->data();
        auto r = mat::make(m.rows(), m.cols());  // result keeps this vector's orientation
        r->data() = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        return vm.alloc(std::move(r));
    });
    if (name == "norm") return bind("norm", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);  // Euclidean / Frobenius 2-norm (= the vector length for a vector)
        double acc = 0.0;
        for (double x : m.data()) acc += x * x;
        return vm.makeFloat(std::sqrt(acc));
    });
    // --- serialization (serialize / dump): a Matrix is a pure value, so it round-trips as
    // [rows, cols, [elements...]]. The matrix module registers the matching deserializer. ---
    if (name == "_getstate_") return bind("_getstate_", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        List st(vm);
        st.push(static_cast<int64_t>(m.rows()));
        st.push(static_cast<int64_t>(m.cols()));
        List data(vm);
        for (double x : m.data()) data.push(x);
        st.push(data);
        return st.handle();
    });
    if (name == "_setstate_") return bind("_setstate_", {"state"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        Args(vm, a, "_setstate_").require(1);
        auto& m = self_m(vm, self);
        auto items = Value(vm, a[0]).items();
        if (items.size() < 3) throw KiritoError("Matrix _setstate_: malformed state");
        int64_t r64 = items[0].asInt("rows"), c64 = items[1].asInt("cols");
        // A negative dim would cast to a huge size_t and slip past the element-count guard (c==0
        // skips it), yielding a matrix with rows()==-1 from untrusted state. Reject it up front
        // (the complex-matrix sibling already does). ints are non-negative counts.
        if (r64 < 0 || c64 < 0) throw KiritoError("Matrix _setstate_: negative dimension");
        std::size_t r = static_cast<std::size_t>(r64);
        std::size_t c = static_cast<std::size_t>(c64);
        if (c != 0 && r > mat::kMaxMatrixElems / c) throw KiritoError("Matrix too large");
        std::vector<double> data;
        for (Value e : items[2].items()) data.push_back(e.asFloat("element"));
        if (data.size() != r * c) throw KiritoError("Matrix _setstate_: data size does not match shape");
        m.t = tensor::Tensor<double>(tensor::Shape{r, c}, std::move(data));
        return vm.none();
    });
    return Object::getAttr(vm, self, name);
}

class MatrixModule : public NativeModule {
public:
    std::string name() const override { return "matrix"; }
    void setup(ModuleBuilder& m) override {
        // Let serialize/dump reconstruct a Matrix: build an empty one; _setstate_ fills it in.
        m.vm().registerDeserializer("Matrix", [](KiritoVM& vm, Handle) -> Handle {
            return vm.alloc(std::make_unique<MatrixVal>());
        });
        // Matrix(nested-list) or Matrix(rows, cols) — the (rows, cols) form also accepts the rows=/cols= keywords.
        m.kwfn("Matrix", [](KiritoVM& vm, std::span<const Handle> a, std::span<const NamedArg> named) -> Handle {
            if (!named.empty() || a.size() == 2) {                       // (rows, cols) form
                bool hasR = false, hasC = false; int64_t r = 0, c = 0;
                for (const auto& na : named) {
                    if (na.name == "rows") { r = Value(vm, na.value).asInt("Matrix rows"); hasR = true; }
                    else if (na.name == "cols") { c = Value(vm, na.value).asInt("Matrix cols"); hasC = true; }
                    else throw KiritoError("Matrix() got an unexpected keyword argument '" + na.name + "'");
                }
                if (a.size() > 2) throw KiritoError("Matrix expects a nested list or (rows, cols)");
                if (a.size() >= 1) { if (hasR) throw KiritoError("Matrix() got multiple values for 'rows'"); r = Value(vm, a[0]).asInt("Matrix rows"); hasR = true; }
                if (a.size() >= 2) { if (hasC) throw KiritoError("Matrix() got multiple values for 'cols'"); c = Value(vm, a[1]).asInt("Matrix cols"); hasC = true; }
                if (!hasR || !hasC) throw KiritoError("Matrix(rows, cols) needs both rows and cols");
                if (r < 0 || c < 0) throw KiritoError("Matrix dimensions must be non-negative");
                return vm.alloc(mat::make(static_cast<std::size_t>(r), static_cast<std::size_t>(c)));
            }
            Args args(vm, a, "Matrix");
            if (args.size() != 1) throw KiritoError("Matrix expects a nested list or (rows, cols)");
            std::vector<std::vector<double>> grid;
            for (Value row : args[0].items()) {
                std::vector<double> r;
                for (Value cell : row.items()) r.push_back(cell.asFloat("Matrix element"));
                grid.push_back(std::move(r));
            }
            std::size_t r = grid.size(), c = r ? grid[0].size() : 0;
            for (const auto& row : grid)
                if (row.size() != c) throw KiritoError("Matrix rows must have equal length");
            auto mtx = mat::make(r, c);
            for (std::size_t i = 0; i < r; ++i)
                for (std::size_t j = 0; j < c; ++j) mtx->at(i, j) = grid[i][j];
            return vm.alloc(std::move(mtx));
        });
        m.fn("zeros", {{"rows", "Integer"}, {"cols", "Integer"}}, "Matrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "zeros");
            auto rows = args[0].asInt("rows"), cols = args[1].asInt("cols");
            if (rows < 0 || cols < 0) throw KiritoError("matrix.zeros: dimensions must be non-negative");
            return vm.alloc(mat::make(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols), 0.0));
        });
        m.fn("ones", {{"rows", "Integer"}, {"cols", "Integer"}}, "Matrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "ones");
            auto rows = args[0].asInt("rows"), cols = args[1].asInt("cols");
            if (rows < 0 || cols < 0) throw KiritoError("matrix.ones: dimensions must be non-negative");
            return vm.alloc(mat::make(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols), 1.0));
        });
        m.fn("identity", {{"n", "Integer"}}, "Matrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto n64 = Args(vm, a, "identity")[0].asInt("n");
            if (n64 < 0) throw KiritoError("matrix.identity: dimension must be non-negative");
            std::size_t n = static_cast<std::size_t>(n64);
            auto mtx = mat::make(n, n);
            for (std::size_t i = 0; i < mtx->rows(); ++i) mtx->at(i, i) = 1.0;
            return vm.alloc(std::move(mtx));
        });
        // vector(list) -> a 1×n row-vector Matrix (a flat list of numbers), the natural vector shape.
        m.fn("vector", {{"values", "List"}}, "Matrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::vector<double> xs;
            for (Value e : Args(vm, a, "vector")[0].items()) xs.push_back(e.asFloat("vector element"));
            std::size_t n = xs.size();
            return vm.alloc(mat::fromTensor(tensor::Tensor<double>(tensor::Shape{1, n}, std::move(xs))));
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
