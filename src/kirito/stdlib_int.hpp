#ifndef KIRITO_STDLIB_INT_HPP
#define KIRITO_STDLIB_INT_HPP

// The `int` module: arbitrary-precision integers (a `BigInt` value type) plus the integer-meaningful
// math functions (gcd/lcm/factorial/comb/perm/isqrt/abs/pow/modpow/modinv) and primality
// (deterministic trial division + probabilistic Miller-Rabin) — the exact/unbounded analogues of the
// int64 `math` builtins, carried in their own module the way `complex` carries its analytic set.
//
// BigInt is pure C++ (no GMP): a sign + a little-endian base-2^32 magnitude, with schoolbook add/sub/
// mul, long division (floor semantics matching native Integer //, %), fast exponentiation, and modpow.
// It follows Kirito's reflected-operator rule — arithmetic dispatches on the LEFT operand only, so
// `BigInt(2)+3` works while `3+BigInt(2)` throws; only `==`/`!=` are symmetric — and its true division
// `/` yields a Float (the language-wide "/ is true division" rule), lossy beyond double range exactly
// as Integer/Integer already is.

#include <climits>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "builtins.hpp"
#include "collections.hpp"
#include "native.hpp"
#include "rand_compat.hpp"

namespace kirito {

namespace bigint {

// ~4.2M limbs => ~40M decimal digits: a memory guard so a runaway mul/pow/factorial/parse throws
// instead of OOMing (mirrors kMaxRepeat's role for strings/lists).
constexpr std::size_t kMaxLimbs = std::size_t(1) << 20;
inline void checkSize(std::size_t limbs) {
    if (limbs > kMaxLimbs) throw KiritoError("int: number too large (exceeds size limit)");
}

// A signed big integer: little-endian base-2^32 magnitude with no trailing zero limbs; canonical zero
// is an empty magnitude with neg == false.
struct Big {
    bool neg = false;
    std::vector<uint32_t> mag;
    bool isZero() const { return mag.empty(); }
    void trim() {
        while (!mag.empty() && mag.back() == 0) mag.pop_back();
        if (mag.empty()) neg = false;
    }
};

// ---- unsigned magnitude primitives ----
inline int cmpMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
inline std::vector<uint32_t> addMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> r;
    std::size_t n = a.size() > b.size() ? a.size() : b.size();
    r.reserve(n + 1);
    uint64_t carry = 0;
    for (std::size_t i = 0; i < n; ++i) {
        uint64_t s = carry;
        if (i < a.size()) s += a[i];
        if (i < b.size()) s += b[i];
        r.push_back(static_cast<uint32_t>(s));
        carry = s >> 32;
    }
    if (carry) r.push_back(static_cast<uint32_t>(carry));
    return r;
}
// a - b, requires a >= b (magnitudes).
inline std::vector<uint32_t> subMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> r;
    r.reserve(a.size());
    int64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        int64_t s = static_cast<int64_t>(a[i]) - borrow - (i < b.size() ? static_cast<int64_t>(b[i]) : 0);
        if (s < 0) { s += (int64_t(1) << 32); borrow = 1; } else borrow = 0;
        r.push_back(static_cast<uint32_t>(s));
    }
    while (!r.empty() && r.back() == 0) r.pop_back();
    return r;
}
inline std::vector<uint32_t> mulMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.empty() || b.empty()) return {};
    checkSize(a.size() + b.size());
    std::vector<uint32_t> r(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
            uint64_t cur = static_cast<uint64_t>(r[i + j]) + static_cast<uint64_t>(a[i]) * b[j] + carry;
            r[i + j] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        std::size_t k = i + b.size();
        while (carry) {
            uint64_t cur = static_cast<uint64_t>(r[k]) + carry;
            r[k] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
            ++k;
        }
    }
    while (!r.empty() && r.back() == 0) r.pop_back();
    return r;
}
inline std::vector<uint32_t> mulSmall(const std::vector<uint32_t>& a, uint32_t m) {
    if (a.empty() || m == 0) return {};
    std::vector<uint32_t> r(a.size(), 0);
    uint64_t carry = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        uint64_t cur = static_cast<uint64_t>(a[i]) * m + carry;
        r[i] = static_cast<uint32_t>(cur);
        carry = cur >> 32;
    }
    if (carry) r.push_back(static_cast<uint32_t>(carry));
    return r;
}
inline std::vector<uint32_t> addSmall(std::vector<uint32_t> a, uint32_t v) {
    uint64_t carry = v;
    for (std::size_t i = 0; i < a.size() && carry; ++i) {
        uint64_t cur = static_cast<uint64_t>(a[i]) + carry;
        a[i] = static_cast<uint32_t>(cur);
        carry = cur >> 32;
    }
    if (carry) a.push_back(static_cast<uint32_t>(carry));
    return a;
}
inline void shiftLeft1(std::vector<uint32_t>& v) {
    uint32_t carry = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        uint32_t nc = v[i] >> 31;
        v[i] = static_cast<uint32_t>((v[i] << 1) | carry);
        carry = nc;
    }
    if (carry) v.push_back(carry);
}
inline std::vector<uint32_t> shiftRight1(std::vector<uint32_t> v) {
    uint32_t carry = 0;
    for (std::size_t i = v.size(); i-- > 0;) {
        uint32_t nc = v[i] & 1u;
        v[i] = static_cast<uint32_t>((v[i] >> 1) | (carry << 31));
        carry = nc;
    }
    while (!v.empty() && v.back() == 0) v.pop_back();
    return v;
}
// magnitude divmod by a single 32-bit limb (fast path for base conversion). Returns {quotient, rem}.
inline std::pair<std::vector<uint32_t>, uint32_t> divmodSmall(const std::vector<uint32_t>& a, uint32_t d) {
    std::vector<uint32_t> q(a.size(), 0);
    uint64_t rem = 0;
    for (std::size_t i = a.size(); i-- > 0;) {
        uint64_t cur = (rem << 32) | a[i];
        q[i] = static_cast<uint32_t>(cur / d);
        rem = cur % d;
    }
    while (!q.empty() && q.back() == 0) q.pop_back();
    return {q, static_cast<uint32_t>(rem)};
}
// general magnitude divmod (bit-by-bit long division). b != 0. Returns {quotient, remainder}.
inline std::pair<std::vector<uint32_t>, std::vector<uint32_t>>
divmodMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (cmpMag(a, b) < 0) return {{}, a};
    std::vector<uint32_t> q(a.size(), 0), r;
    std::size_t bits = a.size() * 32;
    for (std::size_t bi = bits; bi-- > 0;) {
        shiftLeft1(r);
        if ((a[bi >> 5] >> (bi & 31)) & 1u) {
            if (r.empty()) r.push_back(1);
            else r[0] |= 1u;
        }
        if (cmpMag(r, b) >= 0) {
            r = subMag(r, b);
            q[bi >> 5] |= (1u << (bi & 31));
        }
    }
    while (!q.empty() && q.back() == 0) q.pop_back();
    return {q, r};
}

