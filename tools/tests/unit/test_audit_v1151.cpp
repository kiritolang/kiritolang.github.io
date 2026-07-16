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

    // ===== A14-1: Tensor.take() with no argument must give a deterministic arity error, not read past
    // the empty arg span (an OOB read whose "value" changes with unrelated surrounding code). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var tensor = import("tensor")
var t = tensor.Tensor([[1.0, 2.0], [3.0, 4.0]])
var threw = False
try:
    discard t.take()
catch as e:
    threw = True
assert threw
# and it still works with an argument
assert t.take([0]).tolist() == [[1.0, 2.0]]
)"));
    }

    // ===== A14-5: in-place element assignment on a grad-tracking tensor must be refused (it would
    // silently desync the autograd graph and return gradients against stale pre-mutation values). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var tensor = import("tensor")
var w = tensor.Tensor([1.0, 2.0, 3.0], requiresgrad = True)
var threw = False
try:
    w[0] = 100.0
catch as e:
    threw = "grad" in e
assert threw
# a non-grad tensor still allows element assignment
var m = tensor.Tensor([1.0, 2.0, 3.0])
m[0] = 100.0
assert m.tolist() == [100.0, 2.0, 3.0]
)"));
    }

    // ===== A14-6: softplus is numerically stable — softplus(1000) ~ 1000, not inf. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var tensor = import("tensor")
var math = import("math")
var y = tensor.Tensor([1000.0]).softplus().item()
assert not math.isinf(y)
assert y.compare(1000.0, 0.0, 1e-6)
)"));
    }

    // ===== A08-2: math.prod with a zero factor after an overflowing prefix is 0, not a spurious
    // "result too large" (0 * anything is 0, trivially an Integer). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var math = import("math")
assert math.prod([4611686018427387904, 4, 0]) == 0
assert math.prod([2, 3, 4]) == 24
)"));
    }

    // ===== A16-2: File.writelines on a write-only stream raises a CLEAR "iterable" error, not a raw
    // std::bad_optional_access. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var io = import("io")
var path = import("path")
var p = path.join(path.gettempdir(), "ki_a16_writelines.txt")
var f = io.open(p, "w")
var threw = False
try:
    f.writelines(io.stdout)         # io.stdout is write-only: its iterate() returns nullopt
catch as e:
    threw = "iterable" in e
discard f.close()
discard path.remove(p)
assert threw
)"));
    }

    // ===== A04-2: a bound method backed by a NATIVE function honours keyword args and rejects unknown
    // ones (via applyCall/bindArgs), instead of silently dropping every keyword. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
class C:
    var f = format
var c = C()
# an unknown keyword is now REJECTED (was silently ignored)
var rejected = False
try:
    discard c.f(zzz = "nonsense")
catch as e:
    rejected = "keyword" in e
assert rejected
# a known keyword is now DELIVERED to the native (format applies 'f' to a non-number -> throws)
var delivered = False
try:
    discard c.f(spec = ".2f")
catch as e:
    delivered = "number" in e
assert delivered
)"));
    }

    // ===== A15-2: an embedded NUL in an argv element / shell command / cwd must be REJECTED (a
    // poison-NUL truncation lets validation and execution disagree), not silently truncated. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var sys = import("sys")
var n1 = False
try:
    discard sys.createprocess(["/bin/echo", "a\0b"])
catch as e:
    n1 = "NUL" in e
assert n1
var n2 = False
try:
    discard sys.shell("echo a\0b")
catch as e:
    n2 = "NUL" in e
assert n2
var n3 = False
try:
    discard sys.createprocess(["/bin/echo", "hi"], "/tmp\0evil")
catch as e:
    n3 = "NUL" in e
assert n3
)"));
    }

    // ===== A15-1: a bad cwd is reported as a DIRECTORY error, not "failed to start '<program>'"
    // (which blames a program that exists); a genuinely missing program still says "failed to start". =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var sys = import("sys")
