// Deadlock resistance — the emphasis. Every case runs under noDeadlock(): if the scenario hangs the
// watchdog hard-exits with a clear FAIL instead of stalling CI forever. These verify the core
// invariant (shutdown aborts EVERY blocked primitive before joining), bounded back-pressure,
// close/drain semantics, that timeouts never block unboundedly, and a producer/consumer stress run.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "../parallel_util.hpp"

using namespace kitest;
using namespace kirito;

int main() {
    // shutdown() wakes a worker blocked in ANY primitive (the abort-on-shutdown contract). A raw
    // thread blocks; shutdown() aborts; the op returns Aborted and the thread exits within budget.
    noDeadlock("shutdown wakes blocked queue.get", 20.0, [] {
        KiritoDispatcher disp;
        auto q = disp.createQueue(0);
        std::atomic<int> r{-1};
        std::thread t([&] { std::string out; r = static_cast<int>(q->get(true, std::nullopt, out)); });
        disp.shutdown();
        t.join();
        CHECK(r.load() == static_cast<int>(WaitResult::Aborted));
    });
    noDeadlock("shutdown wakes blocked lock.acquire", 20.0, [] {
        KiritoDispatcher disp;
        auto lock = disp.createLock();
        lock->acquire(true, std::nullopt);  // main holds it; the thread will block
        std::atomic<int> r{-1};
        std::thread t([&] { r = static_cast<int>(lock->acquire(true, std::nullopt)); });
        disp.shutdown();
        t.join();
        CHECK(r.load() == static_cast<int>(WaitResult::Aborted));
    });
    noDeadlock("shutdown wakes blocked event.wait", 20.0, [] {
        KiritoDispatcher disp;
        auto ev = disp.createEvent();
        std::atomic<int> r{-1};
        std::thread t([&] { r = static_cast<int>(ev->wait(std::nullopt)); });
        disp.shutdown();
        t.join();
        CHECK(r.load() == static_cast<int>(WaitResult::Aborted));
    });
    noDeadlock("shutdown wakes blocked semaphore.acquire", 20.0, [] {
        KiritoDispatcher disp;
        auto sem = disp.createSemaphore(0);  // no permits => acquire blocks
        std::atomic<int> r{-1};
        std::thread t([&] { r = static_cast<int>(sem->acquire(true, std::nullopt)); });
        disp.shutdown();
        t.join();
        CHECK(r.load() == static_cast<int>(WaitResult::Aborted));
    });
    noDeadlock("shutdown wakes blocked barrier.wait", 20.0, [] {
        KiritoDispatcher disp;
        auto bar = disp.createBarrier(2);  // only one party arrives => blocks
        std::atomic<int> r{-1};
        std::thread t([&] { int64_t idx = -1; r = static_cast<int>(bar->wait(std::nullopt, idx)); });
        disp.shutdown();
        t.join();
        CHECK(r.load() == static_cast<int>(WaitResult::Aborted));
    });

    // The dispatcher destructor must complete even with workers blocked on a primitive (it shuts down
    // first). The raw threads hold their own shared_ptr to the queue, so it outlives the dispatcher.
    noDeadlock("dtor with live blocked workers", 20.0, [] {
        std::vector<std::thread> ts;
        std::atomic<int> completed{0};
        std::shared_ptr<ConcurrentQueue> q;
        {
            KiritoDispatcher disp;
            q = disp.createQueue(0);
            for (int i = 0; i < 4; ++i)
                ts.emplace_back([&] {
                    std::string out;
                    q->get(true, std::nullopt, out);
                    ++completed;
                });
        }  // ~KiritoDispatcher aborts the queue here, unblocking the gets
        for (auto& t : ts) t.join();
        CHECK(completed.load() == 4);
    });

    // Nested spawn+join: a worker spawns and joins a sub-task while the parent joins it. Proves the
    // registry mutex is never held across a join.
    expectOk("nested spawn+join", R"KI(
var parallel = import("parallel")
var leaf = Function(x):
    return x + 1
var middle = Function(x):
    return parallel.spawn(leaf, x).join() * 2
if argmain:
    assert parallel.spawn(middle, 10).join() == 22
)KI");

    // Bounded back-pressure: a fast producer + slow-started consumer over a capacity-1 queue still
    // delivers every item in order (the producer blocks on full; nothing is lost; no hang).
    noDeadlock("bounded back-pressure", 20.0, [] {
        KiritoDispatcher disp;
        auto q = disp.createQueue(1);
        const int N = 300;
        std::vector<int> received;
        std::thread consumer([&] {
            for (int i = 0; i < N; ++i) {
                std::string out;
                if (q->get(true, std::nullopt, out) == WaitResult::Ok) received.push_back(std::stoi(out));
            }
        });
        for (int i = 0; i < N; ++i) q->put(std::to_string(i), true, std::nullopt);
        consumer.join();
        CHECK(static_cast<int>(received.size()) == N);
        bool ordered = true;
        for (int i = 0; i < N; ++i) if (received[static_cast<std::size_t>(i)] != i) ordered = false;
        CHECK(ordered);
    });

    // close() then drain: a closed queue yields the remaining items, then reports Closed (never hangs
    // waiting for more).
    noDeadlock("close then drain", 20.0, [] {
        KiritoDispatcher disp;
        auto q = disp.createQueue(0);
        for (int i = 0; i < 5; ++i) q->put(std::to_string(i), true, std::nullopt);
        q->close();
        int count = 0;
        std::string out;
        while (q->get(true, std::nullopt, out) == WaitResult::Ok) ++count;
        CHECK(count == 5);
        CHECK(q->get(true, std::nullopt, out) == WaitResult::Closed);
        CHECK(q->put("x", true, std::nullopt) == WaitResult::Closed);  // put on closed -> Closed
    });

    // A full bounded queue: non-blocking and timed puts fail fast (never hang).
    noDeadlock("full queue fails fast", 20.0, [] {
        KiritoDispatcher disp;
        auto q = disp.createQueue(2);
        CHECK(q->put("a", true, std::nullopt) == WaitResult::Ok);
        CHECK(q->put("b", true, std::nullopt) == WaitResult::Ok);
        CHECK(q->put("c", false, std::nullopt) == WaitResult::TimedOut);  // nonblocking, full
        CHECK(q->put("c", true, 0.05) == WaitResult::TimedOut);            // timed, full
    });

    // Every timed blocking op returns within ~the timeout on an ever-blocked condition.
    noDeadlock("timeouts never hang", 20.0, [] {
        KiritoDispatcher disp;
        auto q = disp.createQueue(0);
        std::string out;
        CHECK(q->get(true, 0.05, out) == WaitResult::TimedOut);
        auto lock = disp.createLock();
        lock->acquire(true, std::nullopt);
        std::atomic<int> lr{-1};
        std::thread t([&] { lr = static_cast<int>(lock->acquire(true, 0.05)); });
        t.join();
        CHECK(lr.load() == static_cast<int>(WaitResult::TimedOut));
        CHECK(disp.createEvent()->wait(0.05) == WaitResult::TimedOut);
        CHECK(disp.createSemaphore(0)->acquire(true, 0.05) == WaitResult::TimedOut);
        int64_t idx = -1;
        CHECK(disp.createBarrier(2)->wait(0.05, idx) == WaitResult::Broken);
    });

    // Producer/consumer stress: P producers + C consumers + a bounded queue. The consumed multiset
    // equals the produced one and every thread joins (shakes out lost wakeups / missed notifications).
    noDeadlock("producer/consumer stress", 30.0, [] {
        KiritoDispatcher disp;
        auto q = disp.createQueue(8);
        const long P = 4, C = 4, perProd = 800;
        std::atomic<long> consumed{0}, sum{0};
        std::vector<std::thread> prods, cons;
        for (long p = 0; p < P; ++p)
            prods.emplace_back([&, p] {
                for (long i = 0; i < perProd; ++i)
                    q->put(std::to_string(p * perProd + i), true, std::nullopt);
            });
        for (long c = 0; c < C; ++c)
            cons.emplace_back([&] {
                std::string out;
                while (q->get(true, std::nullopt, out) == WaitResult::Ok) {
                    ++consumed;
                    sum += std::stol(out);
                }
            });
        for (auto& t : prods) t.join();
        q->close();  // producers done => consumers drain remaining, then see Closed and exit
        for (auto& t : cons) t.join();
        long total = P * perProd;
        CHECK(consumed.load() == total);
        CHECK(sum.load() == total * (total - 1) / 2);  // sum of 0 .. total-1
    });

    return RUN_TESTS();
}