// ---- signed operations ----
inline Big fromInt64(int64_t v) {
    Big r;
    uint64_t u = v < 0 ? (0ULL - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v);
    r.neg = v < 0;
    while (u) { r.mag.push_back(static_cast<uint32_t>(u)); u >>= 32; }
    r.trim();
    return r;
}
inline Big fromU64(uint64_t u) {
    Big r;
    while (u) { r.mag.push_back(static_cast<uint32_t>(u)); u >>= 32; }
    return r;
}
inline int cmp(const Big& x, const Big& y) {
    if (x.neg != y.neg) return x.neg ? -1 : 1;
    int c = cmpMag(x.mag, y.mag);
    return x.neg ? -c : c;
}
inline Big negate(Big x) { if (!x.isZero()) x.neg = !x.neg; return x; }
inline Big add(const Big& x, const Big& y) {
    Big r;
    if (x.neg == y.neg) { r.mag = addMag(x.mag, y.mag); r.neg = x.neg; }
    else {
        int c = cmpMag(x.mag, y.mag);
        if (c == 0) return Big{};
        if (c > 0) { r.mag = subMag(x.mag, y.mag); r.neg = x.neg; }
        else { r.mag = subMag(y.mag, x.mag); r.neg = y.neg; }
    }
    r.trim();
    return r;
}
inline Big sub(const Big& x, const Big& y) { return add(x, negate(y)); }
inline Big mul(const Big& x, const Big& y) {
    Big r; r.mag = mulMag(x.mag, y.mag); r.neg = (x.neg != y.neg); r.trim(); return r;
}
inline Big shr1(Big n) { n.mag = shiftRight1(n.mag); n.trim(); return n; }

