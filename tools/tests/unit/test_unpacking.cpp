// Packing and unpacking: multiple assignment, multiple return values, starred targets, and
// iterable unpacking in `var`, plain assignment, and `for`. A bare comma sequence packs into a List.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static std::string err(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return ""; }
    catch (const KiritoError& e) { return e.what(); }
}

int main() {
    // --- packing: a comma sequence is a List ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var t = 1, 2, 3\nString(t)") == "[1, 2, 3]");
        CHECK(run(vm, "var t = 1, 2, 3\nlen(t)") == "3");
    }

    // --- basic multiple assignment (var and rebind) ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var a, b = 1, 2\nString(a) + String(b)") == "12");
        CHECK(run(vm, "var a = 0\nvar b = 0\na, b = 5, 9\nString(a) + String(b)") == "59");
    }

    // --- swap without a temporary ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var a = 1\nvar b = 2\na, b = b, a\nString(a) + String(b)") == "21");
    }

    // --- multiple return values ---
    {
        KiritoVM vm;
        CHECK(run(vm,
            "var divmod = Function(x, y):\n    return x // y, x % y\n"
            "var q, r = divmod(17, 5)\nString(q) + \" \" + String(r)") == "3 2");
        // a multi-return is itself a List the caller can keep whole
        CHECK(run(vm, "var f = Function():\n    return 1, 2\nString(f())") == "[1, 2]");
    }

    // --- unpack from an existing list / string ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var p = [10, 20, 30]\nvar x, y, z = p\nString(x) + String(y) + String(z)") == "102030");
        CHECK(run(vm, "var a, b, c = \"xyz\"\na + b + c") == "xyz");
    }

    // --- starred targets ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var first, *rest = [1, 2, 3, 4]\nString(first) + \" \" + String(rest)") == "1 [2, 3, 4]");
        CHECK(run(vm, "var head, *mid, tail = [1, 2, 3, 4, 5]\nString(mid)") == "[2, 3, 4]");
        CHECK(run(vm, "var head, *mid, tail = [1, 2, 3, 4, 5]\nString(head) + String(tail)") == "15");
        CHECK(run(vm, "var *init, last = [1, 2, 3]\nString(init) + \" \" + String(last)") == "[1, 2] 3");
        // star can absorb zero items
        CHECK(run(vm, "var a, *rest = [1]\nString(rest)") == "[]");
    }

    // --- for-loop unpacking ---
    {
        KiritoVM vm;
        CHECK(run(vm,
            "var total = 0\nfor i, n in [[1, 10], [2, 20], [3, 30]]:\n    total = total + i * n\ntotal")
            == "140");
        // (dict iteration is hash-ordered, so sum the values rather than assume key order)
        CHECK(run(vm,
            "var total = 0\nfor k, v in {\"a\": 1, \"b\": 2, \"c\": 3}.items():\n    total = total + v\ntotal")
            == "6");
        // starred loop targets
        CHECK(run(vm,
            "var out = []\nfor first, *others in [[1, 2, 3], [4, 5, 6]]:\n"
            "    out.append(String(first) + \"/\" + String(others))\n\", \".join(out)")
            == "1/[2, 3], 4/[5, 6]");
    }

    // --- assignment targets can be index/member, not just names ---
    {
        KiritoVM vm;
        CHECK(run(vm, "var a = [0, 0]\na[0], a[1] = 7, 8\nString(a)") == "[7, 8]");
    }

    // --- error cases (clean, catchable) ---
    {
        KiritoVM vm;
        CHECK(err(vm, "var a, b = [1, 2, 3]").find("expected 2 values to unpack, got 3") != std::string::npos);
        CHECK(err(vm, "var a, b, c = [1, 2]").find("expected 3 values to unpack, got 2") != std::string::npos);
        CHECK(err(vm, "var a, *b, c = [1]").find("expected at least 2 values to unpack, got 1") != std::string::npos);
        CHECK(err(vm, "var a, b = 5").find("'Integer' is not iterable") != std::string::npos);
        CHECK(err(vm, "var a, *b, *c = [1, 2, 3]").find("two starred targets") != std::string::npos);
    }

    return RUN_TESTS();
}
