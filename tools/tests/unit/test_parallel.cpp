// Typical multiprocessing behaviour: spawn/join, keyword forwarding, Queue value round-trips,
// cross-VM Queue identity, cpucount, and the bare-VM "no parallel module" contract.

#include "../parallel_util.hpp"

using namespace kitest;

int main() {
    // spawn a top-level function; join returns its (serialized-then-rebuilt) value. Keyword + default
    // arguments forward through spawn.
    expectOk("spawn join kwargs", R"KI(
var parallel = import("parallel")
var work = Function(a, b = 100):
    return a * 10 + b
if argmain:
    assert parallel.spawn(work, 3, 4).join() == 34
    assert parallel.spawn(work, a = 5).join() == 150
    assert parallel.spawn(work, b = 7, a = 2).join() == 27
)KI");

    // done() is true after a join completes; a returned None round-trips.
    expectOk("spawn done none", R"KI(
var parallel = import("parallel")
var nothing = Function():
    return None
if argmain:
    var t = parallel.spawn(nothing)
    assert t.join() == None
    assert t.done() == True
)KI");

    // A nested value graph survives put/get through a Queue (serialize -> deserialize).
    expectOk("queue value graph", R"KI(
var parallel = import("parallel")
if argmain:
    var q = parallel.Queue()
    q.put([1, "two", {"k": [3, 4]}, True])
    var x = q.get()
    assert x[1] == "two"
    assert x[2]["k"][1] == 4
    assert x[3] == True
)KI");

    // A Queue passed to a worker references the SAME underlying queue: the worker produces, main
    // consumes; close() ends the consume loop.
    expectOk("cross-vm queue identity", R"KI(
var parallel = import("parallel")
var producer = Function(out, n):
    var i = 0
    while i < n:
        out.put(i)
        i = i + 1
    out.close()
if argmain:
    var q = parallel.Queue()
    var t = parallel.spawn(producer, q, 5)
    var total = 0
    var running = True
    while running:
        try:
            total = total + q.get()
        catch as e:
            running = False
    t.join()
    assert total == 10
)KI");

    // Fan-out/fan-in: K workers each return a partial; the sum matches the serial baseline.
    expectOk("fan-out join", R"KI(
var parallel = import("parallel")
var square = Function(x):
    return x * x
if argmain:
    var tasks = []
    var i = 1
    while i <= 6:
        tasks.append(parallel.spawn(square, i))
        i = i + 1
    var total = 0
    for t in tasks:
        total = total + t.join()
    assert total == 91
)KI");

    expectOk("cpucount", R"KI(
var parallel = import("parallel")
if argmain:
    assert parallel.cpucount() >= 1
)KI");

    // --- adversarial: worker faults are isolated and reported, never crash the host ---

    // A worker that throws: join re-throws it, and the host VM keeps working.
    expectOk("worker throw propagates", R"KI(
var parallel = import("parallel")
var boom = Function():
    throw "kaboom"
if argmain:
    var t = parallel.spawn(boom)
    var caught = False
    try:
        discard t.join()
    catch as e:
        caught = True
    assert caught
    assert 1 + 1 == 2
)KI");

    // A worker that blows the recursion guard is captured as an error, surfaced by join (no crash).
    expectOk("worker recursion guard", R"KI(
var parallel = import("parallel")
var recurse = Function(n):
    return recurse(n + 1)
if argmain:
    var caught = False
    try:
        discard parallel.spawn(recurse, 0).join()
    catch as e:
        caught = True
    assert caught
)KI");

    // Many short-lived spawn+join cycles (ASan watches for leaks across the churn).
    expectOk("spawn churn", R"KI(
var parallel = import("parallel")
var inc = Function(x):
    return x + 1
if argmain:
    var i = 0
    var total = 0
    while i < 100:
        total = total + parallel.spawn(inc, i).join()
        i = i + 1
    assert total == 5050
)KI");

    // A bare embedded KiritoVM (no dispatcher) has no `parallel` module at all — multiprocessing is a
    // dispatcher-provided capability. Importing it throws clearly.
    {
        kirito::KiritoVM vm;
        std::string msg;
        try {
            vm.runSource("import(\"parallel\")");
        } catch (const kirito::KiritoError& e) {
            msg = e.what();
        }
        CHECK(!msg.empty());
        CHECK(msg.find("parallel") != std::string::npos);
    }

    return RUN_TESTS();
}
