#ifndef KIRITO_STDLIB_COMPLEX_HPP
#define KIRITO_STDLIB_COMPLEX_HPP

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <memory>
#include <numbers>
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

using cdouble = std::complex<double>;

// Tolerant complex comparison (rel/abs tolerance): |a-b| <= max(rel_tol*max(|a|,|b|), abs_tol), with |.|
// the complex magnitude. NaN is never close; a non-equal infinity is never close. Powers the tolerant
// `.compare()` on Complex / ComplexMatrix / Tensor (their `==` is EXACT). Local so this header needn't
// depend on runtime.hpp's floatClose (which is included after the stdlib headers in the umbrella).
inline bool cClose(cdouble a, cdouble b, double relTol, double absTol) {
    if (a == b) return true;  // exact (covers identical infinities)
    if (std::isnan(a.real()) || std::isnan(a.imag()) || std::isnan(b.real()) || std::isnan(b.imag()) ||
        std::isinf(a.real()) || std::isinf(a.imag()) || std::isinf(b.real()) || std::isinf(b.imag()))
        return false;
    double diff = std::abs(a - b);
    return diff <= std::max(relTol * std::max(std::abs(a), std::abs(b)), absTol);
}

// Render a complex value as "a+bi" / "a-bi" (negative zero on the imaginary part normalized away).
inline std::string complexToString(cdouble z) {
    double im = z.imag();
    if (im == 0.0) im = 0.0;  // collapse -0.0 -> +0.0 so we never print "+-0.0i"
    std::string s = floatToString(z.real());
    if (im < 0.0) return s + "-" + floatToString(-im) + "i";
    return s + "+" + floatToString(im) + "i";
}

// ------------------------------------------------------------------- the Complex number value
class ComplexVal : public NativeClass<ComplexVal> {
public:
    static constexpr const char* kTypeName = "Complex";
    cdouble z;

    ComplexVal() = default;
    explicit ComplexVal(cdouble v) : z(v) {}
    ComplexVal(double re, double im) : z(re, im) {}

    bool truthy() const override { return z != cdouble(0.0, 0.0); }
    std::string str(StringifyCtx&) const override { return complexToString(z); }
    bool equals(const ObjectArena&, const Object& other) const override {
        // EXACT IEEE-754, like scalar Float == (std::complex::== compares real and imag bit-exactly,
        // so NaN never equals anything). For approximate comparison use the .compare() method.
        if (const auto* c = dynamic_cast<const ComplexVal*>(&other))
            return z == c->z;
        // A Complex equals a real number when its imaginary part is zero (so Complex(2,0) == 2).
        if (other.kind() == ValueKind::Integer)
            return z.imag() == 0.0 && z.real() == static_cast<double>(static_cast<const IntVal&>(other).value());
        if (other.kind() == ValueKind::Float)
            return z.imag() == 0.0 && z.real() == static_cast<const FloatVal&>(other).value();
        return false;
    }

    std::vector<std::string> inspectMembers() const override {
        return {"re: Float", "im: Float", "compare(other, rel_tol = 1e-09, abs_tol = 0.0) -> Bool",
                "conjugate() -> Complex", "modulus() -> Float",
                "argument() -> Float", "norm2() -> Float", "is_zero() -> Bool"};
    }

    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override;
    Handle unary(KiritoVM& vm, UnOp op, Handle self) override;
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
};

namespace cpx {

// Read a Complex, Integer or Float as std::complex<double> (numbers sit on the real axis).
inline cdouble asComplex(KiritoVM& vm, Handle h, const char* who = "Complex") {
    const Object& o = vm.arena().deref(h);
    if (const auto* c = dynamic_cast<const ComplexVal*>(&o)) return c->z;
    if (o.kind() == ValueKind::Integer) return cdouble(static_cast<double>(static_cast<const IntVal&>(o).value()), 0.0);
    if (o.kind() == ValueKind::Float) return cdouble(static_cast<const FloatVal&>(o).value(), 0.0);
    throw KiritoError(std::string(who) + " expects a Complex or a number");
}

inline Handle make(KiritoVM& vm, cdouble z) { return vm.alloc(std::make_unique<ComplexVal>(z)); }

// Complex exponentiation with the singularities handled explicitly: zero to a negative or
// non-real power throws (`0j ** -1` / `0j ** 1j` is a domain error; std::pow would emit inf/nan),
// while `0 ** 0 == 1` (matching Kirito's scalar `0 ** 0`; std::pow gives nan).
// Other bases/powers are well-defined and go straight through std::pow.
inline void checkPow(cdouble base, cdouble exp) {
    if (base == cdouble(0.0, 0.0) && (exp.real() < 0.0 || exp.imag() != 0.0))
        throw KiritoError("complex pow: zero to a negative or complex power");
}
inline cdouble cpow(cdouble base, cdouble exp) {
    checkPow(base, exp);
    if (base == cdouble(0.0, 0.0) && exp == cdouble(0.0, 0.0)) return cdouble(1.0, 0.0);  // 0 ** 0 == 1
    return std::pow(base, exp);
}

}  // namespace cpx

