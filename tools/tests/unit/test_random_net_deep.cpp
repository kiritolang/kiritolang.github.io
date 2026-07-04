// test_random_net_deep.cpp — adversarial/edge coverage for `random` (pure, deterministic) and the
// pure `net` URL helpers. Server-dependent net paths (redirects, resolveUrl, Socket host resolution)
// are intentionally left to the net loopback harness — see NOTEs below.
#include <string>
#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string run(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}
static bool throws(KiritoVM& vm, const std::string& src) {
    try { vm.runSource(src); return false; }
    catch (...) { return true; }
}

int main() {
    KiritoVM vm;

    // ================= random (all deterministic under a fixed seed) =================

    // ---- distribution defaults: gauss()/expovariate() with fewer than the usual args ----
    CHECK(run(vm, "var random = import(\"random\")\nisinstance(random.Random(42).gauss(), Float)") == "True");
    CHECK(run(vm, "var random = import(\"random\")\nisinstance(random.Random(42).gauss(5), Float)") == "True");
    CHECK(run(vm, "var random = import(\"random\")\nisinstance(random.Random(42).expovariate(), Float)") == "True");
    // two identically-seeded generators agree (default gauss consumes the same draws)
    CHECK(run(vm, "var random = import(\"random\")\nrandom.Random(42).gauss() == random.Random(42).gauss()") == "True");

    // ---- under-arity guards throw ----
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).uniform(1)"));
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).sample([1, 2, 3])"));
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).randint(1)"));
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).seed()"));            // seed expects a value

    // ---- non-iterable / empty population guards throw ----
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).sample(5, 2)"));
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).choice(5)"));
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).choice([])"));

    // ---- resource / arity caps throw ----
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).choices([1, 2], k=300000000)"));  // k too large
    CHECK(throws(vm, "var random = import(\"random\")\nrandom.Random(42).randrange(1, 2, 3, 4)"));         // 1..3 args

    // ---- both engines: .generator name, and serialize round-trip reproduces the stream ----
    CHECK(run(vm, "var random = import(\"random\")\nrandom.Random(42, \"mersenne_twister\").generator") == "mersenne_twister");
    CHECK(run(vm, "var random = import(\"random\")\nrandom.Random(42).generator != \"mersenne_twister\"") == "True");  // default xoshiro
    CHECK(run(vm, R"KI(var random = import("random")
var serialize = import("serialize")
var r = random.Random(42)
discard r.randint(1, 1000000)
var r2 = serialize.loads(serialize.dumps(r))
r.randint(1, 1000000) == r2.randint(1, 1000000))KI") == "True");
    CHECK(run(vm, R"KI(var random = import("random")
var dump = import("dump")
var r = random.Random(99, "mersenne_twister")
discard r.random()
var r2 = dump.loads(dump.dumps(r))
r.random() == r2.random())KI") == "True");

    // ================= net URL helpers (pure — no server) =================

    // ---- quote / unquote round-trip and exact space encoding ----
    CHECK(run(vm, "var net = import(\"net\")\nnet.quote(\"a b\")") == "a%20b");
    CHECK(run(vm, "var net = import(\"net\")\nnet.unquote(\"a%20b\")") == "a b");
    CHECK(run(vm, "var net = import(\"net\")\nnet.unquote(net.quote(\"héllo world/?&=x\")) == \"héllo world/?&=x\"") == "True");

    // ---- urlencode of a Dict contains each field; parseqs recovers the key ----
    CHECK(run(vm, "var net = import(\"net\")\n\"a=1\" in net.urlencode({\"a\": 1})") == "True");
    CHECK(run(vm, "var net = import(\"net\")\n\"a\" in net.parseqs(\"a=1&b=2\")") == "True");

    // NOTE: HTTP redirect method-downgrade (301/302/303/307/308), resolveUrl absolute/bare-relative,
    // Socket.bind/connect host-resolution, and the TLS handshake path all require a live server and
    // belong in the net loopback/HTTPS harness (tools/tests/unit/test_net.cpp), not this pure suite.

    return RUN_TESTS();
}
