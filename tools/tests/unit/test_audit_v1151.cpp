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

    // ===== A08-1: Integer.compare with ZERO tolerance is exact above 2^53 (an exact __int128 diff,
    // not a lossy double round-trip that collapses two int64 differing by 1). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
assert (2**62 + 1).compare(2**62, 0.0, 0.0) == False   # differ by 1 above 2^53 -> NOT close at 0 tol
assert (2**62).compare(2**62, 0.0, 0.0) == True        # equal -> close
assert (2**62 + 1).compare(2**62, 0.0, 2.0) == True    # within abs_tol 2 -> close
# still agrees with == and <
assert (2**62 + 1) != (2**62) and (2**62) < (2**62 + 1)
)"));
    }

    // ===== A09-4: a class defining _setstate_ but no _getstate_ must be REJECTED at serialize time
    // (the mirror of _getstate_-without-_setstate_, which already errors) — else _setstate_ silently
    // never runs on load. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var dump = import("dump")
class Half:
    var _init_ = Function(self):
        self.x = 1
    var _setstate_ = Function(self, s):
        self.x = s
var threw = False
try:
    discard dump.dumps(Half())
catch as e:
    threw = "_getstate_" in e
assert threw
# a class with BOTH still round-trips
class Whole:
    var _init_ = Function(self):
        self.x = 7
    var _getstate_ = Function(self):
        return self.x
    var _setstate_ = Function(self, s):
        self.x = s
assert dump.loads(dump.dumps(Whole())).x == 7
)"));
    }

    // ===== A05-1: range's guard is an ELEMENT-count cap (~32M), so a range that would allocate
    // multiple GB is rejected up front; a normal range still works. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var threw = False
try:
    discard range(40000000)          # > 32M cap, ~3 GB of IntVals -> rejected before allocating
catch as e:
    threw = "range too large" in e
assert threw
assert len(range(1000)) == 1000
)"));
    }

    // ===== A10-3: Matrix.row()'s out-of-range message names "Matrix" (matching getItem), not the
    // bare "row index out of range". =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var matrix = import("matrix")
var m = matrix.Matrix([[1.0, 2.0], [3.0, 4.0]])
var e1 = ""
try:
    discard m.row(9)
catch as e:
    e1 = e
assert "Matrix row index out of range" in e1
)"));
    }

    // ===== A01-1: an inline Function literal written across physical lines inside a bracket (lexer
    // line-continuation) must still serialize — the captured source is wrapped in parens so serde's
    // standalone re-parse sees the same newline suppression. =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var ser = import("serialize")
var mk = Function(f): return f
var k = mk(Function(p): return p[0] * 1000 +
    p[1])
assert k([2, 5]) == 2005
assert ser.loads(ser.dumps(k))([2, 5]) == 2005     # round-trips (used to throw at deserialize)
)"));
    }

    // ===== A01-4: an inline function body accepts `discard` and `assert` (the analyzer recommends
    // `discard` there; it used to be a parse error). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var g = Function(): return 41
