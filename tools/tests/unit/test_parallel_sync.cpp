// Synchronization primitives: Lock / Event / Semaphore / Barrier. Mostly exercised at the C++ level
// with std::threads for deterministic, race-meaningful assertions (genuine mutual exclusion, a peak
// concurrency bound, a rendezvous), plus one end-to-end Kirito scenario for the module wiring.
// All assertions happen on the main thread; spawned threads only touch atomics / lock-protected state.

#include <atomic>
#include <thread>
#include <vector>

#include "../parallel_util.hpp"

using namespace kitest;
using namespace kirito;

int main() {
    // Lock — genuine mutual exclusion: many threads increment a plain int guarded by the lock; the
    // final value is exact (without the lock it would race / be flagged by TSan).
    noDeadlock("lock mutual exclusion", 20.0, [] {
        KiritoDispatcher disp;
        auto lock = disp.createLock();
        long counter = 0;
        const int N = 8, iters = 2000;
        std::vector<std::thread> ts;
        for (int i = 0; i < N; ++i)
            ts.emplace_back([&] {
                for (int k = 0; k < iters; ++k) {
                    lock->acquire(true, std::nullopt);
                    ++counter;
                    lock->release();
                }
            });
        for (auto& t : ts) t.join();
        CHECK(counter == static_cast<long>(N) * iters);
    });

    // Lock — non-reentrant (same thread re-acquire reported, never a self-deadlock), and held => a
    // different thread's non-blocking acquire fails until release.
    noDeadlock("lock reentrant + contended", 20.0, [] {
        KiritoDispatcher disp;
        auto lock = disp.createLock();
        CHECK(lock->acquire(true, std::nullopt) == WaitResult::Ok);
        CHECK(lock->acquire(false, std::nullopt) == WaitResult::Reentrant);  // same thread
        std::atomic<int> other{-1};
        std::thread t([&] { other = static_cast<int>(lock->acquire(false, std::nullopt)); });
        t.join();
        CHECK(other.load() == static_cast<int>(WaitResult::TimedOut));  // held by main
        CHECK(lock->release() == true);
        CHECK(lock->release() == false);  // releasing an unheld lock reports false
    });

    // Event — broadcast: M waiters all proceed once set; an unset wait times out, a set wait returns.
    noDeadlock("event broadcast", 20.0, [] {
        KiritoDispatcher disp;
        auto ev = disp.createEvent();
        std::atomic<int> proceeded{0};
        const int M = 6;
        std::vector<std::thread> ts;
        for (int i = 0; i < M; ++i)
            ts.emplace_back([&] {
                if (ev->wait(std::nullopt) == WaitResult::Ok) ++proceeded;
            });
        ev->set();
        for (auto& t : ts) t.join();
        CHECK(proceeded.load() == M);

        auto ev2 = disp.createEvent();
        CHECK(ev2->wait(0.05) == WaitResult::TimedOut);
        ev2->set();
        CHECK(ev2->wait(0.05) == WaitResult::Ok);
        ev2->clear();
        CHECK(ev2->wait(0.05) == WaitResult::TimedOut);  // resettable
    });

    // Semaphore(k) — at most k holders at once. Track peak concurrency; assert it never exceeds k.
    noDeadlock("semaphore bound", 20.0, [] {
        KiritoDispatcher disp;
        const int k = 3, M = 12;
        auto sem = disp.createSemaphore(k);
        std::atomic<int> active{0}, peak{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < M; ++i)
            ts.emplace_back([&] {
                for (int r = 0; r < 50; ++r) {
                    CHECK(sem->acquire(true, std::nullopt) == WaitResult::Ok);
                    int a = ++active;
                    int prev = peak.load();
                    while (a > prev && !peak.compare_exchange_weak(prev, a)) { /* retry */ }
                    std::this_thread::yield();
                    --active;
                    sem->release();
                }
            });
        for (auto& t : ts) t.join();
        CHECK(peak.load() <= k);
        CHECK(peak.load() >= 1);
        CHECK(active.load() == 0);
    });

    // Barrier(n) — n threads rendezvous; the arrival indices are exactly a permutation of 0..n-1
    // (everyone arrived and was released together).
    noDeadlock("barrier rendezvous", 20.0, [] {
        KiritoDispatcher disp;
        const int n = 5;
        auto bar = disp.createBarrier(n);
        std::vector<int64_t> indices(static_cast<std::size_t>(n), -1);
        std::vector<std::thread> ts;
        for (int i = 0; i < n; ++i)
            ts.emplace_back([&, i] {
                int64_t idx = -1;
                if (bar->wait(std::nullopt, idx) == WaitResult::Ok) indices[static_cast<std::size_t>(i)] = idx;
            });
        for (auto& t : ts) t.join();
        std::vector<int64_t> sorted = indices;
        std::sort(sorted.begin(), sorted.end());
        for (int i = 0; i < n; ++i) CHECK(sorted[static_cast<std::size_t>(i)] == i);
    });

    // Barrier — a timed-out wait breaks the barrier (only one of two parties shows up).
    noDeadlock("barrier broken on timeout", 20.0, [] {
        KiritoDispatcher disp;
        auto bar = disp.createBarrier(2);
        int64_t idx = -1;
        CHECK(bar->wait(0.05, idx) == WaitResult::Broken);
    });

    // Barrier — REUSABLE after a timeout: the timed-out waiter must reset the generation's count, so a
    // fresh, complete set of parties rendezvous cleanly. With a stale count a single later arrival
    // would trip "last party" off the departed waiter and self-heal the barrier at the wrong moment.
    noDeadlock("barrier reusable after a timeout", 20.0, [] {
        KiritoDispatcher disp;
        auto bar = disp.createBarrier(2);
        int64_t idx = -1;
        CHECK(bar->wait(0.05, idx) == WaitResult::Broken);   // one party, times out -> breaks + resets
        std::atomic<int> okCount{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 2; ++i)
            ts.emplace_back([&] {
                int64_t j = -1;
                if (bar->wait(5.0, j) == WaitResult::Ok) ++okCount;
            });
        for (auto& t : ts) t.join();
        CHECK(okCount.load() == 2);   // both fresh parties released together (buggy: 0)
    });

    // Semaphore — UNBOUNDED by design: over-releasing grows the permit count (a plain counting
    // semaphore, not a BoundedSemaphore), so a later burst of acquires all succeed.
    noDeadlock("semaphore over-release grows permits", 20.0, [] {
        KiritoDispatcher disp;
        auto sem = disp.createSemaphore(1);
        for (int i = 0; i < 4; ++i) sem->release();                        // 1 + 4 == 5 permits
        for (int i = 0; i < 5; ++i) CHECK(sem->acquire(false, std::nullopt) == WaitResult::Ok);
        CHECK(sem->acquire(false, std::nullopt) == WaitResult::TimedOut);  // and no more
    });

    // Lock — released BY IDENTITY, not by owning thread: a lock acquired on one thread can be released
    // from another (the transferable-handoff pattern the parallel tests rely on).
    noDeadlock("lock cross-thread release", 20.0, [] {
        KiritoDispatcher disp;
        auto lock = disp.createLock();
        std::thread t([&] { CHECK(lock->acquire(true, std::nullopt) == WaitResult::Ok); });  // held by worker
        t.join();
        CHECK(lock->locked() == true);
        CHECK(lock->release() == true);   // a DIFFERENT thread (main) releases it
        CHECK(lock->locked() == false);
    });

    // End-to-end through Kirito: workers wait on a shared Event, then report via a shared Queue.
    expectOk("event + queue (kirito)", R"KI(
var parallel = import("parallel")
var worker = Function(ev, out):
    ev.wait()
    out.put(1)
if argmain:
    var ev = parallel.Event()
    var q = parallel.Queue()
    var i = 0
    while i < 4:
        discard parallel.spawn(worker, ev, q)
        i = i + 1
    ev.set()
    var got = 0
    while got < 4:
        discard q.get()
        got = got + 1
    assert got == 4
)KI");

    return RUN_TESTS();
}
