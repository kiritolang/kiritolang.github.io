#ifndef KIRITO_STDLIB_MATH_HPP
#define KIRITO_STDLIB_MATH_HPP

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>

#include "builtins.hpp"
#include "native.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// Read a numeric argument (Integer or Float) as a double, via the ergonomic Value API.
inline double mathNum(KiritoVM& vm, Handle h) {
    return Value(vm, h).asFloat("math");
}

// Convert a double to int64 safely: casting a NaN/inf/out-of-range double to int64 is UB, so guard.
inline int64_t toInt64Checked(double d, const char* who) {
    if (std::isnan(d)) throw KiritoError(std::string(who) + ": cannot convert NaN to Integer");
    if (std::isinf(d)) throw KiritoError(std::string(who) + ": cannot convert infinity to Integer");
    if (d >= 9223372036854775808.0 || d < -9223372036854775808.0)
        throw KiritoError(std::string(who) + ": result out of Integer range");
    return static_cast<int64_t>(d);
}

// The `math` standard module: constants and the usual functions. Unary functions return Float;
// floor/ceil/factorial/gcd return Integer.
class MathModule : public NativeModule {
public:
    std::string name() const override { return "math"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
        m.value("pi", Value(vm, std::numbers::pi));
        m.value("e", Value(vm, std::numbers::e));
        m.value("tau", Value(vm, 2.0 * std::numbers::pi));
        m.value("inf", Value(vm, HUGE_VAL));
        m.value("nan", Value(vm, std::nan("")));

        // `ok` is an optional domain predicate: if the (non-NaN) argument is outside the function's
        // mathematical domain, throw a clear "math domain error" instead of returning NaN/inf rubbish.
        // A NaN argument always passes through (math.sqrt(nan) -> nan), and overflow to
        // inf (e.g. exp(1000)) is a range, not a domain, condition and is left as-is.
        auto unary = [&](const char* nm, double (*f)(double), bool (*ok)(double) = nullptr) {
            m.fn(nm, {{"x", "Number"}}, "Float", [f, ok, nm](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                if (a.size() != 1) throw KiritoError("math function expected 1 argument");
                double x = mathNum(vm, a[0]);
                if (ok && !std::isnan(x) && !ok(x))
                    throw KiritoError(std::string(nm) + ": math domain error (got " + floatToString(x) + ")");
                return Value(vm, f(x));
            });
        };
        unary("sqrt", std::sqrt, [](double x) { return x >= 0.0; });
        unary("cbrt", std::cbrt);
        unary("sin", std::sin);
        unary("cos", std::cos);
        unary("tan", std::tan);
        unary("asin", std::asin, [](double x) { return x >= -1.0 && x <= 1.0; });
        unary("acos", std::acos, [](double x) { return x >= -1.0 && x <= 1.0; });
        unary("atan", std::atan);
        unary("sinh", std::sinh);
        unary("cosh", std::cosh);
        unary("tanh", std::tanh);
        unary("asinh", std::asinh);
        unary("acosh", std::acosh, [](double x) { return x >= 1.0; });
        unary("atanh", std::atanh, [](double x) { return x > -1.0 && x < 1.0; });
        unary("exp", std::exp);
        unary("expm1", std::expm1);
        unary("log1p", std::log1p, [](double x) { return x > -1.0; });
        unary("log2", std::log2, [](double x) { return x > 0.0; });
        unary("log10", std::log10, [](double x) { return x > 0.0; });
        unary("trunc", std::trunc);
        unary("gamma", std::tgamma, [](double x) { return !(x <= 0.0 && x == std::floor(x)); });   // poles at 0, -1, -2, ...
        unary("lgamma", std::lgamma, [](double x) { return !(x <= 0.0 && x == std::floor(x)); });
        unary("erf", std::erf);
        unary("erfc", std::erfc);

        m.fn("isnan", {{"x", "Number"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, std::isnan(mathNum(vm, a[0])));
        });
        m.fn("isinf", {{"x", "Number"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, std::isinf(mathNum(vm, a[0])));
        });
        m.fn("isfinite", {{"x", "Number"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, std::isfinite(mathNum(vm, a[0])));
        });
        m.fn("copysign", {{"x", "Number"}, {"y", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, std::copysign(mathNum(vm, a[0]), mathNum(vm, a[1])));
        });
        m.fn("fmod", {{"x", "Number"}, {"y", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            double x = mathNum(vm, a[0]), y = mathNum(vm, a[1]);
            // Domain errors THROW rather than returning a silent NaN (the module-wide policy). fmod is a
            // domain error when the divisor is 0 or the dividend is infinite (both yield NaN in libm);
            // NaN inputs pass through, and fmod(finite, inf)=finite is fine (A13-1).
            if (!std::isnan(x) && !std::isnan(y)) {
                if (y == 0.0) throw KiritoError("fmod: math domain error (divisor is zero)");
                if (std::isinf(x)) throw KiritoError("fmod: math domain error (infinite dividend)");
            }
            return Value(vm, std::fmod(x, y));
        });
        m.fn("lcm", {{"a", "Integer"}, {"b", "Integer"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto get = [&](Handle h) {
                const Object& o = vm.arena().deref(h);
                if (o.kind() != ValueKind::Integer) throw KiritoError("lcm expects Integers");
                return static_cast<const IntVal&>(o).value();
            };
            // Work in unsigned magnitudes: std::abs(INT64_MIN) is UB, and the product can overflow.
            auto mag = [](int64_t v) -> uint64_t {
                return v < 0 ? 0ull - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
            };
            uint64_t x = mag(get(a[0])), y = mag(get(a[1]));
            if (x == 0 || y == 0) return Value(vm, 0);
            uint64_t g = x, h2 = y;
            while (h2) { uint64_t t = g % h2; g = h2; h2 = t; }
            uint64_t prod;
            if (__builtin_mul_overflow(x / g, y, &prod) || prod > static_cast<uint64_t>(INT64_MAX))
                throw KiritoError("lcm result too large for Integer");
            return Value(vm, static_cast<int64_t>(prod));
        });

        m.fn("log", {{"x", "Number"}, {"base", "", vm.none()}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            // log(x) is the natural log; log(x, base) changes the base. A None base (the default) is
            // the natural log too. The argument must be > 0 and a given base must be > 0 and != 1.
            double x = mathNum(vm, a[0]);
            if (!std::isnan(x) && x <= 0.0) throw KiritoError("log: math domain error (argument must be > 0)");
            if (a.size() < 2 || vm.arena().deref(a[1]).kind() == ValueKind::None)
                return Value(vm, std::log(x));
            double b = mathNum(vm, a[1]);
            if (!std::isnan(b) && (b <= 0.0 || b == 1.0)) throw KiritoError("log: math domain error (base must be > 0 and != 1)");
            return Value(vm, std::log(x) / std::log(b));
        });
        m.fn("pow", {{"x", "Number"}, {"y", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.size() != 2) throw KiritoError("pow expected 2 arguments");
            double x = mathNum(vm, a[0]), y = mathNum(vm, a[1]);
            if (!std::isnan(x) && !std::isnan(y)) {
                if (x < 0.0 && y != std::floor(y))
                    throw KiritoError("pow: math domain error (a negative base requires an integer exponent)");
                if (x == 0.0 && y < 0.0)
                    throw KiritoError("pow: math domain error (zero to a negative power)");
            }
            return Value(vm, std::pow(x, y));
        });
        m.fn("atan2", {{"y", "Number"}, {"x", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.size() != 2) throw KiritoError("atan2 expected 2 arguments");
            return Value(vm, std::atan2(mathNum(vm, a[0]), mathNum(vm, a[1])));
        });
        m.fn("hypot", {{"x", "Number"}, {"y", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.size() != 2) throw KiritoError("hypot expected 2 arguments");
            return Value(vm, std::hypot(mathNum(vm, a[0]), mathNum(vm, a[1])));
        });
        m.fn("fabs", {{"x", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, std::fabs(mathNum(vm, a[0])));
        });
        m.fn("degrees", {{"x", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, mathNum(vm, a[0]) * 180.0 / std::numbers::pi);
        });
        m.fn("radians", {{"x", "Number"}}, "Float", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, mathNum(vm, a[0]) * std::numbers::pi / 180.0);
        });
        m.fn("floor", {{"x", "Number"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, toInt64Checked(std::floor(mathNum(vm, a[0])), "floor"));
        });
        m.fn("ceil", {{"x", "Number"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return Value(vm, toInt64Checked(std::ceil(mathNum(vm, a[0])), "ceil"));
        });
        m.fn("factorial", {{"n", "Integer"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            const Object& o = vm.arena().deref(a[0]);
            if (o.kind() != ValueKind::Integer) throw KiritoError("factorial expects an Integer");
            int64_t n = static_cast<const IntVal&>(o).value();
            if (n < 0) throw KiritoError("factorial is not defined for negatives");
            int64_t r = 1;
            for (int64_t i = 2; i <= n; ++i)
                if (__builtin_mul_overflow(r, i, &r))  // 21! already exceeds int64
                    throw KiritoError("factorial result too large for Integer");
            return Value(vm, r);
        });
        m.fn("gcd", {{"a", "Integer"}, {"b", "Integer"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            auto get = [&](Handle h) {
                const Object& o = vm.arena().deref(h);
                if (o.kind() != ValueKind::Integer) throw KiritoError("gcd expects Integers");
                return static_cast<const IntVal&>(o).value();
            };
            auto mag = [](int64_t v) -> uint64_t {  // avoid std::abs(INT64_MIN) UB
                return v < 0 ? 0ull - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
            };
            uint64_t x = mag(get(a[0])), y = mag(get(a[1]));
            while (y) { uint64_t t = x % y; x = y; y = t; }
            if (x > static_cast<uint64_t>(INT64_MAX))
                throw KiritoError("gcd result too large for Integer");
            return Value(vm, static_cast<int64_t>(x));
        });
        // prod(iterable[, start]): product of the elements (Integer if all Integer, else Float).
        m.fn("prod", {{"iterable"}, {"start", "", vm.makeInt(1)}}, "Number", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) throw KiritoError("prod expects an iterable");
            // NB: prod materializes a lazy source (it lives in stdlib_math.hpp, included before runtime.hpp
            // where streamIterate is defined). A lazy source is bounded (the range cap) so this is fine.
            auto items = vm.arena().deref(a[0]).iterate(vm);
            if (!items) throw KiritoError("prod expects an iterable");
            bool isFloat = false, intOverflow = false;
            double f = 1.0;
            int64_t n = 1;
            if (a.size() > 1) {
                const Object& s = vm.arena().deref(a[1]);
                if (s.kind() == ValueKind::Float) { isFloat = true; f = static_cast<const FloatVal&>(s).value(); }
                else if (s.kind() == ValueKind::Integer) { n = static_cast<const IntVal&>(s).value(); f = static_cast<double>(n); }
                else throw KiritoError("prod start must be a number");
            }
            for (Handle h : items.value()) {
                const Object& o = vm.arena().deref(h);
                if (o.kind() == ValueKind::Float) { isFloat = true; f *= static_cast<const FloatVal&>(o).value(); }
                else if (o.kind() == ValueKind::Integer) {
                    int64_t v = static_cast<const IntVal&>(o).value();
                    // A zero factor makes the exact result 0 (trivially an Integer), so it clears any
                    // earlier overflow — otherwise prod([2**62, 4, 0]) throws though its answer is 0.
                    if (v == 0) { n = 0; intOverflow = false; }
                    else if (__builtin_mul_overflow(n, v, &n)) intOverflow = true;  // track; only an error if the result stays Integer
                    f *= static_cast<double>(v);
                }
                else throw KiritoError("prod expects numbers");
            }
            // Like its siblings (factorial/comb/perm/lcm), an Integer prod throws on overflow rather
            // than silently wrapping. A Float in the mix makes the result Float (no overflow concept).
            if (!isFloat && intOverflow) throw KiritoError("prod result too large for Integer");
            return isFloat ? Value(vm, f) : Value(vm, n);
        });
        // comb(n, k) / perm(n, k): combinations / partial permutations, computed without overflow
        // for the common small cases (throws if the exact result exceeds int64).
        auto combPerm = [](KiritoVM& vm, std::span<const Handle> a, bool comb) -> Handle {
            auto geti = [&](Handle h, const char* w) {
                const Object& o = vm.arena().deref(h);
                if (o.kind() != ValueKind::Integer) throw KiritoError(std::string(w) + " expects Integers");
                return static_cast<const IntVal&>(o).value();
            };
            int64_t n = geti(a[0], comb ? "comb" : "perm");
            // perm(n) (k omitted / None) means perm(n, n) == n!; comb always needs both.
            bool haveK = a.size() > 1 && vm.arena().deref(a[1]).kind() != ValueKind::None;
            int64_t k = haveK ? geti(a[1], comb ? "comb" : "perm") : n;
            if (n < 0 || k < 0) throw KiritoError("comb/perm require non-negative Integers");
            if (k > n) return Value(vm, 0);
            if (comb && k > n - k) k = n - k;  // symmetry: fewer multiplications
            if (!comb) {
                // perm: n * (n-1) * ... * (n-k+1), with overflow checking.
                unsigned long long r = 1;
                for (int64_t i = 0; i < k; ++i)
                    if (__builtin_mul_overflow(r, static_cast<unsigned long long>(n - i), &r))
                        throw KiritoError("comb/perm result too large for Integer");
                if (r > static_cast<unsigned long long>(INT64_MAX))
                    throw KiritoError("comb/perm result too large for Integer");
                return Value(vm, static_cast<int64_t>(r));
            }
            // comb: multiply then divide step-by-step so the running value stays the EXACT partial
            // binomial coefficient (always an integer) — avoids overflowing on the full numerator
            // for results that themselves fit in int64 (e.g. comb(30, 15) = 155117520).
            // The running value r stays the EXACT partial binomial C(n-k+i, i), which is monotone up to
            // the final C(n,k) — so if the result fits int64, every partial does too. Only the
            // INTERMEDIATE r*(n-k+i) (before dividing by i) can exceed u64 near the central coefficient
            // (e.g. comb(64,32) whose result 1.83e18 < INT64_MAX). Widen just that product to 128-bit so
            // a representable result isn't falsely rejected as "too large".
            unsigned long long r = 1;
            for (int64_t i = 1; i <= k; ++i) {
                __extension__ unsigned __int128 t = static_cast<unsigned __int128>(r) * static_cast<unsigned long long>(n - k + i);
                t /= static_cast<unsigned long long>(i);  // exact: the partial is an integer
                if (t > static_cast<unsigned long long>(INT64_MAX))
                    throw KiritoError("comb/perm result too large for Integer");
                r = static_cast<unsigned long long>(t);
            }
            return Value(vm, static_cast<int64_t>(r));
        };
        m.fn("comb", {{"n", "Integer"}, {"k", "Integer"}}, "Integer", [combPerm](KiritoVM& vm, std::span<const Handle> a) { return combPerm(vm, a, true); });
        m.fn("perm", {{"n", "Integer"}, {"k", "", vm.none()}}, "Integer", [combPerm](KiritoVM& vm, std::span<const Handle> a) { return combPerm(vm, a, false); });  // k optional: perm(n)=n!
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