// FLOOR division: q = floor(x/y), r = x - q*y with sign(r) == sign(y) (or r == 0). Matches native
// Integer //, % (runtime.hpp ifloordiv/imod): truncate, then adjust when r and y disagree in sign.
inline std::pair<Big, Big> divmodFloor(const Big& x, const Big& y) {
    auto qr = divmodMag(x.mag, y.mag);
    Big q; q.mag = qr.first; q.neg = (x.neg != y.neg); q.trim();
    Big r; r.mag = qr.second; r.neg = x.neg; r.trim();
    if (!r.isZero() && (r.neg != y.neg)) {
        q = sub(q, fromInt64(1));
        r = add(r, y);
    }
    return {q, r};
}

inline std::size_t bitLength(const Big& n) {
    if (n.mag.empty()) return 0;
    std::size_t top = n.mag.size() - 1;
    uint32_t hi = n.mag[top];
    std::size_t bits = top * 32;
    while (hi) { ++bits; hi >>= 1; }
    return bits;
}
inline bool toInt64(const Big& n, int64_t& out) {
    if (n.mag.size() > 2) return false;
    uint64_t u = 0;
    if (n.mag.size() >= 1) u = n.mag[0];
    if (n.mag.size() >= 2) u |= static_cast<uint64_t>(n.mag[1]) << 32;
    if (n.neg) {
        if (u > static_cast<uint64_t>(INT64_MAX) + 1) return false;
        out = static_cast<int64_t>(0ULL - u);
        return true;
    }
    if (u > static_cast<uint64_t>(INT64_MAX)) return false;
    out = static_cast<int64_t>(u);
    return true;
}
inline bool toUint64(const Big& n, uint64_t& out) {
    if (n.neg || n.mag.size() > 2) return false;
    uint64_t u = 0;
    if (n.mag.size() >= 1) u = n.mag[0];
    if (n.mag.size() >= 2) u |= static_cast<uint64_t>(n.mag[1]) << 32;
    out = u;
    return true;
}
inline double toDouble(const Big& n) {
    double d = 0.0;
    for (std::size_t i = n.mag.size(); i-- > 0;) d = d * 4294967296.0 + n.mag[i];
    return n.neg ? -d : d;
}

inline std::string toString(const Big& n, int base = 10) {
    if (n.isZero()) return "0";
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::vector<uint32_t> m = n.mag;
    std::string s;
    while (!m.empty()) {
        auto qr = divmodSmall(m, static_cast<uint32_t>(base));
        s.push_back(digits[qr.second]);
        m = qr.first;
    }
    if (n.neg) s.push_back('-');
    for (std::size_t i = 0, j = s.size() - 1; i < j; ++i, --j) std::swap(s[i], s[j]);
    return s;
}
inline int digitVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}
// Parse a signed integer literal. base == 0 auto-detects a 0x/0o/0b prefix (default 10); base 2..36
// parses in that base with no prefix. Surrounding whitespace is allowed; any other trailing text throws.
inline Big parseBig(const std::string& s, int base) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); ++i; }
    int b = base;
    if (base == 0) {
        b = 10;
        if (i + 1 < s.size() && s[i] == '0') {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + 1])));
            if (c == 'x') { b = 16; i += 2; }
            else if (c == 'o') { b = 8; i += 2; }
            else if (c == 'b') { b = 2; i += 2; }
        }
    }
    if (b < 2 || b > 36) throw KiritoError("int: base must be between 2 and 36");
    Big r;
    bool any = false;
    while (i < s.size()) {
        int d = digitVal(s[i]);
        if (d < 0 || d >= b) break;
        r.mag = mulSmall(r.mag, static_cast<uint32_t>(b));
        r.mag = addSmall(r.mag, static_cast<uint32_t>(d));
        r.trim();
        checkSize(r.mag.size());
        any = true;
        ++i;
    }
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (!any || i != s.size()) throw KiritoError("int: invalid integer literal '" + s + "'");
    r.neg = neg && !r.isZero();
    return r;
}

