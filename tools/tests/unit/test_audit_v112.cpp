// Regression tests for the v1.12 hardening pass (audit_v1_12/FINDINGS.md). Each block pins one
// confirmed finding so it can never silently return. Run under -fsanitize=address,undefined — several
// of these were UBSan/ASan-confirmed crashes, so the sanitizer build is the real gate.
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
    // === A07-1: InstanceValue::equals must not downcast a NATIVE value that also reports
    // ValueKind::Instance. `Bytes("") in [C()]` makes List::contains call C-instance.equals(Bytes)
    // (List uses raw Object::equals, no hashing) — previously a static_cast<InstanceValue&>(Bytes)
    // read a garbage Handle (UBSan-confirmed). Must return False cleanly, no crash. ===
    CHECK(ok("class C:\n  var _eq_ = Function(self, o): return False\n"
             "Bytes(\"\") in [C()]") == "False");
    // A user instance and a native Bytes coexisting as Dict keys must not corrupt the container.
    CHECK(ok("class C:\n  var _hash_ = Function(self): return 0\n"
             "  var _eq_ = Function(self, o): return False\n"
             "var d = {}\nd[C()] = 1\nd[Bytes(\"\")] = 2\nlen(d)") == "2");

    // === A09-1: modular pow must not overflow int64 during `(base % mod) + mod`. With mod just under
    // 2^63, base ≡ -1, so base^2 ≡ 1 — the buggy int64 path returned 9. ===
    CHECK(ok("pow(9223372036854775806, 2, 9223372036854775807)") == "1");
    CHECK(ok("pow(9223372036854775805, 2, 9223372036854775807)") == "4");  // (-2)^2 = 4
    CHECK(ok("pow(7, 0, 9223372036854775807)") == "1");
    CHECK(has(err("pow(2, 10, 0)"), "modulus"));       // guards still fire
    CHECK(has(err("pow(2, -1, 5)"), "non-negative"));

    // === M1 (perf): the DEFAULT (adaptive) GC threshold still reclaims. 300k short-lived allocations
    // with no explicit setGcThreshold must keep the live set small (collections run + retarget). ===
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.runSource("var i = 0\nwhile i < 300000:\n  var t = [i, i + 1]\n  i = i + 1\n");
        CHECK(vm.liveCount() < 100000);
    }
    // setGcThreshold still pins exactly (adaptive off) — collect-on-every-alloc stays honored and
    // the built container survives the aggressive GC without a dangling handle.
    {
        KiritoVM vm;
        vm.installStandardLibrary();
        vm.setGcThreshold(1);
        CHECK(vm.stringify(vm.runSource(
            "var xs = []\nvar i = 0\nwhile i < 500:\n  xs.append([i])\n  i = i + 1\nlen(xs)")) == "500");
    }

    return RUN_TESTS();
}
