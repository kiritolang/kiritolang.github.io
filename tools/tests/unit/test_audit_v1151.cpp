// Regression tests for the v1.15.1 audit round. Each block pins ONE confirmed finding; where the
// bug only surfaced under the write-barrier soak, the test drives it at setGcThreshold(1). Every
// scenario runs in its own VM and self-asserts inside Kirito, so `ok(...)` returning false (an
// assert failure, a dangling-handle throw, or a missing guard) fails the test — and each was
// verified to fail on the pre-fix build.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a program; true iff it completes without throwing (asserts inside throw on failure).
static bool ok(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return true; } catch (...) { return false; }
}

int main() {
    // ===== A08-4: a BigInt and an equal native Integer share ONE Dict/Set bucket, either order.
    // probeBucket's symmetric-equality retry (mirroring kiEquals) — without it the two hash-equal
    // keys land in one bucket yet fail to dedupe, an order-dependent duplicate key. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var i = import("int")
var m = import("math")
assert len({5000, i.BigInt(5000)}) == 1
assert len({i.BigInt(5000), 5000}) == 1
assert (i.BigInt(1000) in {1000})
assert (1000 in {i.BigInt(1000)})
var d = {}
d[1000] = "native-first"
d[i.BigInt(1000)] = "bigint-second"
assert len(d) == 1
assert d[1000] == "bigint-second"
var d2 = {}
d2[i.BigInt(2000)] = "bigint-first"
d2[2000] = "native-second"
assert len(d2) == 1
# pinning: write-only NaN keys are UNTOUCHED (the retry is gated on a kind mismatch)
assert len({m.nan, m.nan}) == 2
)"));
    }

    // ===== A16-1: hash.pbkdf2 must REJECT an iterations >= 2^32 rather than silently truncating it
    // to a uint32_t (2^32+1 -> a single HMAC, the weak-KDF outcome the >= 1 guard exists to stop). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var h = import("hash")
var threw = False
try:
    discard h.pbkdf2("password", "salt", 4294967297)
catch:
    threw = True
assert threw
# the boundary value is accepted, and distinct work factors give distinct keys
assert h.pbkdf2("password", "salt", 1) != h.pbkdf2("password", "salt", 2)
)"));
    }

    // ===== A07-1: Bytes/BigInt are immutable + hashable; _setstate_ must be one-shot (deserializer
    // only). Re-homing an established value in place changes its hash inside a live bucket. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var i = import("int")
var dump = import("dump")
var k = Bytes([1, 2])
var threw = False
try:
    discard k._setstate_("ZZZ")
catch:
    threw = True
assert threw
var b = i.BigInt(42)
var threw2 = False
try:
    discard b._setstate_("99")
catch:
    threw2 = True
assert threw2
# an ordinary empty Bytes() is a REAL value (initialized), usable as a Set element
assert len({Bytes()}) == 1
# and the deserializer path still round-trips both types
assert dump.loads(dump.dumps(Bytes([1, 2, 3]))) == Bytes([1, 2, 3])
assert dump.loads(dump.dumps(i.BigInt(123456789))) == i.BigInt(123456789)
)"));
    }

    // ===== A06-1: Dict.popitem() drains a Dict that holds a NaN key (a write-only key that can't be
    // looked back up by equality) — popArbitrary takes the pair straight from its bucket. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var math = import("math")
var d = {}
d[math.nan] = "n"
d[1] = "one"
d[2] = "two"
var cnt = 0
while len(d) > 0:
    discard d.popitem()
    cnt = cnt + 1
assert cnt == 3
)"));
    }

    // ===== A03-1: Dict.update(<user _iter_ returning a fresh container>) must root every level —
    // the pairs and their key/value are swept mid-loop otherwise (dangling handle). Driven under the
    // write-barrier soak, where the unfixed build throws "dangling handle (stale generation)". =====
    {
        KiritoVM vm;
        vm.setGcThreshold(1);
        CHECK(ok(vm, R"(
class Item:
    var _init_ = Function(self, v):
        self.v = v
    var _hash_ = Function(self):
        discard "allocate-" + String(self.v)    # allocates -> forces a minor mid-probe
        return self.v
    var _eq_ = Function(self, o):
        return self.v == o.v
class Bag:
    var _iter_ = Function(self):
        return [[Item(1000), "v1000"], [Item(2000), "v2000"], [Item(3000), "v3000"]]
var d = {}
d.update(Bag())
assert len(d) == 3
assert d[Item(2000)] == "v2000"
)"));
    }

    return RUN_TESTS();
}