inline Big powU64(Big base, uint64_t e) {
    // Early size guard: the result has ~ e * bitLength(base) bits. Throw up front rather than let the
    // square-and-multiply loop grind through O(n^2) squarings of ever-larger operands before the
    // per-mul checkSize finally trips (base in {0, ±1} stays bounded and is exempt).
    std::size_t bb = bitLength(base);
    if (bb >= 1 && !(base.mag.size() == 1 && base.mag[0] == 1) &&
        e > (static_cast<uint64_t>(kMaxLimbs) * 32) / bb + 1)
        throw KiritoError("int: pow result too large (exceeds size limit)");
    Big result = fromInt64(1);
    while (e) {
        if (e & 1) result = mul(result, base);
        e >>= 1;
        if (e) base = mul(base, base);
    }
    return result;
}
inline Big modpow(const Big& base, const Big& exp, const Big& mod) {
    if (mod.isZero()) throw KiritoError("modpow: modulus is zero");
    if (exp.neg) throw KiritoError("modpow: negative exponent");
    Big result = divmodFloor(fromInt64(1), mod).second;   // 1 % mod (0 when |mod| == 1)
    Big b = divmodFloor(base, mod).second;
    Big e = exp;
    while (!e.isZero()) {
        if (e.mag[0] & 1) result = divmodFloor(mul(result, b), mod).second;
        e = shr1(e);
        if (!e.isZero()) b = divmodFloor(mul(b, b), mod).second;
    }
    return result;
}
inline Big gcd(Big a, Big b) {
    a.neg = false; b.neg = false;
    while (!b.isZero()) {
        Big r = divmodFloor(a, b).second;
        a = b; b = r;
    }
    return a;
}
inline Big isqrt(const Big& n) {
    if (n.neg) throw KiritoError("isqrt: negative operand");
    if (n.isZero()) return Big{};
    Big two = fromInt64(2), one = fromInt64(1);
    Big x = powU64(two, (bitLength(n) + 2) / 2);   // an overestimate of sqrt(n)
    while (true) {
        Big y = divmodFloor(add(x, divmodFloor(n, x).first), two).first;   // (x + n/x) / 2
        if (cmp(y, x) >= 0) break;
        x = y;
    }
    while (cmp(mul(x, x), n) > 0) x = sub(x, one);
    return x;
}
// modular inverse of a mod m (m >= 2), via the extended Euclidean algorithm. Throws if not coprime.
inline Big modinv(const Big& a, const Big& m) {
    Big two = fromInt64(2), one = fromInt64(1);
    if (m.neg || cmp(m, two) < 0) throw KiritoError("modinv: modulus must be >= 2");
    Big oldR = divmodFloor(a, m).second, r = m;
    Big oldS = one, s = Big{};
    while (!r.isZero()) {
        Big q = divmodFloor(oldR, r).first;
        Big t = sub(oldR, mul(q, r)); oldR = r; r = t;
        t = sub(oldS, mul(q, s)); oldS = s; s = t;
    }
    if (cmp(oldR, one) != 0) throw KiritoError("modinv: arguments are not coprime (no inverse exists)");
    return divmodFloor(oldS, m).second;   // normalize into [0, m)
}
inline Big factorial(int64_t n) {
    if (n < 0) throw KiritoError("factorial: not defined for negatives");
    Big r = fromInt64(1);
    for (int64_t i = 2; i <= n; ++i) { r = mul(r, fromInt64(i)); checkSize(r.mag.size()); }
    return r;
}
inline Big perm(int64_t n, int64_t k) {
    if (n < 0 || k < 0) throw KiritoError("perm: requires non-negative integers");
    if (k > n) return Big{};
    Big r = fromInt64(1);
    for (int64_t i = 0; i < k; ++i) { r = mul(r, fromInt64(n - i)); checkSize(r.mag.size()); }
    return r;
}
inline Big comb(int64_t n, int64_t k) {
    if (n < 0 || k < 0) throw KiritoError("comb: requires non-negative integers");
    if (k > n) return Big{};
    if (k > n - k) k = n - k;
    Big r = fromInt64(1);
    for (int64_t i = 0; i < k; ++i) {
        r = mul(r, fromInt64(n - i));
        r = divmodFloor(r, fromInt64(i + 1)).first;   // exact: running value is a partial binomial
        checkSize(r.mag.size());
    }
    return r;
}

// ---- randomness (OS CSPRNG) for primality ----
// Throws if the OS entropy source is unavailable rather than proceeding with an unfilled buffer —
// a predictable "random" prime or a fixed Miller-Rabin base would silently defeat both callers, so
// they fail loudly instead. (The deterministic is_prime needs no randomness and still works.)
inline Big randomBits(int bits) {
    if (bits <= 0) return Big{};
    std::size_t limbs = (static_cast<std::size_t>(bits) + 31) / 32;
    Big n; n.mag.assign(limbs, 0);
    if (!randcompat::fillRandom(n.mag.data(), limbs * 4))
        throw KiritoError("int: OS secure random source unavailable (needed for primality/random_prime)");
    int top = bits & 31;
    if (top != 0) n.mag[limbs - 1] &= ((1u << top) - 1);
    n.trim();
    return n;
}
inline Big randomBelow(const Big& n) {   // uniform [0, n), rejection sampling
    std::size_t bits = bitLength(n);
    while (true) {
        Big r = randomBits(static_cast<int>(bits));
        if (cmp(r, n) < 0) return r;
    }
}
inline Big randomInRange(const Big& lo, const Big& hi) {   // [lo, hi]
    return add(lo, randomBelow(add(sub(hi, lo), fromInt64(1))));
}

