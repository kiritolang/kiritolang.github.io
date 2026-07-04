// test_audit_hardening.cpp — probes the deep-audit hardening fixes, with an emphasis on the
// MEMORY-SAFETY ones: run under AddressSanitizer, a regression surfaces as a use-after-free / crash.
//
// The technique for the memory-safety blocks is an AGGRESSIVE GC threshold (setGcThreshold(1) => a
// collection on every allocation), so any temporary the fixed code failed to root is swept
// immediately and dereferenced-while-dead — exactly what asan flags. On a correct build every block
// is quiet and the assertions on the *result* also confirm behavioural correctness.
//
// Covers: List.sort snapshot (iterator-invalidation UAF), Set.union/symmetricdifference GC rooting,
// Dict/Set reentrant _eq_/_hash_ guard, value.hpp List-from-vector rooting, format-spec precision
// overflow, math.comb 128-bit intermediate, Integer() doubled base prefix, serde/dump hostile-count
// rejection, regex lone-surrogate escape, zlib FDICT, tensor norm(-inf)/zero-dim, nested-f-string
// depth guard, and break/continue-in-finally.

#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a Kirito program on a fresh VM (optional aggressive GC) and return the stringified result.
static std::string run(const std::string& src, std::size_t gc = 0) {
    KiritoVM vm;
    if (gc) vm.setGcThreshold(gc);
    return vm.stringify(vm.runSource(src));
}
// Run a program and return the error text ("<ok>" if it did not throw).
static std::string errOf(const std::string& src, std::size_t gc = 0) {
    KiritoVM vm;
    if (gc) vm.setGcThreshold(gc);
    try { vm.runSource(src); return "<ok>"; }
    catch (const std::exception& e) { return e.what(); }
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // ============================================================================================
    // MEMORY SAFETY under a GC-on-every-alloc threshold (asan is the real oracle here).
    // ============================================================================================
    {
        // List.sort with a key that mutates the SAME list: must snapshot, sort, reassign — no UAF.
        CHECK(run(R"KI(
var xs = [5, 3, 4, 1, 2]
var f = Function(x):
    xs.append(99)
    return x
xs.sort(key = f)
xs
)KI", 1) == "[1, 2, 3, 4, 5]");

        // A comparator (_lt_) that mutates the SAME list being sorted is also snapshot-safe: the sort
        // reassigns from the snapshot, so the concurrent appends are discarded (final length is 3).
        CHECK(run(R"KI(
var items = []
class Boxed:
    var _init_ = Function(self, v):
        self.v = v
    var _lt_ = Function(self, other):
        items.append(self)
        return self.v < other.v
items = [Boxed(3), Boxed(1), Boxed(2)]
items.sort()
len(items)
)KI", 1) == "3");

        // Set.union / symmetricdifference / difference with a String `other`: iterate() yields FRESH
        // element handles that must be rooted across the trailing vm.alloc.
        CHECK(run(R"KI(var s = {1}
len(s.union("abcde")))KI", 1) == "6");
        CHECK(run(R"KI(len(({1, 2}).symmetricdifference("ab")))KI", 1) == "4");
        CHECK(run(R"KI(len(({1}).difference("ab")))KI", 1) == "1");
        CHECK(run(R"KI(len(({1}).union(Bytes([120, 121, 122]))))KI", 1) == "4");  // Bytes other (Integer elems)

        // value.hpp List-from-vector ctor must root its elements across its own alloc.
        {
            KiritoVM vm;
            vm.setGcThreshold(1);
            std::vector<Handle> hs;
            {
                RootScope rs(vm);
                for (int i = 0; i < 32; ++i) hs.push_back(rs.add(vm.makeString("elem-" + std::to_string(i))));
                // Build under aggressive GC; the ctor's internal alloc collects — elements must survive.
                List built(vm, hs);
                vm.collectGarbage();
                CHECK(built.size() == 32);
                CHECK(vm.stringify(built[0]) == "elem-0");
                CHECK(vm.stringify(built[31]) == "elem-31");
            }
        }
    }

    // ============================================================================================
    // Dict/Set reentrancy: mutating the container inside a key's _eq_/_hash_ is a clean, catchable
    // error (never a bucket use-after-free). Stress it many times so asan has plenty to catch.
    // ============================================================================================
    {
        const char* prog = R"KI(
var d = {}
class Reentrant:
    var _init_ = Function(self): pass
    var _hash_ = Function(self): return 0
    var _eq_ = Function(self, other):
        d["injected"] = 1
        return False
d[Reentrant()] = 1
var caught = 0
var i = 0
while i < 200:
    try:
        d[Reentrant()] = 2
    catch as e:
        caught = caught + 1
    i = i + 1
caught
)KI";
        CHECK(run(prog, 1) == "200");
        // The same for a Set (add probes the bucket, runs _eq_).  `{}` is a Dict literal, so an empty
        // Set needs the Set() constructor.
        CHECK(has(errOf(R"KI(
var st = Set()
class RS:
    var _init_ = Function(self): pass
    var _hash_ = Function(self): return 0
    var _eq_ = Function(self, other):
        st.add(1)
        return False
var seen = st
seen.add(RS())
seen.add(RS())
)KI", 1), "Set changed size"));
    }

    // ============================================================================================
    // Correctness / overflow / hostile-input fixes.
    // ============================================================================================
    {
        // math.comb: representable central binomials no longer falsely "too large".
        CHECK(run(R"KI(import("math").comb(64, 32))KI") == "1832624140942590534");
        CHECK(run(R"KI(import("math").comb(52, 5))KI") == "2598960");
        CHECK(run(R"KI(import("math").comb(0, 0))KI") == "1");
        CHECK_THROWS(run(R"KI(import("math").comb(100000, 50000))KI"));  // genuinely too large: still throws

        // Integer(): a doubled base prefix is rejected; single prefixes still parse.
        CHECK(run(R"KI(Integer("0x1F"))KI") == "31");
        CHECK(run(R"KI(Integer("0o17"))KI") == "15");
        CHECK(run(R"KI(Integer("0b1010"))KI") == "10");
        CHECK_THROWS(run(R"KI(Integer("0x0x5"))KI"));
        CHECK_THROWS(run(R"KI(Integer("0xZ"))KI"));

        // format precision (the mini-spec used by the format() builtin + f-strings): an enormous
        // precision is a clean error, not signed-int-overflow UB.
        CHECK(has(errOf(R"KI(format(1.5, ".2345678901f"))KI"), "precision too large"));
        CHECK(run(R"KI(format(3.14159, ".3f"))KI") == "3.142");
        CHECK(run(R"KI(f"{3.14159:.3f}")KI") == "3.142");

        // String padding still correct (byte-length bound added for multibyte fills).
        CHECK(run(R"KI("x".ljust(5, "."))KI") == "x....");
        CHECK(run(R"KI("x".center(5))KI") == "  x  ");

        // regex: a lone UTF-16 surrogate escape is rejected (dead pattern element otherwise).
        CHECK(has(errOf(R"KI(import("regex").compile(r"\uD800"))KI"), "surrogate"));
        CHECK(run(R"KI(import("regex").search(r"A", "xAy") != None)KI") == "True");  // valid \u still works

        // regex: a group-dense pattern (huge numGroups) is rejected at COMPILE time so the Pike VM's
        // per-thread capture-copy can't turn into an O(input*program*numGroups) hang. 2000 groups is
        // over the 1000 cap; 500 is under it and still compiles.
        CHECK(has(errOf(R"KI(import("regex").compile("()" * 2000))KI"), "too many capture groups"));
        CHECK(run(R"KI(import("regex").compile("()" * 500) != None)KI") == "True");

        // f-string: the `{…}` scanner is quote-aware — a `:` or `}` inside a string literal is data,
        // not a spec separator / closing brace (was mis-split when the scanner ignored quotes).
        CHECK(run(R"KI(f"{'a:b'}")KI") == "a:b");        // colon inside a string is not a spec sep
        CHECK(run(R"KI(f"{'x:y':>6}")KI") == "   x:y");   // real spec still applies after a quoted colon
        CHECK(run(R"KI(var d = {"}": 9}
f"{d['}']}")KI") == "9");                                 // brace inside a string does not close the field

        // lexer: a genuine NUL byte inside a string literal is a valid character, not a premature EOF
        // (was mis-reported as "unterminated string" because '\0' doubled as the end-of-input sentinel).
        {
            std::string src = "var s = \"";
            src += '\0';               // an embedded NUL byte in the literal
            src += "Z\"\ndiscard len(s)\n";
            CHECK(errOf(src) == "<ok>");
            std::string val = "var s = \"";
            val += '\0';
            val += "Z\"\nlen(s)\n";
            CHECK(run(val) == "2");    // NUL + 'Z' == two code points
        }

        // zlib: a preset-dictionary (FDICT) stream is rejected clearly instead of misparsed.
        // CMF=0x78, FLG=0x20 is a valid header check (30752 % 31 == 0) with the FDICT bit set.
        CHECK(has(errOf(R"KI(import("zlib").decompress(Bytes([120, 32, 0, 0, 0, 0, 0, 0])))KI"),
                  "preset dictionary"));

        // dump/serialize: a hostile object-count is rejected (no OOM / crash).
        CHECK_THROWS(run(R"KI(import("dump").loads(Bytes([75, 68, 77, 80, 1, 255, 255, 255, 255])))KI"));
        CHECK_THROWS(run(R"KI(import("dump").loads(Bytes([75, 68, 77, 80, 1, 4, 0, 0, 0])))KI"));  // truncated body

        // tensor: norm(ord=-inf) is min|x| (was wrongly max); a 0-axis shape is allowed (0 elements).
        CHECK(run(R"KI(import("tensor").norm(import("tensor").Tensor([3.0, -1.0, 4.0]), ord = Float("-inf")))KI")
              == "1.0");
        CHECK(run(R"KI(import("tensor").norm(import("tensor").Tensor([3.0, -1.0, 4.0]), ord = Float("inf")))KI")
              == "4.0");
        CHECK(errOf(R"KI(discard import("tensor").zeros([100000000, 0]))KI") == "<ok>");
    }

    // ============================================================================================
    // Parser: deeply nested f-strings must hit the depth guard (clean error), not overflow the
    // native stack. Build ~4000 alternating-quote levels programmatically.
    // ============================================================================================
    {
        std::string src = "0";
        for (int i = 0; i < 4000; ++i) {
            char q = (i % 2 == 0) ? '"' : '\'';
            src = std::string("f") + q + "{" + src + "}" + q;
        }
        std::string e = errOf(src);
        CHECK(e != "<ok>");                 // it must be REJECTED...
        CHECK(!has(e, "Segmentation"));     // ...cleanly, not via a crash
    }

    // ============================================================================================
    // Compiler: break / continue / return in a `finally` reached via an exception (was crash/hang).
    // ============================================================================================
    {
        CHECK(run(R"KI(
var out = []
for i in [1, 2, 3]:
    try:
        throw "x"
    finally:
        out.append(i)
        continue
out
)KI") == "[1, 2, 3]");
        CHECK(run(R"KI(
var f = Function():
    try:
        throw "boom"
    finally:
        return 42
f()
)KI") == "42");
        // A finally WITHOUT break/continue/return still propagates.
        CHECK(run(R"KI(
var log = []
try:
    try:
        throw "boom"
    finally:
        log.append("fin")
catch as e:
    log.append(e)
log
)KI") == "['fin', 'boom']");
    }

    // ============================================================================================
    // The VM stays healthy after all the adversarial paths above (reuse one VM for a mixed batch).
    // ============================================================================================
    {
        KiritoVM vm;
        vm.setGcThreshold(1);
        for (int i = 0; i < 50; ++i) {
            (void)vm.runSource(R"KI(var a = [3,1,2]
a.sort()
var s = {1}
discard s.union("xy")
discard format(1.5, ".2f"))KI");
        }
        CHECK(vm.stringify(vm.runSource("1 + 1\n")) == "2");
    }

    return RUN_TESTS();
}
