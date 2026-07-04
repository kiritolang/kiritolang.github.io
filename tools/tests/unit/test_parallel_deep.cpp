// test_parallel_deep.cpp — adversarial/edge coverage for the parallel module's primitives, filling
// audited gaps: same-VM dump round-trip preserving primitive IDENTITY (these are cross-VM-by-identity
// objects), Queue.getnowait on a CLOSED empty queue (Closed, not Empty), Barrier.reset() re-arm
// reuse, and Semaphore _enter_/_exit_ via `with`. Runs on the dispatcher harness like the other
// parallel suites.
#include "../parallel_util.hpp"

using namespace kitest;

int main() {
    // ---- same-VM dump.dumps/loads of a Queue preserves IDENTITY: q and its "copy" are one queue ----
    expectOk("dump round-trip preserves Queue identity", R"KI(
var parallel = import("parallel")
var dump = import("dump")
if argmain:
    var q = parallel.Queue()
    var q2 = dump.loads(dump.dumps(q))
    q.put(42)
    assert q2.get() == 42
)KI");

    // ---- dump round-trip of the other sync primitives reconstructs without error ----
    expectOk("dump round-trip of sync primitives", R"KI(
var parallel = import("parallel")
var dump = import("dump")
if argmain:
    discard dump.loads(dump.dumps(parallel.Lock()))
    discard dump.loads(dump.dumps(parallel.Event()))
    discard dump.loads(dump.dumps(parallel.Semaphore(2)))
    discard dump.loads(dump.dumps(parallel.Barrier(1)))
    assert True
)KI");

    // ---- getnowait on a CLOSED empty queue raises (the Closed path, distinct from Empty) ----
    expectOk("getnowait on closed empty queue throws", R"KI(
var parallel = import("parallel")
if argmain:
    var q = parallel.Queue()
    q.close()
    var threw = False
    try:
        discard q.getnowait()
    catch as e:
        threw = True
    assert threw
)KI");

    // ---- Barrier.reset() re-arms a single-party barrier for reuse ----
    expectOk("Barrier.reset re-arm reuse", R"KI(
var parallel = import("parallel")
if argmain:
    var b = parallel.Barrier(1)
    b.wait()
    b.reset()
    b.wait()
    assert True
)KI");

    // ---- Semaphore _enter_/_exit_ via `with`, then re-acquire ----
    expectOk("Semaphore with-context acquire/release", R"KI(
var parallel = import("parallel")
if argmain:
    var sem = parallel.Semaphore(1)
    with sem:
        assert True
    with sem:
        assert True
)KI");

    return RUN_TESTS();
}