inline bool isPrimeU64(uint64_t n) {
    if (n < 2) return false;
    if (n < 4) return true;              // 2, 3
    if ((n & 1) == 0) return false;
    if (n % 3 == 0) return false;
    for (uint64_t i = 5; i <= n / i; i += 6)   // i <= n/i avoids i*i overflow
        if (n % i == 0 || n % (i + 2) == 0) return false;
    return true;
}
// Deterministic primality: the naive O(sqrt n) trial division. A tight native uint64 loop when the
// value fits int64 (the common, optimal case); BigInt-arithmetic trial division as a correct — if
// slow — fallback for larger values.
inline bool isPrimeExact(const Big& n) {
    int64_t v;
    if (toInt64(n, v)) return v >= 0 && isPrimeU64(static_cast<uint64_t>(v));
    if (n.neg) return false;
    if ((n.mag[0] & 1) == 0) return false;    // n > 2^63 and even
    Big two = fromInt64(2), i = fromInt64(3), limit = isqrt(n);
    while (cmp(i, limit) <= 0) {
        if (divmodFloor(n, i).second.isZero()) return false;
        i = add(i, two);
    }
    return true;
}
// Probabilistic primality: Miller-Rabin with `rounds` random bases from the OS CSPRNG.
inline bool isProbablePrime(const Big& n, int rounds) {
    if (n.neg) return false;
    Big one = fromInt64(1), two = fromInt64(2), three = fromInt64(3);
    if (cmp(n, two) < 0) return false;
    if (cmp(n, three) <= 0) return true;
    if ((n.mag[0] & 1) == 0) return false;
    static const uint32_t small[] = {3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (uint32_t p : small) {
        Big bp = fromInt64(p);
        if (cmp(n, bp) == 0) return true;
        if (divmodFloor(n, bp).second.isZero()) return false;
    }
    Big nm1 = sub(n, one), nm2 = sub(n, two), d = nm1;
    int s = 0;
    while ((d.mag[0] & 1) == 0) { d = shr1(d); ++s; }
    for (int i = 0; i < rounds; ++i) {
        Big a = randomInRange(two, nm2);
        Big x = modpow(a, d, n);
        if (cmp(x, one) == 0 || cmp(x, nm1) == 0) continue;
        bool composite = true;
        for (int r = 1; r < s; ++r) {
            x = modpow(x, two, n);
            if (cmp(x, nm1) == 0) { composite = false; break; }
        }
        if (composite) return false;
    }
    return true;
}
inline Big randomPrime(int bits, int rounds) {
    if (bits < 2) throw KiritoError("random_prime: bits must be >= 2");
    while (true) {
        Big n = randomBits(bits);
        n.mag.resize((static_cast<std::size_t>(bits) + 31) / 32, 0);
        n.mag[(static_cast<std::size_t>(bits) - 1) >> 5] |= (1u << ((bits - 1) & 31));   // top bit -> exact bit length
        n.mag[0] |= 1u;                                                                  // odd
        n.trim();
        if (isProbablePrime(n, rounds)) return n;
    }
}

}  // namespace bigint

// ------------------------------------------------------------------- the BigInt value
class BigIntVal : public NativeClass<BigIntVal> {
public:
    static constexpr const char* kTypeName = "BigInt";
    bigint::Big val;

    BigIntVal() = default;
    explicit BigIntVal(bigint::Big v) : val(std::move(v)) {}

    bool truthy() const override { return !val.isZero(); }
    std::string str(StringifyCtx&) const override { return bigint::toString(val, 10); }