var f = Function(): discard g()
discard f()
var h = Function(): assert 1 == 1
discard h()
)"));
    }

    // ===== A13-1: a class whose eager class-var is built by a captured FACTORY helper (which
    // instantiates another class) must load in a FRESH VM — the tiered build order sees the class
    // reachable only through the helper's free vars. Two VMs share the blob via a temp file. =====
    {
        std::string mk = "var p = import(\"path\").join(import(\"path\").gettempdir(), \"ki_a13_factory.kdmp\")\n";
        {
            KiritoVM a;
            CHECK(ok(a, mk + R"(
var dump = import("dump")
class Beta:
    var _init_ = Function(self):
        self.v = 777
var factory = Function(): return Beta()
class Alpha:
    var b = factory()
dump.save(Alpha(), p)
)"));
        }
        {
            KiritoVM b;   // FRESH VM: knows nothing of Alpha/Beta/factory
            CHECK(ok(b, mk + R"(
var dump = import("dump")
var loaded = dump.load(p)
assert loaded.b.v == 777
discard import("path").remove(p, missing_ok = True)
)"));
        }
    }

    // ===== A09-1 / A09-2: a function literal defined INSIDE a method inherits the method's class
    // ownership, so a nested closure/callback may touch self._private and resolve self._super_() —
    // while a function defined OUTSIDE a method still can't (privacy is only granted, never opened). =====
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
class Bag:
    var _init_ = Function(self):
        self._items = [500, 300, 900]
    var total = Function(self):
        var acc = 0
        var add = Function(v):           # closure reads self via capture; write to acc
            acc = acc + v
        for v in self._items:            # and self._items is private
            add(v)
        return acc
    var first_private = Function(self):
        var f = Function(): return self._items[0]   # private read INSIDE a nested function
        return f()
assert Bag().total() == 1700
assert Bag().first_private() == 500
class Base:
    var greet = Function(self): return "base"
class Sub(Base):
    var greet = Function(self): return "sub"
    var via_closure = Function(self):
        var f = Function(): return self._super_().greet()   # _super_ inside a nested function
        return f()
assert Sub().via_closure() == "base"
)"));
    }
    {
        KiritoVM vm;   // a function defined OUTSIDE any method still cannot reach a private
        CHECK(ok(vm, R"(
class Bag:
    var _init_ = Function(self):
        self._x = 5
var b = Bag()
var ext = Function(): return b._x
var denied = False
try:
    discard ext()
catch as e:
    denied = "private" in e
assert denied
)"));
    }

    // ===== A19.1-1: a user-class instance's _hash_/_eq_/_bool_ dispatch through its OWNING VM, so a
    // SECOND VM constructed on the same thread doesn't misroute them (multi-VM isolation contract). =====
    {
        KiritoVM a;
        KiritoVM b;      // constructed AFTER a -> activeVM() == &b for the rest of the scope
        (void)b;
        // Define the class AND exercise its dunders in ONE run on `a` (runSource doesn't persist across
        // calls). The K instances are owned by `a`; their _hash_/_eq_/_bool_ must dispatch through `a`
        // even though `b` is the thread's active VM — otherwise a's class handle is deref'd in b's arena.
        CHECK(ok(a, R"(
class K:
    var _init_ = Function(self, v):
        self.v = v
    var _hash_ = Function(self):
        return self.v
    var _eq_ = Function(self, o):
        return self.v == o.v
    var _bool_ = Function(self):
        return self.v != 0
assert len({K(1), K(2), K(1)}) == 2          # _hash_ + _eq_ via A
assert (K(1) in {K(1), K(2)})                # _eq_/_hash_ via A
assert Bool(K(0)) == False and Bool(K(5)) == True    # _bool_ via A
)"));
    }

    // ===== A02-3: a duplicate parameter name is a hard PARSE error (was a warn-and-run that then
    // desynced the resolver slot layout — assertion abort / silent wrong binding). =====
    {
        KiritoVM vm;
        bool threw = false;
        try { vm.runSource("var f = Function(a, b, a):\n    return a\n"); }
        catch (const KiritoError& e) {
            threw = std::string(e.what()).find("duplicate parameter name") != std::string::npos;
        }
        CHECK(threw);
    }
    {
        KiritoVM vm;   // distinct params still fine, incl. the shapes that used to abort
        CHECK(ok(vm, R"(
var f = Function(a, b):
    var x = 100
    var g = Function():
        return [x, a, b]
    return g()
assert f(1, 2) == [100, 1, 2]
)"));
    }

    // ===== A02-2: a module (frozen OR .ki-file — one shared rule now) hides a `_private` top-level
    // name but still exports a `_dunder_` (trailing underscore = not private) and ordinary names. =====
    {
        KiritoVM vm;
        vm.registerSourceModule("priv_export_mod", R"KI(
var _secret = 42
var _dunder_ = 7
var trailing_ = 1
var value = 99
)KI");
        CHECK(ok(vm, R"(
var m = import("priv_export_mod")
assert m.value == 99 and m.trailing_ == 1
assert not hasattr(m, "_secret")     # single-leading-underscore private is hidden
assert m._dunder_ == 7               # trailing-underscore name is NOT private -> exported
)"));
    }

    // ===== Batch 7 conformance changes (maintainer-approved; each updates a formerly-pinned test) =====

    // A18-5: tabular Series/DataFrame ops PROPAGATE missing (None/NaN) as None instead of throwing,
    // so the masking idiom survives a blank cell (pandas parity).
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var t = import("tabular")
assert (t.Series([1, None, 3]) + 1).tolist() == [2, None, 4]
assert (t.Series([1, None, 3]) > 0).tolist() == [True, None, True]
var df = t.DataFrame({"a": [1, None, 3], "b": [10, 20, 30]})
assert len(df[df["a"] > 1]) == 1               # blank row drops, no throw
)"));
    }

    // A17-3: regex must_advance — findall/sub/split don't drop a non-empty match masked by a
    // higher-priority zero-width one (Python parity).
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var re = import("regex")
assert re.findall("|\\w", "ab") == ["", "a", "", "b", ""]
assert re.findall("a||b", "ab") == ["a", "", "b", ""]
assert re.sub("|\\w", "X", "ab") == "XXXXX"
assert re.findall("a", "aaa") == ["a", "a", "a"]      # ordinary case unaffected
assert re.findall("a*", "aa") == ["aa", ""]           # trailing empty preserved
)"));
    }

    // A18-1: deque.pop()/popleft() on empty name "deque", not the internal "List".
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var co = import("collections")
var e1 = ""
try:
    co.deque().pop()
catch as e:
    e1 = e
assert "deque" in e1 and not ("List" in e1)
)"));
    }

    // A13-3: json.loads substitutes U+FFFD for a high surrogate + non-low \u (consistent with a lone
    // surrogate) instead of throwing; a valid pair still combines.
    {
        KiritoVM vm;
        CHECK(ok(vm, R"(
var json = import("json")
assert json.loads("\"\\uD800\\u0041\"") == chr(65533) + "A"
assert json.loads("\"\\uD83D\\uDE00\"") == chr(128512)     # valid pair -> U+1F600
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