inline Handle ComplexVal::binary(KiritoVM& vm, BinOp op, Handle, Handle rhs) {
    // Equality is handled by the evaluator via equals(); ordering is undefined for complex numbers.
    if (op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge)
        throw KiritoError("complex numbers are not ordered (no <, <=, >, >=)");
    cdouble b = cpx::asComplex(vm, rhs, "Complex arithmetic");
    switch (op) {
        case BinOp::Add: { return cpx::make(vm, z + b); } break;
        case BinOp::Sub: { return cpx::make(vm, z - b); } break;
        case BinOp::Mul: { return cpx::make(vm, z * b); } break;
        case BinOp::Div: {
            if (b == cdouble(0.0, 0.0)) throw KiritoError("complex division by zero");
            return cpx::make(vm, z / b);
        } break;
        case BinOp::Pow: { return cpx::make(vm, cpx::cpow(z, b)); } break;
        case BinOp::Eq: { return vm.makeBool(z == b); } break;   // EXACT (std::complex::==); .compare() for tolerance
        case BinOp::Ne: { return vm.makeBool(z != b); } break;
        default: { } break;
    }
    throw KiritoError("Complex does not support this operator");
}

inline Handle ComplexVal::unary(KiritoVM& vm, UnOp op, Handle) {
    if (op == UnOp::Neg) return cpx::make(vm, -z);
    throw KiritoError("Complex does not support this unary operator");
}

inline Handle ComplexVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto self_z = [](KiritoVM& vm, Handle self) -> cdouble {
        return static_cast<ComplexVal&>(vm.arena().deref(self)).z;
    };
    auto bind = [&](const char* nm, NativeFn fn) {
        return makeMethod(vm, nm, {}, std::move(fn), std::vector<Handle>{self});
    };
    if (name == "re" || name == "real") return vm.makeFloat(z.real());
    if (name == "im" || name == "imag") return vm.makeFloat(z.imag());
    // .compare(other, rel_tol=1e-9, abs_tol=0.0) -> Bool — tolerant comparison (rel/abs tolerance), since
    // `==` is now exact. Signatured so it takes keyword args/defaults and shows under inspect.
    if (name == "compare") {
        std::vector<NativeParam> sig;
        sig.emplace_back("other");
        sig.emplace_back("rel_tol", "Float", vm.makeFloat(1e-9));
        sig.emplace_back("abs_tol", "Float", vm.makeFloat(0.0));
        return vm.alloc(std::make_unique<NativeFunction>(
            "compare", std::move(sig), "Bool",
            [self](KiritoVM& v, std::span<const Handle> a) -> Handle {
                cdouble me = static_cast<ComplexVal&>(v.arena().deref(self)).z;
                cdouble other = cpx::asComplex(v, a[0], "compare");
                return v.makeBool(cClose(me, other, Value(v, a[1]).asFloat("rel_tol"),
                                         Value(v, a[2]).asFloat("abs_tol")));
            },
            std::vector<Handle>{self}));
    }
    if (name == "conjugate") return bind("conjugate", [self, self_z](KiritoVM& vm, std::span<const Handle>) { return cpx::make(vm, std::conj(self_z(vm, self))); });
    if (name == "modulus" || name == "magnitude" || name == "abs")
        return bind("modulus", [self, self_z](KiritoVM& vm, std::span<const Handle>) { return vm.makeFloat(std::abs(self_z(vm, self))); });
    if (name == "argument" || name == "arg" || name == "phase")
        return bind("argument", [self, self_z](KiritoVM& vm, std::span<const Handle>) { return vm.makeFloat(std::arg(self_z(vm, self))); });
    if (name == "norm2")
        return bind("norm2", [self, self_z](KiritoVM& vm, std::span<const Handle>) { return vm.makeFloat(std::norm(self_z(vm, self))); });
    if (name == "is_zero")  // a near-zero predicate (deliberately tolerant, unlike exact ==)
        return bind("is_zero", [self, self_z](KiritoVM& vm, std::span<const Handle>) { return vm.makeBool(std::norm(self_z(vm, self)) < 1e-20); });
    // --- serialization (serialize / dump): a Complex round-trips as [re, im]. ---
    if (name == "_getstate_")
        return bind("_getstate_", [self, self_z](KiritoVM& vm, std::span<const Handle>) -> Handle {
            cdouble v = self_z(vm, self);
            List st(vm); st.add(v.real()); st.add(v.imag());
            return st.build().handle();
        });
    if (name == "_setstate_")
        return makeMethod(vm, "_setstate_", {"state"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            requireArgs(a, 1, "_setstate_");
            auto items = Value(vm, a[0]).items();
            if (items.size() < 2) throw KiritoError("Complex _setstate_: malformed state");
            static_cast<ComplexVal&>(vm.arena().deref(self)).z =
                cdouble(items[0].asFloat("re"), items[1].asFloat("im"));
            return vm.none();
        }, std::vector<Handle>{self});
    return Object::getAttr(vm, self, name);
}