    bool hashable() const override { return true; }
    std::size_t hash() const override {
        int64_t v;
        if (bigint::toInt64(val, v)) return std::hash<int64_t>{}(v);   // agree with an equal Integer
        std::size_t h = val.neg ? 1u : 0u;
        for (uint32_t limb : val.mag) h = h * 1000003u + limb;
        return h;
    }
    bool equals(const ObjectArena&, const Object& other) const override {
        if (const auto* b = dynamic_cast<const BigIntVal*>(&other)) return bigint::cmp(val, b->val) == 0;
        if (other.kind() == ValueKind::Integer)
            return bigint::cmp(val, bigint::fromInt64(static_cast<const IntVal&>(other).value())) == 0;
        if (other.kind() == ValueKind::Bool)
            return bigint::cmp(val, bigint::fromInt64(static_cast<const BoolVal&>(other).value() ? 1 : 0)) == 0;
        return false;
    }

    std::vector<std::string> inspectMembers() const override {
        return {"modpow(exponent, modulus) -> BigInt", "is_prime() -> Bool",
                "is_probable_prime(rounds = 25) -> Bool", "bit_length() -> Integer", "to_int() -> Integer"};
    }

    Handle binary(KiritoVM& vm, BinOp op, Handle self, Handle rhs) override;
    Handle unary(KiritoVM& vm, UnOp op, Handle self) override;
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override;
};

namespace bigint {

inline Handle make(KiritoVM& vm, Big v) { return vm.alloc(std::make_unique<BigIntVal>(std::move(v))); }

// Coerce a BigInt or a native Integer/Bool to a Big (numbers are exact; a Float/other throws).
inline Big coerce(KiritoVM& vm, Handle h, const char* who) {
    const Object& o = vm.arena().deref(h);
    if (const auto* b = dynamic_cast<const BigIntVal*>(&o)) return b->val;
    if (o.kind() == ValueKind::Integer) return fromInt64(static_cast<const IntVal&>(o).value());
    if (o.kind() == ValueKind::Bool) return fromInt64(static_cast<const BoolVal&>(o).value() ? 1 : 0);
    throw KiritoError(std::string(who) + " expects a BigInt or Integer");
}
inline int64_t coerceInt(KiritoVM& vm, Handle h, const char* who) {
    Big b = coerce(vm, h, who);
    int64_t v;
    if (!toInt64(b, v)) throw KiritoError(std::string(who) + " is too large to use here");
    return v;
}
inline Handle powOp(KiritoVM& vm, const Big& base, const Big& exp) {
    if (exp.neg) return vm.makeFloat(std::pow(toDouble(base), toDouble(exp)));   // Float, like Integer**negInt
    uint64_t e;
    if (!toUint64(exp, e)) {
        if (base.isZero()) return make(vm, Big{});
        if (base.mag.size() == 1 && base.mag[0] == 1) {
            if (!base.neg) return make(vm, fromInt64(1));
            return make(vm, fromInt64((exp.mag.empty() || !(exp.mag[0] & 1)) ? 1 : -1));   // (-1)**e by parity
        }
        throw KiritoError("pow: exponent too large");
    }
    return make(vm, powU64(base, e));
}

}  // namespace bigint

// The native-binding idiom: bound-method lambdas take vm/self params that intentionally shadow the
// enclosing scope's (same VM, by design). Silence -Wshadow here as the other stdlib glue does.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

inline Handle BigIntVal::binary(KiritoVM& vm, BinOp op, Handle, Handle rhs) {
    using namespace bigint;
    Big b = coerce(vm, rhs, "BigInt arithmetic");
    switch (op) {
        case BinOp::Add: return make(vm, add(val, b));
        case BinOp::Sub: return make(vm, sub(val, b));
        case BinOp::Mul: return make(vm, mul(val, b));
        case BinOp::FloorDiv:
            if (b.isZero()) throw KiritoError("integer division by zero");
            return make(vm, divmodFloor(val, b).first);
        case BinOp::Mod:
            if (b.isZero()) throw KiritoError("integer modulo by zero");
            return make(vm, divmodFloor(val, b).second);
        case BinOp::Div:                                    // true division -> Float (language-wide rule)
            if (b.isZero()) throw KiritoError("division by zero");
            return vm.makeFloat(toDouble(val) / toDouble(b));
        case BinOp::Pow: return powOp(vm, val, b);
        case BinOp::Eq: return vm.makeBool(cmp(val, b) == 0);
        case BinOp::Ne: return vm.makeBool(cmp(val, b) != 0);
        case BinOp::Lt: return vm.makeBool(cmp(val, b) < 0);
        case BinOp::Le: return vm.makeBool(cmp(val, b) <= 0);
        case BinOp::Gt: return vm.makeBool(cmp(val, b) > 0);
        case BinOp::Ge: return vm.makeBool(cmp(val, b) >= 0);
        default: break;
    }
    throw KiritoError("BigInt does not support this operator");
}

