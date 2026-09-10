#ifndef KIRITO_STDLIB_INT_HPP
#define KIRITO_STDLIB_INT_HPP

// The `int` module: arbitrary-precision integers (a `BigInt` value type) plus the integer-meaningful
// math functions (gcd/lcm/factorial/comb/perm/isqrt/abs/pow/modpow/modinv) and primality
// (deterministic AKS + probabilistic Miller-Rabin) — the exact/unbounded analogues of the
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
// they fail loudly instead. (The deterministic isprime needs no randomness and still works.)
inline Big randomBits(int bits) {
    if (bits <= 0) return Big{};
    std::size_t limbs = (static_cast<std::size_t>(bits) + 31) / 32;
    Big n; n.mag.assign(limbs, 0);
    if (!randcompat::fillRandom(n.mag.data(), limbs * 4))
        throw KiritoError("int: OS secure random source unavailable (needed for primality/randomprime)");
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

// ---- AKS deterministic primality ----------------------------------------------------------------
// Efficient AKS: deterministic and polynomial-time (replaces the old naive O(sqrt n) trial division).
// The hot path is polynomial arithmetic in (Z/nZ)[x]/(x^r - 1). Coefficients use native u64/u128 when
// n fits in 64 bits (every native Integer, and the whole practical range) and fall back to Big for
// larger n. The AKS ring degree r is O((log n)^5) in the worst case, so an infeasibly-large n would
// allocate an unbounded polynomial: a `maxdegree` guard bounds r and fails fast with a clear
// KiritoError instead of OOMing (a resource backstop like kMaxLimbs, never a silent degradation).
// AKS remains far slower than the probabilistic isprobableprime; it is the deterministic, exact test.

inline constexpr uint64_t kAksDefaultMaxDegree = 1u << 20;  // sane, very high: realistic inputs never reach it

inline uint64_t mulmodU64(uint64_t a, uint64_t b, uint64_t m) {
    return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) % m);
}
inline uint64_t gcdU64(uint64_t a, uint64_t b) { while (b) { uint64_t t = a % b; a = b; b = t; } return a; }

// Multiplicative order of a modulo r (requires gcd(a,r) == 1). Returns the order, UINT64_MAX if it
// exceeds `cap` (the caller only needs to know whether ord > target), or 0 if a is not invertible.
inline uint64_t multiplicativeOrderModR(uint64_t a, uint64_t r, uint64_t cap) {
    a %= r;
    if (a == 0) return 0;
    uint64_t k = 0, cur = 1;
    do {
        cur = mulmodU64(cur, a, r);
        ++k;
        if (k > cap) return UINT64_MAX;
    } while (cur != 1);
    return k;
}

inline uint64_t eulerPhiU64(uint64_t r) {
    uint64_t result = r, m = r;
    for (uint64_t p = 2; p <= m / p; ++p)
        if (m % p == 0) { while (m % p == 0) m /= p; result -= result / p; }
    if (m > 1) result -= result / m;
    return result;
}

// ceil(sqrt(v)); v is bounded by phi(r) <= maxdegree, so the linear loop is short.
inline uint64_t ceilSqrtU64(uint64_t v) {
    if (v <= 1) return v;
    uint64_t x = 1;
    while (x <= v / x && x * x < v) ++x;   // x <= v/x guards x*x overflow
    return x;
}

// n == a^b for some integers a >= 2, b >= 2 ?  (composite-detecting AKS step 1)
inline bool isPerfectPower(const Big& n) {
    std::size_t bits = bitLength(n);
    Big one = fromInt64(1);
    for (std::size_t b = 2; b <= bits; ++b) {
        Big lo = fromInt64(2), hi = powU64(fromInt64(2), static_cast<uint64_t>(bits / b + 1));
        while (cmp(lo, hi) <= 0) {
            Big mid = shr1(add(lo, hi));                      // floor((lo+hi)/2)
            Big p = one; bool over = false;                  // p = mid^b, early-exit once p > n
            for (std::size_t i = 0; i < b; ++i) { p = mul(p, mid); if (cmp(p, n) > 0) { over = true; break; } }
            int c = over ? 1 : cmp(p, n);
            if (c == 0) return true;
            if (c < 0) lo = add(mid, one); else hi = sub(mid, one);
        }
    }
    return false;
}

// n mod r as a native u64 (r fits u64; the remainder is < r so it always fits).
inline uint64_t modU64(const Big& n, uint64_t r) {
    Big rem = divmodFloor(n, fromU64(r)).second;
    uint64_t t = 0;
    toUint64(rem, t);
    return t;
}