// ------------------------------------------------------------------- the complex Matrix value
// A dense complex matrix: a rank-2 `tensor::Tensor<cdouble>`. Its operations are the tensor engine's
// — the same code that backs the real `matrix`, instantiated for std::complex<double>.
class ComplexMatrixVal : public NativeClass<ComplexMatrixVal> {
public:
    static constexpr const char* kTypeName = "ComplexMatrix";
    tensor::Tensor<cdouble> t;  // t.shape == {rows, cols}

    ComplexMatrixVal() = default;
    ComplexMatrixVal(std::size_t r, std::size_t c, cdouble fill = cdouble(0.0, 0.0))
        : t(tensor::Shape{r, c}, fill) {}
    explicit ComplexMatrixVal(tensor::Tensor<cdouble> tt) : t(std::move(tt)) {}

    std::size_t rows() const { return t.shape.empty() ? 0 : t.shape[0]; }
    std::size_t cols() const { return t.shape.size() < 2 ? 0 : t.shape[1]; }
    std::vector<cdouble>& data() { return t.data; }
    const std::vector<cdouble>& data() const { return t.data; }
    cdouble& at(std::size_t r, std::size_t c) { return t.data[r * cols() + c]; }
    cdouble at(std::size_t r, std::size_t c) const { return t.data[r * cols() + c]; }

    // A ComplexMatrix with one dimension == 1 is a vector (row 1×n or column n×1).
    bool isVector() const { return rows() == 1 || cols() == 1; }

    std::string str(StringifyCtx&) const override {
        std::string s = "[";
        for (std::size_t r = 0; r < rows(); ++r) {
            if (r) s += ", ";
            s += "[";
            for (std::size_t c = 0; c < cols(); ++c) {
                if (c) s += ", ";
                s += complexToString(at(r, c));
            }
            s += "]";
        }
        return s + "]";
    }
    bool equals(const ObjectArena&, const Object& other) const override {
        // EXACT (same shape, every element bit-equal), like scalar Complex/Float ==. NaN never equal.
        // For a tolerant compare of computed matrices use `.compare(other, rel_tol, abs_tol)`.
        const auto* m = dynamic_cast<const ComplexMatrixVal*>(&other);
        if (!m || m->t.shape != t.shape) return false;
        for (std::size_t i = 0; i < t.data.size(); ++i)
            if (t.data[i] != m->t.data[i]) return false;
        return true;
    }
    std::vector<std::string> inspectMembers() const override {
        return {"rows() -> Integer", "cols() -> Integer", "shape() -> List",
                "compare(other, rel_tol = 1e-09, abs_tol = 0.0) -> Bool",
                "get(row, col) -> Complex", "set(row, col, value)", "row(i) -> List",
                "transpose() -> ComplexMatrix", "conjugate() -> ComplexMatrix",
                "hermitian() -> ComplexMatrix", "determinant() -> Complex",
                "inverse() -> ComplexMatrix", "trace() -> Complex", "apply(fn) -> ComplexMatrix",
                "dot(other) -> Complex", "cross(other) -> ComplexMatrix", "norm() -> Float"};
    }

    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override;
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
    Handle getItem(KiritoVM& vm, std::span<const Handle> keys) override;
    void setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) override;

    static std::size_t indexOf(KiritoVM& vm, Handle h) {
        const Object& o = vm.arena().deref(h);
        if (o.kind() != ValueKind::Integer) throw KiritoError("ComplexMatrix index must be Integer");
        int64_t v = static_cast<const IntVal&>(o).value();
        if (v < 0) throw KiritoError("ComplexMatrix index out of range");
        return static_cast<std::size_t>(v);
    }
};

