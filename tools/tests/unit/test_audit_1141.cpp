// test_audit_1141.cpp — regression tests for the v1.14.1 audit fixes that live in the C++ core /
// embedding API (the Kirito-reachable ones are pinned in scripts/spec_audit_1141.ki):
//   A06-2 / A04-X1  io stream + BytesIO/Random/hash native setup must not dangle under GC pressure
//   A09-1           a signatured native's heap default (hash.hmac's algo="sha256") survives its alloc
//   A09-2           Value::isBytes/asBytes discriminate the native Bytes from a user `class Bytes`
//   A09-3           Value::call(initializer_list) roots its argument handles across the call
//   A06-1           the active-VM thread-local tolerates non-LIFO multi-VM teardown
//   A13-1/2/3       net Host header (port + IPv6) and protocol-relative redirect resolution
#include <memory>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static bool runs(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src, "<t>"); return true; }
    catch (...) { return false; }
}

int main() {
    // ---- A06-2 / A04-X1 / A09-1: native module setup + signatured-default under collect-on-every-alloc.
    // Before the fix, io setup stored dangling stdout/stderr/stdin handles and hash.hmac's "sha256"
    // default was swept during the NativeFunction alloc, so these threw "dangling handle". ----
    {
        KiritoVM vm;
        vm.setGcThreshold(1);                              // collect on EVERY allocation
        // redirect output into an in-memory buffer (also exercises BytesIO's swept setup default arg)
        CHECK(runs(vm, "var io = import(\"io\")\n"
                       "io.stdout = io.BytesIO()\n"
                       "io.print(\"hi\")\n"
                       "io.write(\"x\")\n"
                       "io.eprint(\"e\")"));
        Handle mac{};
        bool hmacOk = true;
        try { mac = vm.runSource("var h = import(\"hash\")\nh.hmac(\"key\", \"msg\")"); }
        catch (...) { hmacOk = false; }
        CHECK(hmacOk);                                     // A09-1: kw-omitted default resolved, not dangling
        CHECK(hmacOk && Value(vm, mac).isString());
        CHECK(runs(vm, "var r = import(\"random\")\nvar g = r.Random(42)\ndiscard g.randint(1, 10)"));
    }

    // ---- A09-2: a user `class Bytes` must NOT be mistaken for the native Bytes value (was a SEGV: a
    // static_cast to BytesVal then operator[]). ----
    {
        KiritoVM vm;
        Handle real = vm.runSource("Bytes([1, 2, 3])");
        CHECK(Value(vm, real).isBytes());                 // the genuine native Bytes
        Handle fake = vm.runSource("class Bytes:\n"
                                   "    var _init_ = Function(self):\n"
                                   "        self.x = 1\n"
                                   "Bytes()");
        CHECK(!Value(vm, fake).isBytes());                // a user class named Bytes is not native Bytes
        CHECK_THROWS(Value(vm, fake).asBytes("t"));       // asBytes rejects it instead of crashing
    }

    // ---- A09-3: Value::call(initializer_list) roots its arg handles across the call (non-interned
    // ints, collect-on-every-alloc). ----
    {
        KiritoVM vm;
        vm.setGcThreshold(1);
        Handle fnH = vm.runSource("Function(a, b):\n    return a + b");
        Value f(vm, fnH);
        Value r = f.call({100000, 200000});               // > small-int intern range, freshly allocated
        CHECK(r.asInt() == 300000);
    }

    // ---- A06-1: destroying VMs out of construction order must leave activeVM() at a live VM or null,
    // never a dangling pointer to a freed VM. ----
    {
        auto a = std::make_unique<KiritoVM>();
        auto b = std::make_unique<KiritoVM>();
        CHECK(KiritoVM::activeVM() == b.get());
        a.reset();                                        // destroy the OUTER vm first (non-LIFO)
        CHECK(KiritoVM::activeVM() == b.get());           // b is still active (was dangling before)
        b.reset();
        CHECK(KiritoVM::activeVM() == nullptr);
    }

    // ---- A13-1/2: the HTTP Host header carries a non-default port and brackets an IPv6 literal. ----
    {
        net::Url u; u.host = "example.com"; u.port = 80; u.tls = false;
        CHECK(net::hostHeader(u) == "example.com");       // default http port omitted
        u.port = 8137;
        CHECK(net::hostHeader(u) == "example.com:8137");  // non-default port included
        net::Url v; v.host = "::1"; v.port = 443; v.tls = true;
        CHECK(net::hostHeader(v) == "[::1]");             // IPv6 bracketed, default https port omitted
        v.port = 8080;
        CHECK(net::hostHeader(v) == "[::1]:8080");        // IPv6 bracketed + non-default port
    }

    // ---- A13-3: a protocol-relative redirect adopts the base scheme and replaces the host. ----
    {
        CHECK(net::resolveUrl("http://orig.example/a/b", "//cdn.example/x") == "http://cdn.example/x");
        CHECK(net::resolveUrl("https://orig.example/a/b", "//cdn.example/x") == "https://cdn.example/x");
        CHECK(net::resolveUrl("http://orig.example/a/b", "/root") == "http://orig.example/root");
        CHECK(net::resolveUrl("http://orig.example/a/b", "https://x.example/y") == "https://x.example/y");
    }

    if (kitest::failures == 0) std::printf("test_audit_1141: all passed\n");
    return RUN_TESTS();
}