// Smallest r with ord_r(n) > (bitLength n)^2 (a safe over-approximation of (log2 n)^2). Throws if r
// would exceed maxdegree. Non-coprime r are skipped; a small factor is caught by the gcd step.
inline uint64_t findAksR(const Big& n, uint64_t maxdegree) {
    uint64_t L = static_cast<uint64_t>(bitLength(n));
    uint64_t target = L * L;
    for (uint64_t r = 2;; ++r) {
        if (r > maxdegree)
            throw KiritoError("isprime: input too large for deterministic AKS (ring degree r exceeds maxdegree)");
        uint64_t nr = modU64(n, r);
        if (nr == 0 || gcdU64(nr, r) != 1) continue;         // r shares a factor with n
        if (multiplicativeOrderModR(nr, r, target) == UINT64_MAX) return r;  // ord_r(n) > target
    }
}

// Modular-arithmetic policies for the polynomial ring coefficients (SSOT: one poly engine, two backends).
struct ModU64 {
    using T = uint64_t;
    uint64_t n;
    T zero() const { return 0; }
    T one() const { return 1 % n; }
    bool isZero(T x) const { return x == 0; }
    bool eq(T x, T y) const { return x == y; }
    T fromU64v(uint64_t v) const { return v % n; }
    T add(T x, T y) const { return static_cast<uint64_t>((static_cast<unsigned __int128>(x) + y) % n); }
    T mul(T x, T y) const { return mulmodU64(x, y, n); }
};
struct ModBig {
    using T = Big;
    Big n;
    T zero() const { return Big{}; }
    T one() const { return divmodFloor(fromInt64(1), n).second; }
    bool isZero(const T& x) const { return x.isZero(); }
    bool eq(const T& x, const T& y) const { return cmp(x, y) == 0; }
    T fromU64v(uint64_t v) const { return divmodFloor(bigint::fromU64(v), n).second; }
    T add(const T& x, const T& y) const { return divmodFloor(bigint::add(x, y), n).second; }
    T mul(const T& x, const T& y) const { return divmodFloor(bigint::mul(x, y), n).second; }
};

// Multiply two length-r polynomials in the cyclotomic ring (indices wrap mod r), coefficients mod n.
template <class Mod>
std::vector<typename Mod::T> aksPolyMul(const Mod& M, const std::vector<typename Mod::T>& x,
                                        const std::vector<typename Mod::T>& y, uint64_t r) {
    using T = typename Mod::T;
    std::vector<T> out(r, M.zero());
    for (uint64_t i = 0; i < r; ++i) {
        if (M.isZero(x[i])) continue;
        for (uint64_t j = 0; j < r; ++j) {
            if (M.isZero(y[j])) continue;
            uint64_t k = i + j; if (k >= r) k -= r;
            out[k] = M.add(out[k], M.mul(x[i], y[j]));
        }
    }
    return out;
}

// The AKS congruence: (x + a)^n == x^(n mod r) + a  in (Z/nZ)[x]/(x^r - 1) ?
template <class Mod>
bool aksCongruenceHolds(const Mod& M, const Big& n, uint64_t r, uint64_t a) {
    using T = typename Mod::T;
    uint64_t nModR = modU64(n, r);
    std::vector<T> base(r, M.zero());
    base[0] = M.fromU64v(a);
    base[1] = M.add(base[1], M.one());                       // + x  (r >= 2 here)
    std::vector<T> result(r, M.zero());
    result[0] = M.one();                                     // result = 1
    std::size_t nb = bitLength(n);
    for (std::size_t bit = nb; bit-- > 0;) {                 // result = base^n, square-and-multiply
        result = aksPolyMul(M, result, result, r);
        if ((n.mag[bit >> 5] >> (bit & 31)) & 1u) result = aksPolyMul(M, result, base, r);
    }
    std::vector<T> rhs(r, M.zero());                         // rhs = x^(n mod r) + a
    rhs[nModR] = M.add(rhs[nModR], M.one());
    rhs[0] = M.add(rhs[0], M.fromU64v(a));
    for (uint64_t i = 0; i < r; ++i) if (!M.eq(result[i], rhs[i])) return false;
    return true;
}

