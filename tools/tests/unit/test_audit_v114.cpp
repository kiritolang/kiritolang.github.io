// Regression tests for the v1.14 audit (.audit/v1.14/). Each block pins one confirmed finding so it
// can never silently regress. Memory-safety items (A06-1 deep-_str_ native recursion) are also a
// crash gate — run under -fsanitize=address,undefined for the strongest signal.
#include <cstdint>
#include <new>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string err(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    try { vm.runSource(src); return ""; }
    catch (const KiritoError& e) { return e.what(); }
    catch (const std::exception& e) { return std::string("std:") + e.what(); }
}
static std::string ok(const std::string& src) {
    KiritoVM vm;
    vm.installStandardLibrary();
    return vm.stringify(vm.runSource(src));
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // === A09-1 (MEDIUM): the format() builtin with an EMPTY spec was lossy for Floats — it routed
    // through applyFormatSpec's default ('g'/precision-6), so `format(2.0)` -> "2" and
    // `format(1234567.0)` -> "1.23457e+06", diverging from f-strings / "{}".format / String() (all
    // stringify). Empty spec now short-circuits to stringify, so a Float round-trips. ===
    CHECK(ok("format(2.0)") == "2.0");
    CHECK(ok("format(1234567.0)") == "1234567.0");
    CHECK(ok("format(0.1)") == "0.1");
    CHECK(ok("format(42)") == "42");           // Integer unchanged
    CHECK(ok("format(3.5)") == "3.5");
    // a NON-empty spec still applies the mini-format-spec (no behaviour change there)
    CHECK(ok("format(2.0, \".2f\")") == "2.00");
    CHECK(ok("format(255, \"x\")") == "ff");
    // consistency: format(x) with no spec now equals String(x) for a Float, as the doc-comment claims
    CHECK(ok("format(2.0) == String(2.0)") == "True");

    // === A06-1 (MEDIUM, native recursion / crash): InstanceValue::str's cycle guard only caught a
    // _str_ that returns SELF; a _str_ that returns a chain of DISTINCT instances recursed in native
    // C++ with no depth bound -> stack overflow (segfault). Now bounded by ctx.depth like the
    // container stringifier: it throws a clean, catchable error instead of crashing. ===
    CHECK(has(err(
        "class N:\n"
        "    var _init_ = Function(self, nxt): self.nxt = nxt\n"
        "    var _str_ = Function(self): return self.nxt\n"
        "var head = None\n"
        "var i = 0\n"
        "while i < 5000:\n"
        "    head = N(head)\n"
        "    i = i + 1\n"
        "String(head)"), "recurses too deeply"));
    // a shallow _str_-returns-instance chain still works (the guard doesn't over-reject)
    CHECK(ok("class W:\n"
             "    var _init_ = Function(self, v): self.v = v\n"
             "    var _str_ = Function(self): return self.v\n"
             "String(W(W(\"deep\")))") == "deep");

    // === A16-1 (MEDIUM, UB): time.sleep() had no finite/range guard, so sleep(inf)/sleep(nan)
    // overflowed the int64 duration_cast (UB, returned immediately) and sleep(1e18) hung. Both are
    // now rejected before sleep_for. (A tiny real sleep still works.) ===
    CHECK(has(err("import(\"time\").sleep(import(\"math\").nan)"), "finite"));
    CHECK(has(err("import(\"time\").sleep(import(\"math\").inf)"), "finite"));
    CHECK(has(err("import(\"time\").sleep(1e18)"), "too large"));
    CHECK(ok("import(\"time\").sleep(0.0)\n42") == "42");                        // <=0 is a no-op
    CHECK(ok("import(\"time\").sleep(-5.0)\n7") == "7");

    // === A14-2 (MEDIUM, signed-overflow UB): json.stringify's indent path computed (depth+1)*indent
    // in int, overflowing for a huge indent (and OOMing with a raw allocator message short of that).
    // A too-large indent is now a clean error; a sane indent still pretty-prints. ===
    CHECK(has(err("import(\"json\").stringify([1, 2], indent = 99999999)"), "indent too large"));
    CHECK(has(err("import(\"json\").stringify([1], indent = 100000000000)"), "indent too large"));
    CHECK(ok("import(\"json\").stringify([1, 2], indent = 2)") == "[\n  1,\n  2\n]");
    CHECK(ok("import(\"json\").stringify([1, 2])") == "[1, 2]");                 // compact default

    // === A15-1 (MEDIUM): Socket.listen() read sock.fd directly instead of fdOrThrow, so
    // listen-after-close leaked a raw "Bad file descriptor" instead of the contractual
    // "listen: socket is closed" every other Socket method gives. ===
    CHECK(has(err("var s = import(\"net\").tcpsocket()\ns.close()\ns.listen(16)"),
              "listen: socket is closed"));

    // === A11-1 (MEDIUM, security): a Kirito String is byte-transparent, so it can hold a NUL; the
    // OS filesystem APIs truncate at the first NUL, so "safe.txt\0.sh" opened "safe.txt" — a
    // suffix-validation / sandbox bypass. Every filesystem-touching path entry now rejects it. ===
    CHECK(has(err("import(\"io\").open(\"safe\" + chr(0) + \".sh\", \"w\")"), "embedded NUL"));
    CHECK(has(err("import(\"path\").exists(\"a\" + chr(0) + \"b\")"), "embedded NUL"));
    CHECK(has(err("import(\"path\").getsize(\"x\" + chr(0))"), "embedded NUL"));
    // the pure path-string builder join() doesn't touch the filesystem, so it still works normally
    CHECK(ok("import(\"path\").join(\"a\", \"b\")") == "a/b");

    // === A11-19 (MEDIUM): BytesIO.truncate() after a large seek materialized the whole buffer,
    // bypassing the 256 MiB write guard. It now honours the same cap. ===
    CHECK(has(err("var b = import(\"io\").BytesIO()\nb.seek(400000000)\nb.truncate()"),
              "BytesIO too large"));
    CHECK(ok("var b = import(\"io\").BytesIO()\nb.write(\"hello world\")\nb.seek(5)\nb.truncate()") == "5");

    // (A04-1 was a FALSE POSITIVE: `not <instance>` intentionally returns the RAW _not_ result,
    // uncoerced — like _neg_, unlike _bool_ — a documented, tested contract [r7_types.ki]. No change.)

    // === A04-2 (LOW): the cyclic-structure recursion guard is shared by == AND </sort/min/max, so
    // its message now says "comparison", not "equality". ===
    CHECK(has(err("var a = [1]\na.append(a)\nvar b = [1]\nb.append(b)\na < b"),
              "comparison recursion depth"));
    CHECK(has(err("var a = [1]\na.append(a)\nvar b = [1]\nb.append(b)\na == b"),
              "comparison recursion depth"));

    // === A13-1 (MEDIUM): tensor max/min/argmax/argmin were order-dependent under NaN
    // (max([nan,1,3])->nan but max([1,nan,3])->3.0). max/min now PROPAGATE NaN (numpy amax/amin)
    // so the value is position-independent; argmax/argmin return the first-NaN index. ===
    CHECK(ok("var t = import(\"tensor\")\nvar m = import(\"math\")\n"
             "String(m.isnan(t.Tensor([m.nan, 1.0, 3.0]).max())) + "
             "String(m.isnan(t.Tensor([1.0, m.nan, 3.0]).max())) + "
             "String(m.isnan(t.Tensor([1.0, 3.0, m.nan]).max()))") == "TrueTrueTrue");
    CHECK(ok("var t = import(\"tensor\")\nvar m = import(\"math\")\n"
             "String(m.isnan(t.Tensor([m.nan, 1.0]).min())) + String(m.isnan(t.Tensor([1.0, m.nan]).min()))")
          == "TrueTrue");
    // no NaN: ordinary max/min/argmax/argmin unchanged
    CHECK(ok("var t = import(\"tensor\")\nvar d = t.Tensor([1.0, 5.0, 3.0])\n"
             "String(d.max()) + \",\" + String(d.min()) + \",\" + String(d.argmax()) + \",\" + String(d.argmin())")
          == "5.0,1.0,1,0");
    // argmax/argmin with a NaN return the FIRST NaN index (numpy semantics), position-consistent
    CHECK(ok("var t = import(\"tensor\")\nvar m = import(\"math\")\n"
             "String(t.Tensor([1.0, m.nan, 3.0]).argmax()) + String(t.Tensor([1.0, 3.0, m.nan]).argmax())")
          == "12");
    // per-axis matches the whole-tensor rule: a NaN in the reduced line -> NaN
    CHECK(ok("var t = import(\"tensor\")\nvar m = import(\"math\")\n"
             "var r = t.Tensor([[m.nan, 1.0], [2.0, 3.0]]).max(axis = 1)\n"
             "String(m.isnan(r.tolist()[0])) + String(r.tolist()[1])") == "True3.0");

    // === A13-2 (LOW): complex.atan(±i) has poles there; it now throws a domain error like its
    // mirror atanh(±1), instead of returning 0.0+infi silently. ===
    CHECK(has(err("import(\"complex\").atan(import(\"complex\").i)"), "domain error"));
    CHECK(ok("import(\"complex\").atan(import(\"complex\").real(0.5)).re > 0.4") == "True");

    // === A13-3 (LOW): Matrix._setstate_ with a negative dimension cast to a huge size_t and slipped
    // past the element guard (c==0), yielding rows()==-1 from untrusted state. Reject it. ===
    CHECK(has(err("import(\"matrix\").zeros(1, 1)._setstate_([-1, 0, []])"), "negative dimension"));

    // === A17-1 (MEDIUM, DoS): the linear-time regex is O(text * program * groups) because each NFA
    // thread copies its capture vector; a pattern with hundreds of groups over a long input ran for
    // tens of seconds ((a?)x500 + b over 10k a's). A capture-volume budget now throws instead of
    // hanging (matching an untrusted pattern is a stated security property). ===
    CHECK(has(err(
        "var re = import(\"regex\")\nvar pat = \"\"\nvar i = 0\n"
        "while i < 500:\n    pat = pat + \"(a?)\"\n    i = i + 1\n"
        "pat = pat + \"b\"\ndiscard re.search(pat, \"a\" * 10000)"), "complexity budget"));
    // a legitimate regex (few groups) is unaffected
    CHECK(ok("import(\"regex\").search(\"(\\\\d+)-(\\\\d+)\", \"x 12-34 y\").group(2)") == "34");
    CHECK(ok("String(import(\"regex\").findall(\"\\\\w+\", \"a bb ccc\") == [\"a\", \"bb\", \"ccc\"])") == "True");

    // === A14-1 (MEDIUM, silent data corruption): a class whose _getstate_ returns a Dict/Set keyed
    // by a content-hashed member (Bytes/Matrix/instance) and whose _setstate_ READS THROUGH that
    // container saw it EMPTY — the container was deferred (A17-1) to a pass AFTER _setstate_ ran, so
    // it summed to 0 not 42. Native-Stateful members are now restored, and their containers wired,
    // BEFORE the user _setstate_ pass. Both text and binary codecs. ===
    CHECK(ok("var ser = import(\"serialize\")\n"
             "class C:\n    var _init_ = Function(self): self.total = 0\n"
             "    var _getstate_ = Function(self): return {Bytes([9]): 42}\n"
             "    var _setstate_ = Function(self, s):\n        var t = 0\n"
             "        for k in s:\n            t = t + s[k]\n        self.total = t\n"
             "ser.loads(ser.dumps(C())).total") == "42");
    CHECK(ok("var d = import(\"dump\")\n"
             "class C:\n    var _init_ = Function(self): self.total = 0\n"
             "    var _getstate_ = Function(self): return {Bytes([9]): 42}\n"
             "    var _setstate_ = Function(self, s):\n        var t = 0\n"
             "        for k in s:\n            t = t + s[k]\n        self.total = t\n"
             "d.loads(d.dumps(C())).total") == "42");
    // A17-1 must still hold: a plain Bytes-keyed Dict/Set round-trips with correct buckets
    CHECK(ok("var ser = import(\"serialize\")\n"
             "var d2 = ser.loads(ser.dumps({Bytes([1, 2]): \"x\", Bytes([3]): \"y\"}))\n"
             "d2[Bytes([1, 2])] + d2[Bytes([3])] + String(len(d2))") == "xy2");

    // === A12-1 (LOW): random gauss/uniform/expovariate param validation used </<=  which a NaN
    // slips past (NaN compares false), so gauss(0, nan) returned a quiet NaN instead of erroring.
    // Reject non-finite params, consistent with the strict sigma<0 / lambda<=0 guards. ===
    CHECK(has(err("import(\"random\").Random(1).gauss(0, import(\"math\").nan)"), "finite"));
    CHECK(has(err("import(\"random\").Random(1).uniform(0, import(\"math\").inf)"), "finite"));
    CHECK(has(err("import(\"random\").Random(1).expovariate(import(\"math\").nan)"), "finite"));
    CHECK(has(err("import(\"random\").Random(1).gauss(0, -1)"), "non-negative"));  // existing guard intact
    // ordinary params still work
    CHECK(ok("var x = import(\"random\").Random(1).uniform(0, 1)\nx >= 0.0 and x <= 1.0") == "True");

    // === A10-2 (LOW): hasattr's probe caught only KiritoError, so a native getAttr throwing a plain
    // std::exception would escape. It now catches std::exception (like the try/catch boundary), so a
    // missing member is reported as False. (Positive/negative existence is the observable behaviour.) ===
    CHECK(ok("String(hasattr(\"hi\", \"upper\")) + String(hasattr([1], \"append\")) + "
             "String(hasattr(5, \"definitely_not_a_method\"))") == "TrueTrueFalse");

    // === A19-1 / A19-2 (MEDIUM, embedding memory): Value's arithmetic/unary operators, call,
    // getAttr, at (getItem) and List::pop wrapped their fresh, not-yet-rooted result in the
    // NON-pinning Value(vm, Handle) ctor, so a GC between the op and first use swept it (dangling
    // handle / ASan UAF). They now `adopting`-pin the result. Force GC on every allocation and use
    // each result AFTER many intervening allocations — under ASan this is the real gate. ===
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.setGcThreshold(1);                       // collect on every allocation
        auto churn = [&] { for (int i = 0; i < 64; ++i) vm.makeString("gc-churn-padding-string"); };

        Value a(vm, 2.5), b(vm, 4.0);
        Value sum = a + b;                          // fresh Float (A19-1)
        Value neg = -a;                             // fresh Float, unary (A19-1)
        churn();
        CHECK(sum.asFloat() == 6.5);                // swept-and-reused if unpinned
        CHECK(neg.asFloat() == -2.5);

        String s1(vm, "foo"), s2(vm, "bar");
        Value cat = s1 + s2;                        // fresh String concat
        churn();
        CHECK(cat.str() == "foobar");

        List xs(vm, {10, 20, 30});
        Value popped = xs.pop();                    // orphaned element (A19-2)
        churn();
        CHECK(popped.asInt() == 30);

        String hello(vm, "hello");
        Value ch = hello.at(1);                     // fresh 1-char String via getItem (A19-2)
        churn();
        CHECK(ch.str() == "e");
    }

    // === A08-5 (latent GC-safety): issubset/issuperset/isdisjoint built `otherSet` from a possibly
    // freshly-allocated iterable (String/Bytes/range chars) WITHOUT rooting it, unlike the union
    // family — a GC between adds could sweep a not-yet-added element. Now rooted. Force GC on every
    // allocation: the fresh 1-char String handles from "abcde" must survive into the subset check. ===
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.setGcThreshold(1);
        CHECK(vm.stringify(vm.runSource("String(Set([\"a\", \"b\"]).issubset(\"abcde\")) + "
            "String(Set([\"a\", \"z\"]).issubset(\"abcde\")) + "
            "String(Set([\"a\", \"b\", \"c\"]).issuperset(\"ab\")) + "
            "String(Set([\"x\", \"y\"]).isdisjoint(\"abcde\"))")) == "TrueFalseTrueTrue");
    }

    // === A08-4 (resource): an emptied hash bucket is now reclaimed on delete (else the bucket map
    // grows unbounded across delete-distinct-hash cycles). Correctness must survive the reclamation:
    // churn many distinct keys through add->remove->re-add and verify lookups stay consistent. ===
    CHECK(ok("var d = {}\nvar i = 0\nwhile i < 400:\n    d[i] = i\n    i = i + 1\n"
             "i = 0\nwhile i < 400:\n    discard d.pop(i)\n    i = i + 1\n"
             "d[42] = 99\nString(len(d)) + \",\" + String(d[42]) + \",\" + String(42 in d)") == "1,99,True");
    CHECK(ok("var s = Set()\nvar i = 0\nwhile i < 300:\n    s.add(i)\n    i = i + 1\n"
             "i = 0\nwhile i < 300:\n    s.remove(i)\n    i = i + 1\n"
             "s.add(7)\nString(len(s)) + \",\" + String(7 in s)") == "1,True");
    CHECK(ok("var s = Set([1, 2, 3])\ndiscard s.pop()\ndiscard s.pop()\ndiscard s.pop()\n"
             "s.add(9)\nString(len(s)) + \",\" + String(9 in s)") == "1,True");

    // === A07-1 (MEDIUM, latent UB): declaring a member Object::operator new HID the global aligned
    // allocation functions, so an over-aligned Object subclass would be routed through the 16-aligned
    // small-object pool and constructed under-aligned (UB, sanitizer-invisible). The aligned
    // operator new/delete overloads re-expose the aligned path. This test both COMPILES (the overload
    // exists) and checks it hands back correctly-aligned storage. ===
    {
        void* p = Object::operator new(64, std::align_val_t(32));
        CHECK((reinterpret_cast<std::uintptr_t>(p) % 32) == 0);
        Object::operator delete(p, std::align_val_t(32));
        void* q = Object::operator new(128, std::align_val_t(64));
        CHECK((reinterpret_cast<std::uintptr_t>(q) % 64) == 0);
        Object::operator delete(q, 128, std::align_val_t(64));
    }

    return RUN_TESTS();
}