namespace cpx {

inline constexpr std::size_t kMaxElems = 16ull * 1024 * 1024;
inline std::unique_ptr<ComplexMatrixVal> makeMatrix(std::size_t r, std::size_t c, cdouble fill = cdouble(0.0, 0.0)) {
    if (c != 0 && r > kMaxElems / c) throw KiritoError("ComplexMatrix too large");
    return std::make_unique<ComplexMatrixVal>(r, c, fill);
}
inline std::unique_ptr<ComplexMatrixVal> fromTensor(tensor::Tensor<cdouble> tt) {
    if (tt.ndim() == 2 && tt.shape[1] != 0 && tt.shape[0] > kMaxElems / tt.shape[1])
        throw KiritoError("ComplexMatrix too large");
    return std::make_unique<ComplexMatrixVal>(std::move(tt));
}

// Determinant (Gaussian elimination) and inverse (Gauss-Jordan) come from the shared tensor engine;
// these wrappers add the ComplexMatrix-specific square check and translate engine errors.
inline cdouble determinant(const ComplexMatrixVal& m) {
    if (m.rows() != m.cols()) throw KiritoError("determinant requires a square ComplexMatrix");
    return tensor::determinant(m.t);
}
inline std::unique_ptr<ComplexMatrixVal> inverse(const ComplexMatrixVal& m) {
    if (m.rows() != m.cols()) throw KiritoError("inverse requires a square ComplexMatrix");
    try { return fromTensor(tensor::inverse(m.t)); }
    catch (const tensor::TensorError& e) { throw KiritoError(e.what()); }
}

}  // namespace cpx

inline Handle ComplexMatrixVal::binary(KiritoVM& vm, BinOp op, Handle, Handle rhs) {
    const Object& b = vm.arena().deref(rhs);
    const auto* other = dynamic_cast<const ComplexMatrixVal*>(&b);
    try {
        if (op == BinOp::Add || op == BinOp::Sub) {
            if (!other || other->t.shape != t.shape)
                throw KiritoError("ComplexMatrix +/- requires matrices of equal shape");
            return vm.alloc(cpx::fromTensor(op == BinOp::Add ? tensor::add(t, other->t)
                                                            : tensor::sub(t, other->t)));
        }
        if (op == BinOp::Mul) {
            if (other) {  // matrix product (the inner product of two vectors is `u.dot(v)`, not `u * v`)
                if (cols() != other->rows()) throw KiritoError("ComplexMatrix multiply: inner dimensions differ");
                return vm.alloc(cpx::fromTensor(tensor::matmul(t, other->t)));
            }
            cdouble s = cpx::asComplex(vm, rhs, "ComplexMatrix scalar");  // scalar (Complex/number)
            return vm.alloc(cpx::fromTensor(tensor::scalarOp(t, s, '*')));
        }
    } catch (const tensor::TensorError& e) {
        throw KiritoError(e.what());
    }
    throw KiritoError("ComplexMatrix does not support this operator");
}

inline Handle ComplexMatrixVal::getItem(KiritoVM& vm, std::span<const Handle> keys) {
    if (keys.size() == 1) {  // a whole row, as a List of Complex
        std::size_t r = indexOf(vm, keys[0]);
        if (r >= rows()) throw KiritoError("ComplexMatrix row index out of range");
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        for (std::size_t c = 0; c < cols(); ++c) list->elems.push_back(rs.add(cpx::make(vm, at(r, c))));
        return vm.alloc(std::move(list));
    }
    if (keys.size() == 2) {  // a single element
        std::size_t r = indexOf(vm, keys[0]), c = indexOf(vm, keys[1]);
        if (r >= rows() || c >= cols()) throw KiritoError("ComplexMatrix index out of range");
        return cpx::make(vm, at(r, c));
    }
    throw KiritoError("ComplexMatrix index needs 1 (row) or 2 (element) indices");
}

inline void ComplexMatrixVal::setItem(KiritoVM& vm, std::span<const Handle> keys, Handle value) {
    if (keys.size() != 2) throw KiritoError("ComplexMatrix element assignment needs two indices: m[i, j] = v");
    std::size_t r = indexOf(vm, keys[0]), c = indexOf(vm, keys[1]);
    if (r >= rows() || c >= cols()) throw KiritoError("ComplexMatrix index out of range");
    at(r, c) = cpx::asComplex(vm, value, "ComplexMatrix element");
}