inline bool isPrimeAKS(const Big& n, uint64_t maxdegree = kAksDefaultMaxDegree) {
    if (n.neg) return false;
    int64_t v;
    if (toInt64(n, v) && v < 2) return false;                // 0, 1 (and negatives, already handled)
    Big two = fromInt64(2), three = fromInt64(3);
    if (cmp(n, two) == 0 || cmp(n, three) == 0) return true;
    if ((n.mag[0] & 1) == 0) return false;                   // even and > 2
    if (isPerfectPower(n)) return false;                     // step 1
    uint64_t r = findAksR(n, maxdegree);                     // step 2
    for (uint64_t a = 2; a <= r; ++a) {                      // step 3: 1 < gcd(a, n) < n => composite
        if (cmp(n, fromU64(a)) <= 0) break;                  // a >= n; step 4 handles n <= r
        if (gcdU64(modU64(n, a), a) > 1) return false;
    }
    if (cmp(n, fromU64(r)) <= 0) return true;                // step 4: n <= r => prime
    uint64_t s = ceilSqrtU64(eulerPhiU64(r));                // step 5: check a = 1..floor(sqrt(phi(r))*log2 n)
    uint64_t L = static_cast<uint64_t>(bitLength(n));
    unsigned __int128 lim128 = static_cast<unsigned __int128>(s) * L;
    uint64_t lim = lim128 > static_cast<unsigned __int128>(UINT64_MAX) ? UINT64_MAX : static_cast<uint64_t>(lim128);
    uint64_t nU64 = 0;
    if (toUint64(n, nU64)) {
        ModU64 M{nU64};
        for (uint64_t a = 1; a <= lim; ++a) if (!aksCongruenceHolds(M, n, r, a)) return false;
    } else {
        ModBig M{n};
        for (uint64_t a = 1; a <= lim; ++a) if (!aksCongruenceHolds(M, n, r, a)) return false;
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
    if (bits < 2) throw KiritoError("randomprime: bits must be >= 2");
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
    bool initialized_ = false;  // true once a value is fully built; guards _setstate_ (see below)

    BigIntVal() = default;                                    // empty shell for serialize/dump reconstruction
    explicit BigIntVal(bigint::Big v) : val(std::move(v)) { initialized_ = true; }

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
        // A BigInt equals a Float iff the Float is a finite integer of the same value (F07-2) — without
        // this, `BigInt(3) == 3.0` was False though `3 == 3.0` and `3 == BigInt(3)` are True (equality
        // non-transitive), and since BigInt hashes equal to the integral Float they shared a Set/Dict
        // bucket yet compared unequal (a BigInt key was reachable by Integer but not the equal Float).
        // Exact within the int64 range (an integral double is exact there); a Float outside int64 range
        // is treated as unequal — a genuinely-equal huge power-of-two float is an accepted rare
        // false-negative, and any non-power-of-two huge float is not exactly an integer anyway.
        if (other.kind() == ValueKind::Float) {
            double f = static_cast<const FloatVal&>(other).value();
            if (std::isnan(f) || std::isinf(f) || f != std::trunc(f)) return false;
            if (!doubleFitsInt64(f)) return false;   // shared int64 boundary (builtins.hpp)
            return bigint::cmp(val, bigint::fromInt64(static_cast<int64_t>(f))) == 0;
        }
        return false;
    }

    std::vector<std::string> inspectMembers() const override {
        return {"modpow(exponent, modulus) -> BigInt", "isprime() -> Bool",
                "isprobableprime(rounds = 25) -> Bool", "bitlength() -> Integer", "toint() -> Integer"};
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
// base ** exp for a NON-negative exp (precondition exp >= 0). The single implementation shared by the
// `**` operator (powOp) and the `int.pow` module fn, so both agree on the trivial-base short-circuits
// (0**huge = 0, (±1)**huge by parity) instead of one returning the exact answer while the other throws
// "exponent too large" for the same inputs.
inline Handle powNonNeg(KiritoVM& vm, const Big& base, const Big& exp) {
    uint64_t e;
    if (!toUint64(exp, e)) {   // exponent exceeds uint64: only trivial bases have a representable result
        if (base.isZero()) return make(vm, Big{});                     // 0**n = 0 (n > 0 here)
        if (base.mag.size() == 1 && base.mag[0] == 1)                  // (±1)**e: 1, or -1 for odd e
            return make(vm, fromInt64((!base.neg || exp.mag.empty() || !(exp.mag[0] & 1)) ? 1 : -1));
        throw KiritoError("pow: exponent too large");
    }
    return make(vm, powU64(base, e));
}
inline Handle powOp(KiritoVM& vm, const Big& base, const Big& exp) {
    if (exp.neg) {
        // Mirror native Integer**negInt / Float**neg exactly (runtime.hpp): 0**-n is undefined, throw
        // the same message rather than letting std::pow return a silent inf (A08-1).
        if (base.isZero()) throw KiritoError("zero cannot be raised to a negative power");
        return vm.makeFloat(std::pow(toDouble(base), toDouble(exp)));   // Float, like Integer**negInt
    }
    return powNonNeg(vm, base, exp);
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
    if (name == "isprime") {
        RootScope rs(vm);
        std::vector<NativeParam> sig;
        sig.emplace_back("maxdegree", "Integer",
                         rs.add(vm.makeInt(static_cast<int64_t>(kAksDefaultMaxDegree))));
        return vm.alloc(std::make_unique<NativeFunction>(
            "isprime", std::move(sig), "Bool",
            [self, selfVal](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                int64_t md = Value(vm, a[0]).asInt("isprime maxdegree");
                if (md < 2) throw KiritoError("isprime: maxdegree must be >= 2");
                return vm.makeBool(isPrimeAKS(selfVal(vm, self), static_cast<uint64_t>(md)));
            },
            std::vector<Handle>{self}));
    }
    if (name == "isprobableprime") {
        RootScope rs(vm);
        std::vector<NativeParam> sig;
        sig.emplace_back("rounds", "Integer", rs.add(vm.makeInt(25)));  // interned today; don't rely on it
        return vm.alloc(std::make_unique<NativeFunction>(
            "isprobableprime", std::move(sig), "Bool",
            [self, selfVal](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                int64_t rounds = Value(vm, a[0]).asInt("rounds");
                if (rounds < 1) throw KiritoError("isprobableprime: rounds must be >= 1");
                return vm.makeBool(isProbablePrime(selfVal(vm, self), static_cast<int>(rounds)));
            },
            std::vector<Handle>{self}));
    }
    if (name == "bitlength")
        return makeMethod(vm, "bitlength", {},
            [self, selfVal](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(bitLength(selfVal(vm, self))));
            }, std::vector<Handle>{self});
    if (name == "toint")
        return makeMethod(vm, "toint", {},
            [self, selfVal](KiritoVM& vm, std::span<const Handle>) -> Handle {
                int64_t v;
                if (!toInt64(selfVal(vm, self), v)) throw KiritoError("toint: value does not fit in a native Integer");
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
                auto& b = static_cast<BigIntVal&>(vm.arena().deref(self));
                // BigInt is immutable + hashable (a Dict/Set key). _setstate_ is the deserializer's
                // alloc(empty) -> _setstate_ path only; re-homing an established value in place would
                // change its hash inside a live bucket and corrupt any container keyed on it. One-shot
                // (mirrors DateTime/Bytes._setstate_).
                if (b.initialized_)
                    throw KiritoError("BigInt _setstate_: cannot re-initialize an established BigInt "
                                      "(it is immutable once built)");
                b.val = parseBig(Value(vm, a[0]).asStringRef("BigInt state"), 10);
                b.initialized_ = true;
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
        m.fn("fromstring", {{"s", "String"}, {"base", "Integer", vm.makeInt(10)}}, "BigInt",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "fromstring");
            int64_t base = args[1].asInt("fromstring base");
            if (base < 2 || base > 36) throw KiritoError("fromstring: base must be between 2 and 36");
            return make(vm, parseBig(args[0].asStringRef("fromstring s"), static_cast<int>(base)));
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
            Big base = coerce(vm, args[0].handle(), "pow base");
            // Same power engine as `**` (powNonNeg): 0**huge / (±1)**huge return the exact result rather
            // than throwing, so int.pow and the operator can never disagree.
            return powNonNeg(vm, base, exp);
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

        // Primality. `isprime` is the deterministic AKS test; `maxdegree` bounds the AKS ring degree
        // r so an infeasibly-large n fails fast instead of OOMing (default is very high — realistic
        // inputs never reach it). For large n prefer the fast probabilistic `isprobableprime`.
        m.fn("isprime", {{"n"}, {"maxdegree", "Integer", vm.makeInt(static_cast<int64_t>(bigint::kAksDefaultMaxDegree))}},
             "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "isprime");
            int64_t md = args[1].asInt("isprime maxdegree");
            if (md < 2) throw KiritoError("isprime: maxdegree must be >= 2");
            return vm.makeBool(isPrimeAKS(coerce(vm, args[0].handle(), "isprime n"), static_cast<uint64_t>(md)));
        });
        m.fn("isprobableprime", {{"n"}, {"rounds", "Integer", vm.makeInt(25)}}, "Bool",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "isprobableprime");
            int64_t rounds = args[1].asInt("isprobableprime rounds");
            if (rounds < 1) throw KiritoError("isprobableprime: rounds must be >= 1");
            return vm.makeBool(isProbablePrime(coerce(vm, args[0].handle(), "isprobableprime n"),
                                               static_cast<int>(rounds)));
        });
        m.fn("randomprime", {{"bits", "Integer"}, {"rounds", "Integer", vm.makeInt(25)}}, "BigInt",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "randomprime");
            int64_t bits = args[0].asInt("randomprime bits");
            int64_t rounds = args[1].asInt("randomprime rounds");
            if (bits < 2) throw KiritoError("randomprime: bits must be >= 2");
            if (bits > 1 << 16) throw KiritoError("randomprime: bits too large");
            if (rounds < 1) throw KiritoError("randomprime: rounds must be >= 1");
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
