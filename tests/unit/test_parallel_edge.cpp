// Edge cases: non-serializable values rejected at the queue boundary, bounded-queue mechanics,
// empty/full nowait errors, and deterministic close behaviour — all through the Kirito surface.

#include "../parallel_util.hpp"

using namespace kitest;

int main() {
    // A live resource (Socket) can't cross a Queue: serialization throws cleanly instead of corrupting
    // the receiving arena.
    expectOk("non-serializable socket rejected", R"KI(
var parallel = import("parallel")
var net = import("net")
if argmain:
    var q = parallel.Queue()
    var s = net.Socket()
    var caught = False
    try:
        q.put(s)
    catch as e:
        caught = True
    assert caught
    s.close()
)KI");

    // A Task is not serializable either (no _getstate_).
    expectOk("non-serializable task rejected", R"KI(
var parallel = import("parallel")
var noop = Function():
    return 1
if argmain:
    var q = parallel.Queue()
    var t = parallel.spawn(noop)
    var caught = False
    try:
        q.put(t)
    catch as e:
        caught = True
    assert caught
    discard t.join()
)KI");

    // Bounded queue mechanics: full/empty/qsize, putnowait-on-full and getnowait-on-empty throw, and
    // capacity frees up after a get.
    expectOk("bounded queue mechanics", R"KI(
var parallel = import("parallel")
if argmain:
    var q = parallel.Queue(1)
    assert q.empty()
    q.put(10)
    assert q.full()
    assert q.qsize() == 1
    var caught = False
    try:
        q.putnowait(20)
    catch as e:
        caught = True
    assert caught
    assert q.get() == 10
    assert q.empty()
    q.put(20)
    assert q.get() == 20
)KI");

    // get/timeout and getnowait on an empty queue throw (no hang).
    expectError("getnowait empty throws", R"KI(
var parallel = import("parallel")
if argmain:
    var q = parallel.Queue()
    discard q.getnowait()
)KI",
                "empty");

    expectError("get timeout throws", R"KI(
var parallel = import("parallel")
if argmain:
    var q = parallel.Queue()
    discard q.get(True, 0.05)
)KI",
                "empty");

    // A closed, drained queue throws on get (the worker-loop termination signal).
    expectError("get on closed throws", R"KI(
var parallel = import("parallel")
if argmain:
    var q = parallel.Queue()
    q.put(1)
    q.close()
    assert q.get() == 1
    discard q.get()
)KI",
                "closed");

    // spawn rejects a non-Kirito-file function (e.g. a builtin) clearly.
    expectError("spawn rejects native fn", R"KI(
var parallel = import("parallel")
if argmain:
    discard parallel.spawn(len, [1, 2, 3])
)KI",
                "function");

    return RUN_TESTS();
}