inline Handle ComplexMatrixVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    auto self_m = [](KiritoVM& vm, Handle self) -> ComplexMatrixVal& {
        return static_cast<ComplexMatrixVal&>(vm.arena().deref(self));
    };
    auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
        return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
    };
    auto idx = [](KiritoVM& vm, Handle h) -> std::size_t { return ComplexMatrixVal::indexOf(vm, h); };
    if (name == "rows") return bind("rows", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) { return vm.makeInt(static_cast<int64_t>(self_m(vm, self).rows())); });
    if (name == "cols") return bind("cols", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) { return vm.makeInt(static_cast<int64_t>(self_m(vm, self).cols())); });
    if (name == "shape") return bind("shape", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        auto list = std::make_unique<ListVal>();
        list->elems.push_back(vm.makeInt(static_cast<int64_t>(m.rows())));
        list->elems.push_back(vm.makeInt(static_cast<int64_t>(m.cols())));
        return vm.alloc(std::move(list));
    });
    // compare(other, rel_tol=1e-9, abs_tol=0.0) -> Bool — tolerant whole-matrix comparison (cClose
    // per element), since `==` is now exact. Signatured: keyword args/defaults + inspect.
    if (name == "compare") {
        std::vector<NativeParam> sig;
        sig.emplace_back("other");
        sig.emplace_back("rel_tol", "Float", vm.makeFloat(1e-9));
        sig.emplace_back("abs_tol", "Float", vm.makeFloat(0.0));
        return vm.alloc(std::make_unique<NativeFunction>(
            "compare", std::move(sig), "Bool",
            [self](KiritoVM& v, std::span<const Handle> a) -> Handle {
                auto& m = static_cast<ComplexMatrixVal&>(v.arena().deref(self));
                const auto* o = dynamic_cast<const ComplexMatrixVal*>(&v.arena().deref(a[0]));
                if (!o) throw KiritoError("compare expects a ComplexMatrix");
                if (o->t.shape != m.t.shape) return v.makeBool(false);
                double rel = Value(v, a[1]).asFloat("rel_tol"), abst = Value(v, a[2]).asFloat("abs_tol");
                for (std::size_t i = 0; i < m.t.data.size(); ++i)
                    if (!cClose(m.t.data[i], o->t.data[i], rel, abst)) return v.makeBool(false);
                return v.makeBool(true);
            },
            std::vector<Handle>{self}));
    }
    if (name == "get") return bind("get", {"row", "col"}, [self, self_m, idx](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        requireArgs(a, 2, "get");
        auto& m = self_m(vm, self);
        std::size_t r = idx(vm, a[0]), c = idx(vm, a[1]);
        if (r >= m.rows() || c >= m.cols()) throw KiritoError("ComplexMatrix index out of range");
        return cpx::make(vm, m.at(r, c));
    });
    if (name == "set") return bind("set", {"row", "col", "value"}, [self, self_m, idx](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        requireArgs(a, 3, "set");
        auto& m = self_m(vm, self);
        std::size_t r = idx(vm, a[0]), c = idx(vm, a[1]);
        if (r >= m.rows() || c >= m.cols()) throw KiritoError("ComplexMatrix index out of range");
        m.at(r, c) = cpx::asComplex(vm, a[2], "ComplexMatrix element");
        return vm.none();
    });
    if (name == "row") return bind("row", {"i"}, [self, self_m, idx](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        requireArgs(a, 1, "row");
        auto& m = self_m(vm, self);
        std::size_t r = idx(vm, a[0]);
        if (r >= m.rows()) throw KiritoError("row index out of range");
        RootScope rs(vm);
        auto list = std::make_unique<ListVal>();
        for (std::size_t c = 0; c < m.cols(); ++c) list->elems.push_back(rs.add(cpx::make(vm, m.at(r, c))));
        return vm.alloc(std::move(list));
    });
    if (name == "transpose") return bind("transpose", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        auto t = cpx::makeMatrix(m.cols(), m.rows());
        for (std::size_t r = 0; r < m.rows(); ++r)
            for (std::size_t c = 0; c < m.cols(); ++c) t->at(c, r) = m.at(r, c);
        return vm.alloc(std::move(t));
    });
    if (name == "conjugate") return bind("conjugate", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        auto out = cpx::makeMatrix(m.rows(), m.cols());
        for (std::size_t i = 0; i < m.data().size(); ++i) out->data()[i] = std::conj(m.data()[i]);
        return vm.alloc(std::move(out));
    });
    // hermitian / conjugate transpose: (M*)^T
    if (name == "hermitian" || name == "conjugatetranspose") return bind("hermitian", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        auto t = cpx::makeMatrix(m.cols(), m.rows());
        for (std::size_t r = 0; r < m.rows(); ++r)
            for (std::size_t c = 0; c < m.cols(); ++c) t->at(c, r) = std::conj(m.at(r, c));
        return vm.alloc(std::move(t));
    });
    if (name == "determinant") return bind("determinant", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) { return cpx::make(vm, cpx::determinant(self_m(vm, self))); });
    if (name == "inverse") return bind("inverse", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) { return vm.alloc(cpx::inverse(self_m(vm, self))); });
    if (name == "trace") return bind("trace", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        if (m.rows() != m.cols()) throw KiritoError("trace requires a square ComplexMatrix");
        cdouble s(0.0, 0.0);
        for (std::size_t i = 0; i < m.rows(); ++i) s += m.at(i, i);
        return cpx::make(vm, s);
    });
    // apply(fn): map fn over every element, returning a new ComplexMatrix (the element-wise map).
    if (name == "apply") return bind("apply", {"fn"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        requireArgs(a, 1, "apply");
        Handle fn = a[0];
        auto& m = self_m(vm, self);
        auto out = cpx::makeMatrix(m.rows(), m.cols());
        for (std::size_t i = 0; i < m.data().size(); ++i) {
            RootScope rs(vm);
            std::array<Handle, 1> args{rs.add(cpx::make(vm, m.data()[i]))};
            out->data()[i] = cpx::asComplex(vm, vm.arena().deref(fn).call(vm, args), "apply result");
        }
        return vm.alloc(std::move(out));
    });
    // --- vector operations (a ComplexMatrix with one dimension == 1 is a vector) ---
    if (name == "dot") return bind("dot", {"other"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        requireArgs(a, 1, "dot");
        auto& m = self_m(vm, self);
        const auto* o = dynamic_cast<const ComplexMatrixVal*>(&vm.arena().deref(a[0]));
        if (!o) throw KiritoError("dot expects a ComplexMatrix vector");
        if (!m.isVector() || !o->isVector()) throw KiritoError("dot requires vectors (a 1×n or n×1 ComplexMatrix)");
        if (m.data().size() != o->data().size()) throw KiritoError("dot requires vectors of equal length");
        cdouble acc(0.0, 0.0);  // Hermitian inner product: sum conj(a_i)·b_i
        for (std::size_t i = 0; i < m.data().size(); ++i) acc += std::conj(m.data()[i]) * o->data()[i];
        return cpx::make(vm, acc);
    });
    if (name == "cross") return bind("cross", {"other"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        requireArgs(a, 1, "cross");
        auto& m = self_m(vm, self);
        const auto* o = dynamic_cast<const ComplexMatrixVal*>(&vm.arena().deref(a[0]));
        if (!o) throw KiritoError("cross expects a ComplexMatrix vector");
        if (!m.isVector() || !o->isVector() || m.data().size() != 3 || o->data().size() != 3)
            throw KiritoError("cross is only defined for two 3-element vectors");
        const auto& u = m.data(); const auto& v = o->data();
        auto r = cpx::makeMatrix(m.rows(), m.cols());  // keep this vector's orientation
        r->data() = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        return vm.alloc(std::move(r));
    });
    if (name == "norm") return bind("norm", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);  // Euclidean / Frobenius 2-norm: sqrt(sum |z_i|²) -> Float
        double acc = 0.0;
        for (const cdouble& z : m.data()) acc += std::norm(z);
        return vm.makeFloat(std::sqrt(acc));
    });
    // --- serialization (serialize / dump): round-trips as [rows, cols, [[re, im], ...]]. ---
    if (name == "_getstate_") return bind("_getstate_", {}, [self, self_m](KiritoVM& vm, std::span<const Handle>) -> Handle {
        auto& m = self_m(vm, self);
        List st(vm);
        st.add(static_cast<int64_t>(m.rows()));
        st.add(static_cast<int64_t>(m.cols()));
        List data(vm);
        for (const cdouble& z : m.data()) { List p(vm); p.add(z.real()); p.add(z.imag()); data.add(p.build()); }
        st.add(data.build());
        return st.build().handle();
    });
    if (name == "_setstate_") return bind("_setstate_", {"state"}, [self, self_m](KiritoVM& vm, std::span<const Handle> a) -> Handle {
        requireArgs(a, 1, "_setstate_");
        auto& m = self_m(vm, self);
        auto items = Value(vm, a[0]).items();
        if (items.size() < 3) throw KiritoError("ComplexMatrix _setstate_: malformed state");
        auto r64 = items[0].asInt("rows"), c64 = items[1].asInt("cols");
        if (r64 < 0 || c64 < 0) throw KiritoError("ComplexMatrix _setstate_: dimensions must be non-negative");
        std::size_t r = static_cast<std::size_t>(r64);
        std::size_t c = static_cast<std::size_t>(c64);
        if (c != 0 && r > cpx::kMaxElems / c) throw KiritoError("ComplexMatrix too large");
        std::vector<cdouble> data;
        for (Value e : items[2].items()) {
            auto p = e.items();   // guard: a corrupt/hand-crafted state could give a short pair
            if (p.size() < 2) throw KiritoError("ComplexMatrix _setstate_: element must be [re, im]");
            data.push_back(cdouble(p[0].asFloat("re"), p[1].asFloat("im")));
        }
        if (data.size() != r * c) throw KiritoError("ComplexMatrix _setstate_: data size does not match shape");
        m.t = tensor::Tensor<cdouble>(tensor::Shape{r, c}, std::move(data));
        return vm.none();
    });
    return Object::getAttr(vm, self, name);
}