var cwdErr = ""
try:
    discard sys.createprocess(["/bin/pwd"], "/no/such/dir/zzz_kirito_a15")
catch as e:
    cwdErr = e
assert "directory" in cwdErr
assert not ("failed to start" in cwdErr)
var missErr = ""
try:
    discard sys.createprocess(["no_such_prog_xyz_9z1q_a15"])
catch as e:
    missErr = e
assert "failed to start" in missErr
)"));
    }

    // ===== A14-4: tensor.arange rejects an oversized/non-finite range up front (not after allocating
    // ~1 GB); a normal arange still works. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var tensor = import("tensor")
var threw = False
try:
    discard tensor.arange(0.0, 1.0e308 * 10.0, 1.0)     # inf stop
catch as e:
    threw = "too large" in e
assert threw
assert tensor.arange(5.0).tolist() == [0.0, 1.0, 2.0, 3.0, 4.0]
)"));
    }

    // ===== A14-2: Matrix/ComplexMatrix determinant translates an engine error into a catchable
    // Kirito error (like inverse already does), and a normal determinant still computes. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var matrix = import("matrix")
assert matrix.Matrix([[1.0, 2.0], [3.0, 4.0]]).determinant().compare(-2.0, 0.0, 1e-9)
var inf = 1.0e308 * 10.0
var threw = False
try:
    discard matrix.Matrix([[inf, 0.0], [0.0, 1.0]]).determinant()
catch as e:
    threw = True
assert threw
)"));
    }

    // ===== A14-3: per-axis all()/any() over a ZERO-LENGTH axis returns the identity (True/False),
    // agreeing with the whole-tensor path and NumPy — instead of throwing "zero-size reduction". =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var T = import("tensor")
var z = T.zeros([3, 0])
assert z.all() == True and z.any() == False          # whole-tensor identity (unchanged)
assert z.all(1).tolist() == [1.0, 1.0, 1.0]          # per-axis over empty axis: all -> True
assert z.any(1).tolist() == [0.0, 0.0, 0.0]          # per-axis over empty axis: any -> False
# a non-empty axis is unaffected
assert T.Tensor([[1.0, 0.0], [1.0, 1.0]]).all(1).tolist() == [0.0, 1.0]
)"));
    }

    // ===== A11-2 / A11-1: base64.encode and xml text decoding are behaviour-preserving after the
    // O(n^2)->O(n) rewrite (List+join). Assert correctness (round-trips + known answers); the speed
    // win itself isn't unit-tested (a timing assertion would be flaky). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var b64 = import("base64")
assert b64.encode("Man") == "TWFu"                   # RFC 4648 known answer
assert b64.encode("M") == "TQ==" and b64.encode("Ma") == "TWE="   # 1- and 2-byte padding
var payload = "The quick brown fox" * 200            # non-trivial, exercises many triples
assert b64.decode(b64.encode(payload)) == List(payload.encode())   # decode -> List of byte Integers
var xml = import("xml")
var e = xml.fromstring("<r>a &lt; b &amp; c &gt; d &#65; &#x42;</r>")
assert e.text == "a < b & c > d A B"                 # named + numeric entities decode correctly
)"));
    }

    // ===== A02-1: compiler-generated hidden temporaries ($with0/$exc0) must NOT leak into a module's
    // public exports (cosmetic on inspect + a real resource-retention leak — a top-level `with` would
    // otherwise pin its context manager for the module's whole lifetime). =====
    {
        KiritoVM vm;
        vm.registerSourceModule("hidden_export_mod", R"KI(
class Ctx:
    var _enter_ = Function(self):
        return self
    var _exit_ = Function(self):
        return False
with Ctx() as x:
    var inside = 1
var value = 42
)KI");
        CHECK(ok(vm, R"(
var m = import("hidden_export_mod")
assert not ("$" in inspect(m))       # no compiler-hidden temporary in the exports
assert m.value == 42                  # ordinary top-level names still export
)"));
    }

    return RUN_TESTS();
}