inline Handle BigIntVal::unary(KiritoVM& vm, UnOp op, Handle) {
    if (op == UnOp::Neg) return bigint::make(vm, bigint::negate(val));
    throw KiritoError("BigInt does not support this unary operator");
}

inline Handle BigIntVal::getAttr(KiritoVM& vm, Handle self, std::string_view name) {
    using namespace bigint;
    auto selfVal = [](KiritoVM& vm, Handle self) -> const Big& {
        return static_cast<BigIntVal&>(vm.arena().deref(self)).val;
    };
    if (name == "modpow")
        return makeMethod(vm, "modpow", {"exponent", "modulus"},
            [self, selfVal](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "modpow").require(2);
                return make(vm, modpow(selfVal(vm, self), coerce(vm, a[0], "modpow exponent"),
                                       coerce(vm, a[1], "modpow modulus")));
            }, std::vector<Handle>{self});
    if (name == "is_prime")
        return makeMethod(vm, "is_prime", {},
            [self, selfVal](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeBool(isPrimeExact(selfVal(vm, self)));
            }, std::vector<Handle>{self});
    if (name == "is_probable_prime") {
        std::vector<NativeParam> sig;
        sig.emplace_back("rounds", "Integer", vm.makeInt(25));
        return vm.alloc(std::make_unique<NativeFunction>(
            "is_probable_prime", std::move(sig), "Bool",
            [self, selfVal](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                int64_t rounds = Value(vm, a[0]).asInt("rounds");
                if (rounds < 1) throw KiritoError("is_probable_prime: rounds must be >= 1");
                return vm.makeBool(isProbablePrime(selfVal(vm, self), static_cast<int>(rounds)));
            },
            std::vector<Handle>{self}));
    }
    if (name == "bit_length")
        return makeMethod(vm, "bit_length", {},
            [self, selfVal](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(bitLength(selfVal(vm, self))));
            }, std::vector<Handle>{self});
    if (name == "to_int")
        return makeMethod(vm, "to_int", {},
            [self, selfVal](KiritoVM& vm, std::span<const Handle>) -> Handle {
                int64_t v;
                if (!toInt64(selfVal(vm, self), v)) throw KiritoError("to_int: value does not fit in a native Integer");
                return vm.makeInt(v);
            }, std::vector<Handle>{self});
    // serialization: a BigInt round-trips as its decimal String.
    if (name == "_getstate_")
        return makeMethod(vm, "_getstate_", {},
            [self, selfVal](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeString(toString(selfVal(vm, self), 10));
            }, std::vector<Handle>{self});
    if (name == "_setstate_")
        return makeMethod(vm, "_setstate_", {"state"},
            [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "_setstate_").require(1);
                static_cast<BigIntVal&>(vm.arena().deref(self)).val =
                    parseBig(Value(vm, a[0]).asStringRef("BigInt state"), 10);
                return vm.none();
            }, std::vector<Handle>{self});
    return Object::getAttr(vm, self, name);
}

// ------------------------------------------------------------------- the `int` module
class IntModule : public NativeModule {
public:
    std::string name() const override { return "int"; }