// ------------------------------------------------------------------------------- the module
class ComplexModule : public NativeModule {
public:
    std::string name() const override { return "complex"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();

        // Let serialize/dump reconstruct Complex / ComplexMatrix: build an empty one; _setstate_ fills it.
        vm.registerDeserializer("Complex", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<ComplexVal>());
        });
        vm.registerDeserializer("ComplexMatrix", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<ComplexMatrixVal>());
        });

        // Constructors. Complex(re[, im]); of(re, im); real(re); the unit imaginary i.
        m.fn("Complex", {{"re", "Number"}, {"im", "", vm.makeInt(0)}}, "Complex", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "Complex");
            double re = args[0].asFloat("Complex re");
            double im = args.size() > 1 ? args[1].asFloat("Complex im") : 0.0;
            return vm.alloc(std::make_unique<ComplexVal>(re, im));
        });
        m.fn("of", {{"re", "Number"}, {"im", "Number"}}, "Complex", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "of");
            return vm.alloc(std::make_unique<ComplexVal>(args[0].asFloat("of re"), args[1].asFloat("of im")));
        });
        m.fn("real", {{"re", "Number"}}, "Complex", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return vm.alloc(std::make_unique<ComplexVal>(Args(vm, a, "real")[0].asFloat("real"), 0.0));
        });
        // polar(r, theta) -> r·(cos θ + i sin θ); rect == Complex.
        m.fn("polar", {{"r", "Number"}, {"theta", "Number"}}, "Complex", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "polar");
            return cpx::make(vm, std::polar(args[0].asFloat("polar r"), args[1].asFloat("polar theta")));
        });
        m.value("zero", cpx::make(vm, cdouble(0.0, 0.0)));
        m.value("one", cpx::make(vm, cdouble(1.0, 0.0)));
        m.value("i", cpx::make(vm, cdouble(0.0, 1.0)));
        // pi/e/tau are real-axis constants with no Complex-specific value over math.pi/e/tau —
        // use those (single source of truth) rather than a duplicate copy here.

        // Scalar reductions: modulus/abs, phase/argument, norm2, conjugate. `abs`/`argument` are
        // aliases of `modulus`/`phase` (one shared impl registered under both names — single source
        // of truth, matching the ComplexVal::getAttr method-alias pattern above).
        auto modulusFn = [](KiritoVM& vm, std::span<const Handle> a) -> Handle { return Value(vm, std::abs(cpx::asComplex(vm, a[0]))); };
        m.fn("modulus", {{"z"}}, "Float", modulusFn);
        m.fn("abs", {{"z"}}, "Float", modulusFn);
        auto phaseFn = [](KiritoVM& vm, std::span<const Handle> a) -> Handle { return Value(vm, std::arg(cpx::asComplex(vm, a[0]))); };
        m.fn("phase", {{"z"}}, "Float", phaseFn);
        m.fn("argument", {{"z"}}, "Float", phaseFn);
        m.fn("norm2", {{"z"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle { return Value(vm, std::norm(cpx::asComplex(vm, a[0]))); });
        m.fn("conjugate", {{"z"}}, "Complex", [](KiritoVM& vm, std::span<const Handle> a) -> Handle { return cpx::make(vm, std::conj(cpx::asComplex(vm, a[0]))); });

        // Analytic functions — the complex extensions of the `math` module's transcendental set.
        // Each accepts a Complex or a real number and returns a Complex. `bad` is an optional domain
        // predicate (a non-capturing lambda → function pointer): when it returns a message, throw it
        // instead of letting std:: emit silent inf/nan at a true singularity. Like the `math` module,
        // Kirito's complex analytic functions throw at exactly the genuine singularities — log/log10 of
        // zero, atanh of ±1 — while remaining defined on the rest of the plane (e.g. sqrt(-1)=i,
        // log(-1)=iπ, asin(2), acosh(0) are all valid and must NOT throw).
        auto unary = [&](const char* nm, cdouble (*f)(const cdouble&), const char* (*bad)(const cdouble&) = nullptr) {
            m.fn(nm, {{"z"}}, "Complex", [f, bad, nm](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                cdouble z = cpx::asComplex(vm, a[0]);
                if (bad) { const char* msg = bad(z); if (msg) throw KiritoError(std::string(nm) + ": " + msg); }
                return cpx::make(vm, f(z));
            });
        };
        auto logZero = [](const cdouble& z) -> const char* {
            return z == cdouble(0.0, 0.0) ? "math domain error (logarithm of zero)" : nullptr;
        };
        unary("exp", [](const cdouble& z) { return std::exp(z); });
        unary("log", [](const cdouble& z) { return std::log(z); }, logZero);
        unary("log10", [](const cdouble& z) { return std::log10(z); }, logZero);
        unary("sqrt", [](const cdouble& z) { return std::sqrt(z); });
        unary("sin", [](const cdouble& z) { return std::sin(z); });
        unary("cos", [](const cdouble& z) { return std::cos(z); });
        unary("tan", [](const cdouble& z) { return std::tan(z); });
        unary("asin", [](const cdouble& z) { return std::asin(z); });
        unary("acos", [](const cdouble& z) { return std::acos(z); });
        unary("atan", [](const cdouble& z) { return std::atan(z); });
        unary("sinh", [](const cdouble& z) { return std::sinh(z); });
        unary("cosh", [](const cdouble& z) { return std::cosh(z); });
        unary("tanh", [](const cdouble& z) { return std::tanh(z); });
        unary("asinh", [](const cdouble& z) { return std::asinh(z); });
        unary("acosh", [](const cdouble& z) { return std::acosh(z); });
        unary("atanh", [](const cdouble& z) { return std::atanh(z); },
              [](const cdouble& z) -> const char* {
                  return (z == cdouble(1.0, 0.0) || z == cdouble(-1.0, 0.0))
                             ? "math domain error (atanh of ±1)" : nullptr;
              });
        // pow(z, w) and a cube root (no std::cbrt for complex; use the principal value). Zero thrown
        // to a negative or non-real power is a singularity (a domain error there).
        m.fn("pow", {{"z"}, {"w"}}, "Complex", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.size() != 2) throw KiritoError("complex.pow expects 2 arguments");
            return cpx::make(vm, cpx::cpow(cpx::asComplex(vm, a[0]), cpx::asComplex(vm, a[1])));
        });
        m.fn("cbrt", {{"z"}}, "Complex", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return cpx::make(vm, std::pow(cpx::asComplex(vm, a[0]), cdouble(1.0 / 3.0, 0.0)));
        });

        // -------- complex matrices --------
        // Matrix(rows) — a nested list whose cells may be Complex, Integer or Float (reals go on the
        // real axis). Signatured, so it accepts the `rows=` keyword and shows up under inspect.
        m.fn("Matrix", {{"rows", "List"}}, "ComplexMatrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "Matrix");
            if (args.size() != 1) throw KiritoError("Matrix expects a nested list of rows");
            std::vector<std::vector<cdouble>> grid;
            for (Value row : args[0].items()) {
                std::vector<cdouble> r;
                for (Value cell : row.items()) r.push_back(cpx::asComplex(vm, cell.handle(), "Matrix element"));
                grid.push_back(std::move(r));
            }
            std::size_t r = grid.size(), c = r ? grid[0].size() : 0;
            for (const auto& row : grid)
                if (row.size() != c) throw KiritoError("Matrix rows must have equal length");
            auto mtx = cpx::makeMatrix(r, c);
            for (std::size_t i = 0; i < r; ++i)
                for (std::size_t j = 0; j < c; ++j) mtx->at(i, j) = grid[i][j];
            return vm.alloc(std::move(mtx));
        });
        m.fn("zeros", {{"rows", "Integer"}, {"cols", "Integer"}}, "ComplexMatrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "zeros");
            auto rows = args[0].asInt("rows"), cols = args[1].asInt("cols");
            if (rows < 0 || cols < 0) throw KiritoError("complex.zeros: dimensions must be non-negative");
            return vm.alloc(cpx::makeMatrix(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols)));
        });
        m.fn("ones", {{"rows", "Integer"}, {"cols", "Integer"}}, "ComplexMatrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "ones");
            auto rows = args[0].asInt("rows"), cols = args[1].asInt("cols");
            if (rows < 0 || cols < 0) throw KiritoError("complex.ones: dimensions must be non-negative");
            return vm.alloc(cpx::makeMatrix(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols), cdouble(1.0, 0.0)));
        });
        m.fn("identity", {{"n", "Integer"}}, "ComplexMatrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto n64 = Args(vm, a, "identity")[0].asInt("n");
            if (n64 < 0) throw KiritoError("complex.identity: dimension must be non-negative");
            std::size_t n = static_cast<std::size_t>(n64);
            auto mtx = cpx::makeMatrix(n, n);
            for (std::size_t i = 0; i < n; ++i) mtx->at(i, i) = cdouble(1.0, 0.0);
            return vm.alloc(std::move(mtx));
        });
        // vector(list) -> a 1×n row-vector ComplexMatrix (cells may be Complex or numbers).
        m.fn("vector", {{"values", "List"}}, "ComplexMatrix", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::vector<cdouble> xs;
            for (Value e : Args(vm, a, "vector")[0].items()) xs.push_back(cpx::asComplex(vm, e.handle(), "vector element"));
            std::size_t n = xs.size();
            return vm.alloc(cpx::fromTensor(tensor::Tensor<cdouble>(tensor::Shape{1, n}, std::move(xs))));
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
