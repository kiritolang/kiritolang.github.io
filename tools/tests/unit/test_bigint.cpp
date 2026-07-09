// Arbitrary-precision integers: the `int` module's BigInt value type, its operators, the integer
// math set (gcd/lcm/factorial/comb/perm/isqrt/abs/pow/modpow/modinv), and primality (deterministic
// trial division + Miller-Rabin). Covers known values, a differential fuzz against native Integer,
// the reflected-operator invariant, cross-type equality/hashing, serde, and adversarial inputs. The
// heavy fuzz/property loops run as single compiled Kirito programs (parsed once) so the test is fast.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string ev(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource("var int = import(\"int\")\n" + src));
}

int main() {
    KiritoVM vm;
    vm.installStandardLibrary();

    // ---- known values ----
    CHECK(ev(vm, "String(int.factorial(20))") == "2432902008176640000");
    CHECK(ev(vm, "String(int.factorial(50))") ==
          "30414093201713378043612608166064768844377641568960512000000000000");
    CHECK(ev(vm, "String(int.pow(2, 128))") == "340282366920938463463374607431768211456");
    CHECK(ev(vm, "String(int.fromstring(\"354224848179261915075\"))") == "354224848179261915075");  // fib(100)
    CHECK(ev(vm, "String(int.BigInt(4).modpow(13, 497))") == "445");
    CHECK(ev(vm, "String(int.isqrt(int.pow(2, 200)))") == "1267650600228229401496703205376");  // 2^100
    CHECK(ev(vm, "String(int.comb(100, 50))") == "100891344545564193334812497256");
    CHECK(ev(vm, "String(int.isqrt(int.pow(10, 100)))") ==
          "100000000000000000000000000000000000000000000000000");

    // ---- true division yields Float ----
    CHECK(ev(vm, "int.BigInt(10) / int.BigInt(4)") == "2.5");
    CHECK(ev(vm, "type(int.BigInt(10) / int.BigInt(4))") == "Float");

    // ---- reflected-operator invariant: dispatch on the LEFT operand only ----
    CHECK(ev(vm, "String(int.BigInt(2) + 3)") == "5");
    CHECK_THROWS(ev(vm, "3 + int.BigInt(2)"));
    CHECK_THROWS(ev(vm, "3 * int.BigInt(2)"));
    CHECK_THROWS(ev(vm, "3 < int.BigInt(2)"));
    // == / != stay symmetric across types
    CHECK(ev(vm, "int.BigInt(5) == 5") == "True");
    CHECK(ev(vm, "5 == int.BigInt(5)") == "True");
    CHECK(ev(vm, "6 != int.BigInt(5)") == "True");
    // hashing: an equal BigInt and Integer share a Dict/Set bucket
    CHECK(ev(vm, "var d = {}\nd[int.BigInt(42)] = 1\nd[42]") == "1");
    CHECK(ev(vm, "len({int.BigInt(7), 7, int.BigInt(7)})") == "1");

    // ---- heavy fuzz / property loops, all in ONE compiled program (one parse+run keeps the arena
    // small — GC reclaims each iteration's garbage as the single program runs) ----
    CHECK(ev(vm, R"KI(
var r = import("random")
var math = import("math")
# differential vs native Integer, in a no-overflow range
var i = 0
while i < 3000:
    var a = r.randombelow(2000000000) - 1000000000
    var b = r.randombelow(2000000000) - 1000000000
    assert String(int.BigInt(a) + b) == String(a + b)
    assert String(int.BigInt(a) - b) == String(a - b)
    assert String(int.BigInt(a) * b) == String(a * b)
    assert (int.BigInt(a) < b) == (a < b)
    assert (int.BigInt(a) >= b) == (a >= b)
    assert (int.BigInt(a) == b) == (a == b)
    if b != 0:
        assert String(int.BigInt(a) // b) == String(a // b)
        assert String(int.BigInt(a) % b) == String(a % b)
    i = i + 1
# ** with small base/exp that stays in int64 range
i = 0
while i < 500:
    var base = r.randombelow(61) - 30
    var e = r.randombelow(11)
    assert String(int.BigInt(base) ** e) == String(base ** e)
    i = i + 1
# base round-trips 2..36
for bs in range(2, 37):
    var n = int.pow(int.BigInt(bs), 80) + 12345
    assert int.fromstring(String(n)) == n
# isqrt floor property
i = 0
while i < 500:
    var x = r.randombelow(1000000000000000000)
    var s = int.isqrt(x)
    assert s * s <= x and (s + 1) * (s + 1) > x
    i = i + 1
# modinv property
i = 0
while i < 500:
    var m = 2 + r.randombelow(100000)
    var a2 = 1 + r.randombelow(m - 1)
    if int.gcd(int.BigInt(a2), m) == 1:
        assert (int.modinv(a2, m) * a2) % m == 1
    i = i + 1
# int math == math module where it doesn't overflow
assert String(int.gcd(48, 60)) == String(math.gcd(48, 60))
assert String(int.factorial(12)) == String(math.factorial(12))
assert String(int.comb(20, 7)) == String(math.comb(20, 7))
assert String(int.perm(15, 4)) == String(math.perm(15, 4))
assert String(int.lcm(12, 18)) == String(math.lcm(12, 18))
# deterministic and probabilistic primality agree with a reference over a range
var refprime = Function(n) -> Bool:
    if n < 2:
        return False
    var j = 2
    while j * j <= n:
        if n % j == 0:
            return False
        j = j + 1
    return True
for n in range(0, 2000):
    assert int.isprime(n) == refprime(n)
    assert int.isprobableprime(n) == refprime(n)
# randomprime output is prime with the requested bit length
for bits in [16, 32, 48, 64, 96]:
    var p = int.randomprime(bits)
    assert int.isprobableprime(p)
    assert p.bitlength() == bits
"loops-ok")KI") == "loops-ok");

    CHECK(ev(vm, "String(int.BigInt(\"-123456789012345678901234567890\"))") == "-123456789012345678901234567890");
    CHECK(ev(vm, "String(int.BigInt(\"0xff\"))") == "255");
    CHECK(ev(vm, "String(int.fromstring(\"ff\", 16))") == "255");
    CHECK(ev(vm, "String(int.fromstring(\"zz\", 36))") == "1295");

    CHECK(ev(vm, "String(int.isqrt(0)) + \",\" + String(int.isqrt(1)) + \",\" + String(int.isqrt(2))") == "0,1,1");

    // ---- primality specials (single calls) ----
    // Carmichael numbers (composite but Fermat-fool): Miller-Rabin must still reject them.
    for (const char* c : {"561", "1105", "1729", "2465", "2821", "6601", "8911", "41041", "825265"}) {
        CHECK(ev(vm, std::string("int.isprobableprime(") + c + ")") == "False");
        CHECK(ev(vm, std::string("int.isprime(") + c + ")") == "False");
    }
    // Mersenne primes + large known primes recognised by Miller-Rabin.
    CHECK(ev(vm, "int.isprobableprime(int.pow(2, 61) - 1)") == "True");   // M61
    CHECK(ev(vm, "int.isprobableprime(int.pow(2, 89) - 1)") == "True");   // M89
    CHECK(ev(vm, "int.isprobableprime(int.pow(2, 64) - 59)") == "True");  // largest 64-bit prime
    CHECK(ev(vm, "int.isprobableprime(int.pow(2, 127) - 1)") == "True");  // M127

    // ---- BigInt methods ----
    CHECK(ev(vm, "int.pow(2, 100).bitlength()") == "101");
    CHECK(ev(vm, "int.BigInt(255).toint()") == "255");
    CHECK(ev(vm, "type(int.BigInt(255).toint())") == "Integer");
    CHECK_THROWS(ev(vm, "int.pow(2, 100).toint()"));  // doesn't fit a native Integer

    // ---- serialize / dump round-trips ----
    CHECK(ev(vm, "var s = import(\"serialize\")\nvar b = int.pow(7, 60)\ns.loads(s.dumps(b)) == b") == "True");
    CHECK(ev(vm, "var d = import(\"dump\")\nvar b = int.pow(-3, 41)\nd.loads(d.dumps(b)) == b") == "True");

    // ---- adversarial / bad input ----
    CHECK_THROWS(ev(vm, "int.BigInt(5) // 0"));
    CHECK_THROWS(ev(vm, "int.BigInt(5) % 0"));
    CHECK_THROWS(ev(vm, "int.modpow(2, 10, 0)"));
    CHECK_THROWS(ev(vm, "int.modinv(4, 8)"));               // not coprime
    CHECK_THROWS(ev(vm, "int.modinv(3, 1)"));               // modulus < 2
    CHECK_THROWS(ev(vm, "int.fromstring(\"12x\", 10)"));   // bad digit
    CHECK_THROWS(ev(vm, "int.fromstring(\"7\", 40)"));     // bad base
    CHECK_THROWS(ev(vm, "int.BigInt(\"not a number\")"));
    CHECK_THROWS(ev(vm, "int.factorial(-1)"));
    CHECK_THROWS(ev(vm, "int.pow(int.BigInt(2), int.pow(2, 40))"));   // exponent too large / result guard
    CHECK_THROWS(ev(vm, "int.BigInt(3.14)"));              // Float not accepted
    CHECK(ev(vm, "int.isprime(-7)") == "False");
    CHECK(ev(vm, "int.isprime(0)") == "False");
    CHECK(ev(vm, "int.isprime(1)") == "False");

    if (kitest::failures == 0) std::printf("test_bigint: all passed\n");
    return RUN_TESTS();
}