    void setup(ModuleBuilder& m) override {
        using namespace bigint;
        KiritoVM& vm = m.vm();

        vm.registerDeserializer("BigInt", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<BigIntVal>());
        });

        // Constructors / converters.
        auto construct = [](KiritoVM& vm, Handle h) -> Big {
            const Object& o = vm.arena().deref(h);
            if (const auto* b = dynamic_cast<const BigIntVal*>(&o)) return b->val;
            if (o.kind() == ValueKind::Integer) return fromInt64(static_cast<const IntVal&>(o).value());
            if (o.kind() == ValueKind::Bool) return fromInt64(static_cast<const BoolVal&>(o).value() ? 1 : 0);
            if (o.kind() == ValueKind::String) return parseBig(static_cast<const StrVal&>(o).value(), 0);
            throw KiritoError("BigInt expects an Integer, a String, or a BigInt");
        };
        m.fn("BigInt", {{"value"}}, "BigInt", [construct](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return make(vm, construct(vm, Args(vm, a, "BigInt")[0].handle()));
        });
        m.fn("big", {{"value"}}, "BigInt", [construct](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return make(vm, construct(vm, Args(vm, a, "big")[0].handle()));
        });
        m.fn("from_string", {{"s", "String"}, {"base", "Integer", vm.makeInt(10)}}, "BigInt",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "from_string");
            int64_t base = args[1].asInt("from_string base");
            if (base < 2 || base > 36) throw KiritoError("from_string: base must be between 2 and 36");
            return make(vm, parseBig(args[0].asStringRef("from_string s"), static_cast<int>(base)));
        });

        // Integer math (exact/unbounded analogues of the int64 `math` builtins).
        m.fn("gcd", {{"a"}, {"b"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "gcd");
            return make(vm, gcd(coerce(vm, args[0].handle(), "gcd a"), coerce(vm, args[1].handle(), "gcd b")));
        });
        m.fn("lcm", {{"a"}, {"b"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "lcm");
            Big x = coerce(vm, args[0].handle(), "lcm a"), y = coerce(vm, args[1].handle(), "lcm b");
            if (x.isZero() || y.isZero()) return make(vm, Big{});
            Big g = gcd(x, y);
            Big r = mul(divmodFloor(x, g).first, y);
            r.neg = false;   // lcm is non-negative
            return make(vm, r);
        });
        m.fn("factorial", {{"n"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return make(vm, factorial(coerceInt(vm, Args(vm, a, "factorial")[0].handle(), "factorial n")));
        });
        m.fn("comb", {{"n"}, {"k"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "comb");
            return make(vm, comb(coerceInt(vm, args[0].handle(), "comb n"), coerceInt(vm, args[1].handle(), "comb k")));
        });
        m.fn("perm", {{"n"}, {"k"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "perm");
            return make(vm, perm(coerceInt(vm, args[0].handle(), "perm n"), coerceInt(vm, args[1].handle(), "perm k")));
        });
        m.fn("isqrt", {{"n"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return make(vm, isqrt(coerce(vm, Args(vm, a, "isqrt")[0].handle(), "isqrt n")));
        });
        m.fn("abs", {{"n"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Big n = coerce(vm, Args(vm, a, "abs")[0].handle(), "abs n");
            n.neg = false;
            return make(vm, n);
        });
        m.fn("pow", {{"base"}, {"exp"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "pow");
            Big exp = coerce(vm, args[1].handle(), "pow exp");
            if (exp.neg) throw KiritoError("int.pow: negative exponent (use ** for a Float, or modpow for modular)");
            uint64_t e;
            Big base = coerce(vm, args[0].handle(), "pow base");
            if (!toUint64(exp, e)) throw KiritoError("int.pow: exponent too large");
            return make(vm, powU64(base, e));
        });
        m.fn("modpow", {{"base"}, {"exp"}, {"mod"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "modpow");
            return make(vm, modpow(coerce(vm, args[0].handle(), "modpow base"),
                                   coerce(vm, args[1].handle(), "modpow exp"),
                                   coerce(vm, args[2].handle(), "modpow mod")));
        });
        m.fn("modinv", {{"a"}, {"m"}}, "BigInt", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "modinv");
            return make(vm, modinv(coerce(vm, args[0].handle(), "modinv a"), coerce(vm, args[1].handle(), "modinv m")));
        });

        // Primality.
        m.fn("is_prime", {{"n"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            return vm.makeBool(isPrimeExact(coerce(vm, Args(vm, a, "is_prime")[0].handle(), "is_prime n")));
        });
        m.fn("is_probable_prime", {{"n"}, {"rounds", "Integer", vm.makeInt(25)}}, "Bool",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "is_probable_prime");
            int64_t rounds = args[1].asInt("is_probable_prime rounds");
            if (rounds < 1) throw KiritoError("is_probable_prime: rounds must be >= 1");
            return vm.makeBool(isProbablePrime(coerce(vm, args[0].handle(), "is_probable_prime n"),
                                               static_cast<int>(rounds)));
        });
        m.fn("random_prime", {{"bits", "Integer"}, {"rounds", "Integer", vm.makeInt(25)}}, "BigInt",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "random_prime");
            int64_t bits = args[0].asInt("random_prime bits");
            int64_t rounds = args[1].asInt("random_prime rounds");
            if (bits < 2) throw KiritoError("random_prime: bits must be >= 2");
            if (bits > 1 << 16) throw KiritoError("random_prime: bits too large");
            if (rounds < 1) throw KiritoError("random_prime: rounds must be >= 1");
            return make(vm, randomPrime(static_cast<int>(bits), static_cast<int>(rounds)));
        });

        m.value("zero", make(vm, Big{}));
        m.value("one", make(vm, fromInt64(1)));
    }
};

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

}  // namespace kirito

#endif
