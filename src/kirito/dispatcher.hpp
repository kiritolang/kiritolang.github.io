#ifndef KIRITO_DISPATCHER_HPP
#define KIRITO_DISPATCHER_HPP

// Multiprocessing coordinator. Kirito's value model is share-nothing (each KiritoVM owns an
// UNSYNCHRONIZED arena), so concurrency is many fully-isolated VMs — one per OS thread — that
// communicate ONLY by passing serialized blobs through thread-safe primitives owned here. The one
// hard invariant: a given VM/arena is touched by exactly one thread (the thread that built it).
//
// Deadlock safety is designed-in: every blocking primitive is a `Waitable` with an `aborted_` flag,
// and shutdown() aborts them all before joining threads, so a worker blocked anywhere always unwinds.
//
// NOTE: this header is included by kirito.hpp AFTER runtime.hpp, so the inline KiritoVM members it
// calls (evalIn / newModuleScope / programForFile / importModule) are already defined. The
// dispatcher does not install the `parallel` module itself — configureVM() does (defined in
// stdlib_parallel.hpp, which is included after this file), so a bare KiritoVM has no `parallel`.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fum/unordered_map.hpp"
#include "ast.hpp"
#include "builtins.hpp"
#include "collections.hpp"
#include "exceptions.hpp"
#include "function.hpp"
#include "stdlib_dump.hpp"  // dumpfmt::write / read
#include "vm.hpp"

namespace kirito {

// How a blocking primitive op finished, translated by the `parallel` module into Kirito exceptions.
enum class WaitResult { Ok, TimedOut, Closed, Aborted, Reentrant, Broken };

inline std::chrono::duration<double> waitDuration(double seconds) {
    return std::chrono::duration<double>(seconds);
}

// A blocking, cross-VM coordination object. shutdown() aborts every Waitable so no blocked worker can
// stall teardown; a blocked op then returns Aborted and the worker unwinds (throwing a catchable
// "operation aborted" error — caught by a bare `catch`; there is no typed parallel.Aborted class).
class Waitable {
public:
    virtual ~Waitable() = default;
    virtual void abort() = 0;
};

// ---------------------------------------------------------------------------------------------------
// Queue — a thread-safe MPMC FIFO of serialized blobs (unbounded when maxsize == 0). The central
// cross-VM transfer primitive: a value is dumpfmt::write'd by the sender and dumpfmt::read by the
// receiver, each on its own thread, so no arena is shared.
// ---------------------------------------------------------------------------------------------------
class ConcurrentQueue : public Waitable {
public:
    ConcurrentQueue(uint64_t id, std::size_t maxsize) : id_(id), maxsize_(maxsize) {}
    uint64_t id() const { return id_; }

    WaitResult put(std::string blob, bool block, std::optional<double> timeout) {
        std::unique_lock<std::mutex> lk(m_);
        auto ready = [&] { return aborted_ || closed_ || maxsize_ == 0 || q_.size() < maxsize_; };
        if (!awaitPred(lk, notFull_, ready, block, timeout)) return WaitResult::TimedOut;
        if (aborted_) return WaitResult::Aborted;
        if (closed_) return WaitResult::Closed;
        q_.push_back(std::move(blob));
        notEmpty_.notify_one();
        return WaitResult::Ok;
    }

    WaitResult get(bool block, std::optional<double> timeout, std::string& out) {
        std::unique_lock<std::mutex> lk(m_);
        auto ready = [&] { return aborted_ || closed_ || !q_.empty(); };
        if (!awaitPred(lk, notEmpty_, ready, block, timeout)) return WaitResult::TimedOut;
        if (!q_.empty()) {  // always drain available items before reporting closed
            out = std::move(q_.front());
            q_.pop_front();
            notFull_.notify_one();
            return WaitResult::Ok;
        }
        if (aborted_) return WaitResult::Aborted;
        return WaitResult::Closed;
    }

    void close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }
    void abort() override {
        { std::lock_guard<std::mutex> lk(m_); aborted_ = true; }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }
    bool closed() const { std::lock_guard<std::mutex> lk(m_); return closed_; }
    std::size_t size() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    bool empty() const { std::lock_guard<std::mutex> lk(m_); return q_.empty(); }
    bool full() const {
        std::lock_guard<std::mutex> lk(m_);
        return maxsize_ != 0 && q_.size() >= maxsize_;
    }

private:
    // Wait until `ready` holds. Returns false only on timeout / a non-blocking call that isn't ready.
    template <class Pred>
    static bool awaitPred(std::unique_lock<std::mutex>& lk, std::condition_variable& cv, Pred ready,
                          bool block, std::optional<double> timeout) {
        if (ready()) return true;
        if (!block) return false;
        if (timeout) return cv.wait_for(lk, waitDuration(*timeout), ready);
        cv.wait(lk, ready);
        return true;
    }
    uint64_t id_;
    std::size_t maxsize_;
    mutable std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<std::string> q_;
    bool closed_ = false;
    bool aborted_ = false;
};

// Lock — a non-reentrant mutex. A second acquire by the same thread (without releasing) is reported
// Reentrant (a catchable error), never a self-deadlock.
class Lock : public Waitable {
public:
    explicit Lock(uint64_t id) : id_(id) {}
    uint64_t id() const { return id_; }

    WaitResult acquire(bool block, std::optional<double> timeout) {
        std::unique_lock<std::mutex> lk(m_);
        if (held_ && owner_ == std::this_thread::get_id()) return WaitResult::Reentrant;
        auto ready = [&] { return aborted_ || !held_; };
        if (!ready()) {
            if (!block) return WaitResult::TimedOut;
            if (timeout) { if (!cv_.wait_for(lk, waitDuration(*timeout), ready)) return WaitResult::TimedOut; }
            else cv_.wait(lk, ready);
        }
        if (aborted_) return WaitResult::Aborted;
        held_ = true;
        owner_ = std::this_thread::get_id();
        return WaitResult::Ok;
    }
    // Release, returning false if it wasn't held (a usage error the caller reports). A Lock is
    // deliberately NOT owner-bound: a worker may acquire it, hand the live Lock back to another VM,
    // and that VM releases it (a transferable handoff, like Python's threading.Lock) — so release is
    // by identity, not by owning thread.
    bool release() {
        bool was;
        { std::lock_guard<std::mutex> lk(m_); was = held_; held_ = false; owner_ = std::thread::id{}; }
        if (was) cv_.notify_one();
        return was;
    }
    bool locked() const { std::lock_guard<std::mutex> lk(m_); return held_; }
    void abort() override {
        { std::lock_guard<std::mutex> lk(m_); aborted_ = true; }
        cv_.notify_all();
    }

private:
    uint64_t id_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool held_ = false;
    bool aborted_ = false;
    std::thread::id owner_{};
};

// Event — a resettable flag many workers wait on.
class Event : public Waitable {
public:
    explicit Event(uint64_t id) : id_(id) {}
    uint64_t id() const { return id_; }

    void set() {
        { std::lock_guard<std::mutex> lk(m_); flag_ = true; }
        cv_.notify_all();
    }
    void clear() { std::lock_guard<std::mutex> lk(m_); flag_ = false; }
    bool isset() const { std::lock_guard<std::mutex> lk(m_); return flag_; }
    WaitResult wait(std::optional<double> timeout) {
        std::unique_lock<std::mutex> lk(m_);
        auto ready = [&] { return aborted_ || flag_; };
        if (!ready()) {
            if (timeout) { if (!cv_.wait_for(lk, waitDuration(*timeout), ready)) return WaitResult::TimedOut; }
            else cv_.wait(lk, ready);
        }
        if (aborted_) return WaitResult::Aborted;
        return WaitResult::Ok;
    }
    void abort() override {
        { std::lock_guard<std::mutex> lk(m_); aborted_ = true; }
        cv_.notify_all();
    }

private:
    uint64_t id_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool flag_ = false;
    bool aborted_ = false;
};

// Semaphore — bounded concurrency / a resource pool of `value` permits.
class Semaphore : public Waitable {
public:
    Semaphore(uint64_t id, int64_t value) : id_(id), count_(value) {}
    uint64_t id() const { return id_; }

    WaitResult acquire(bool block, std::optional<double> timeout) {
        std::unique_lock<std::mutex> lk(m_);
        auto ready = [&] { return aborted_ || count_ > 0; };
        if (!ready()) {
            if (!block) return WaitResult::TimedOut;
            if (timeout) { if (!cv_.wait_for(lk, waitDuration(*timeout), ready)) return WaitResult::TimedOut; }
            else cv_.wait(lk, ready);
        }
        if (aborted_) return WaitResult::Aborted;
        --count_;
        return WaitResult::Ok;
    }
    // Unbounded, by design: releasing more than was acquired grows the permit count (a plain counting
    // semaphore / latch, like Python's threading.Semaphore — NOT BoundedSemaphore). A ceiling would
    // break the deliberate over-release-to-raise-permits pattern.
    void release() {
        { std::lock_guard<std::mutex> lk(m_); ++count_; }
        cv_.notify_one();
    }
    void abort() override {
        { std::lock_guard<std::mutex> lk(m_); aborted_ = true; }
        cv_.notify_all();
    }

private:
    uint64_t id_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    int64_t count_;
    bool aborted_ = false;
};

// Barrier — N parties rendezvous: each wait() blocks until `parties` have arrived, then all proceed.
// A timeout, reset(), or abort() breaks the current generation (waiters get Broken / Aborted).
class Barrier : public Waitable {
public:
    Barrier(uint64_t id, int64_t parties) : id_(id), parties_(parties < 1 ? 1 : parties) {}
    uint64_t id() const { return id_; }
    int64_t parties() const { std::lock_guard<std::mutex> lk(m_); return parties_; }
    int64_t nwaiting() const { std::lock_guard<std::mutex> lk(m_); return count_; }

    WaitResult wait(std::optional<double> timeout, int64_t& indexOut) {
        std::unique_lock<std::mutex> lk(m_);
        if (aborted_) return WaitResult::Aborted;
        int64_t gen = generation_;
        indexOut = count_++;
        if (count_ >= parties_) {  // last arrival releases the generation
            ++generation_;
            count_ = 0;
            cv_.notify_all();
            // If this generation was already broken (an earlier waiter timed out / reset), every
            // wait() in it must report Broken — including this last arrival.
            return brokenGen_ == gen ? WaitResult::Broken : WaitResult::Ok;
        }
        auto ready = [&] { return aborted_ || generation_ != gen || brokenGen_ == gen; };
        if (timeout) {
            if (!cv_.wait_for(lk, waitDuration(*timeout), ready)) {
                // A timed-out waiter breaks the barrier. Reset the generation's bookkeeping FULLY —
                // advance the generation and clear count_ — so a later arrival starts a clean
                // generation instead of tripping "last party" off a count that still includes this
                // (now-departed) waiter and its still-blocked peers. The guard makes concurrent
                // timeouts in the same generation break it only once.
                if (brokenGen_ != gen) { brokenGen_ = gen; ++generation_; count_ = 0; }
                cv_.notify_all();
                return WaitResult::Broken;
            }
        } else {
            cv_.wait(lk, ready);
        }
        if (aborted_) return WaitResult::Aborted;
        if (brokenGen_ == gen) return WaitResult::Broken;
        return WaitResult::Ok;
    }
    void resetBarrier() {  // break the current generation's waiters; the barrier stays reusable
        { std::lock_guard<std::mutex> lk(m_); brokenGen_ = generation_; ++generation_; count_ = 0; }
        cv_.notify_all();
    }
    void abort() override {
        { std::lock_guard<std::mutex> lk(m_); aborted_ = true; }
        cv_.notify_all();
    }

private:
    uint64_t id_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    int64_t parties_;
    int64_t count_ = 0;
    int64_t generation_ = 0;
    int64_t brokenGen_ = -1;
    bool aborted_ = false;
};

// A spawned worker: its OS thread plus the result (a dump blob) or error it produced.
struct Task {
    uint64_t id = 0;
    std::thread thread;
    std::atomic<bool> done{false};
    std::atomic<bool> joining{false};   // claimed by the one caller that joins the thread (see reap())
    std::string result;  // dump blob, valid iff done && !hasError
    bool hasError = false;
    std::string errorText;
};

// ---------------------------------------------------------------------------------------------------
// findFunctionBySpan — locate the FunctionExpr at (line,col) by a read-only full-AST descent. Spans
// are unique per file, so this recovers a spawned function's definition in a freshly re-parsed worker
// chunk. Mirrors analyzer.hpp's descent (every Stmt/Expr is visited).
// ---------------------------------------------------------------------------------------------------
namespace dispatch_detail {

inline const ast::FunctionExpr* findInExpr(const ast::Expr* e, uint32_t line, uint32_t col);
inline const ast::FunctionExpr* findInBlock(const ast::Block& b, uint32_t line, uint32_t col);

inline const ast::FunctionExpr* findInStmt(const ast::Stmt* s, uint32_t line, uint32_t col) {
    using namespace ast;
    if (!s) return nullptr;
    if (const auto* e = dynamic_cast<const ExprStmt*>(s)) return findInExpr(e->expr.get(), line, col);
    if (const auto* d = dynamic_cast<const DiscardStmt*>(s)) return findInExpr(d->expr.get(), line, col);
    if (const auto* v = dynamic_cast<const VarDeclStmt*>(s)) return findInExpr(v->init.get(), line, col);
    if (const auto* a = dynamic_cast<const AssignStmt*>(s)) {
        if (auto* r = findInExpr(a->target.get(), line, col)) return r;
        return findInExpr(a->value.get(), line, col);
    }
    if (const auto* i = dynamic_cast<const IfStmt*>(s)) {
        for (const auto& [cond, body] : i->branches) {
            if (auto* r = findInExpr(cond.get(), line, col)) return r;
            if (auto* r = findInBlock(body, line, col)) return r;
        }
        if (i->orelse) return findInBlock(*i->orelse, line, col);
        return nullptr;
    }
    if (const auto* w = dynamic_cast<const WhileStmt*>(s)) {
        if (auto* r = findInExpr(w->cond.get(), line, col)) return r;
        return findInBlock(w->body, line, col);
    }
    if (const auto* f = dynamic_cast<const ForStmt*>(s)) {
        if (auto* r = findInExpr(f->iterable.get(), line, col)) return r;
        return findInBlock(f->body, line, col);
    }
    if (const auto* r = dynamic_cast<const ReturnStmt*>(s)) return findInExpr(r->value.get(), line, col);
    if (const auto* t = dynamic_cast<const TryStmt*>(s)) {
        if (auto* r = findInBlock(t->body, line, col)) return r;
        for (const auto& h : t->handlers) {
            if (auto* r = findInExpr(h.type.get(), line, col)) return r;
            if (auto* r = findInBlock(h.body, line, col)) return r;
        }
        if (t->hasFinally) return findInBlock(t->finallyBody, line, col);
        return nullptr;
    }
    if (const auto* th = dynamic_cast<const ThrowStmt*>(s)) return findInExpr(th->value.get(), line, col);
    if (const auto* c = dynamic_cast<const ClassStmt*>(s)) {
        if (auto* r = findInExpr(c->base.get(), line, col)) return r;
        return findInBlock(c->body, line, col);
    }
    if (const auto* wi = dynamic_cast<const WithStmt*>(s)) {
        if (auto* r = findInExpr(wi->context.get(), line, col)) return r;
        return findInBlock(wi->body, line, col);
    }
    if (const auto* as = dynamic_cast<const AssertStmt*>(s)) {
        if (auto* r = findInExpr(as->cond.get(), line, col)) return r;
        return findInExpr(as->message.get(), line, col);
    }
    if (const auto* sw = dynamic_cast<const SwitchStmt*>(s)) {
        if (auto* r = findInExpr(sw->subject.get(), line, col)) return r;
        for (const auto& cl : sw->cases) {
            for (const auto& cv : cl.values) if (auto* r = findInExpr(cv.get(), line, col)) return r;
            if (auto* r = findInBlock(cl.body, line, col)) return r;
        }
        if (sw->hasDefault) return findInBlock(sw->defaultBody, line, col);
        return nullptr;
    }
    return nullptr;  // Break/Continue/Pass/Todo carry no sub-expressions
}

inline const ast::FunctionExpr* findInBlock(const ast::Block& b, uint32_t line, uint32_t col) {
    for (const auto& s : b) if (auto* r = findInStmt(s.get(), line, col)) return r;
    return nullptr;
}

inline const ast::FunctionExpr* findInExpr(const ast::Expr* e, uint32_t line, uint32_t col) {
    using namespace ast;
    if (!e) return nullptr;
    if (const auto* fn = dynamic_cast<const FunctionExpr*>(e)) {
        if (fn->span.line == line && fn->span.col == col) return fn;  // the match
        for (const auto& p : fn->params) if (auto* r = findInExpr(p.defaultValue.get(), line, col)) return r;
        return findInBlock(fn->body, line, col);
    }
    if (const auto* u = dynamic_cast<const UnaryExpr*>(e)) return findInExpr(u->operand.get(), line, col);
    if (const auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (auto* r = findInExpr(b->lhs.get(), line, col)) return r;
        return findInExpr(b->rhs.get(), line, col);
    }
    if (const auto* l = dynamic_cast<const LogicalExpr*>(e)) {
        if (auto* r = findInExpr(l->lhs.get(), line, col)) return r;
        return findInExpr(l->rhs.get(), line, col);
    }
    if (const auto* cnd = dynamic_cast<const ConditionalExpr*>(e)) {
        if (auto* r = findInExpr(cnd->then.get(), line, col)) return r;
        if (auto* r = findInExpr(cnd->cond.get(), line, col)) return r;
        return findInExpr(cnd->orelse.get(), line, col);
    }
    if (const auto* c = dynamic_cast<const CallExpr*>(e)) {
        if (auto* r = findInExpr(c->callee.get(), line, col)) return r;
        for (const auto& a : c->args) if (auto* r = findInExpr(a.value.get(), line, col)) return r;
        return nullptr;
    }
    if (const auto* m = dynamic_cast<const MemberExpr*>(e)) return findInExpr(m->object.get(), line, col);
    if (const auto* ix = dynamic_cast<const IndexExpr*>(e)) {
        if (auto* r = findInExpr(ix->object.get(), line, col)) return r;
        for (const auto& k : ix->indices) if (auto* r = findInExpr(k.get(), line, col)) return r;
        return nullptr;
    }
    if (const auto* sl = dynamic_cast<const SliceExpr*>(e)) {
        if (auto* r = findInExpr(sl->object.get(), line, col)) return r;
        if (auto* r = findInExpr(sl->start.get(), line, col)) return r;
        if (auto* r = findInExpr(sl->stop.get(), line, col)) return r;
        return findInExpr(sl->step.get(), line, col);
    }
    if (const auto* lst = dynamic_cast<const ListLiteral*>(e)) {
        for (const auto& x : lst->elems) if (auto* r = findInExpr(x.get(), line, col)) return r;
        return nullptr;
    }
    if (const auto* st = dynamic_cast<const SetLiteral*>(e)) {
        for (const auto& x : st->elems) if (auto* r = findInExpr(x.get(), line, col)) return r;
        return nullptr;
    }
    if (const auto* dt = dynamic_cast<const DictLiteral*>(e)) {
        for (const auto& [k, v] : dt->entries) {
            if (auto* r = findInExpr(k.get(), line, col)) return r;
            if (auto* r = findInExpr(v.get(), line, col)) return r;
        }
        return nullptr;
    }
    if (const auto* fs = dynamic_cast<const FStringExpr*>(e)) {
        for (const auto& p : fs->parts) if (p.isExpr) if (auto* r = findInExpr(p.expr.get(), line, col)) return r;
        return nullptr;
    }
    if (const auto* tup = dynamic_cast<const TupleExpr*>(e)) {
        for (const auto& x : tup->elems) if (auto* r = findInExpr(x.get(), line, col)) return r;
        return nullptr;
    }
    if (const auto* star = dynamic_cast<const StarExpr*>(e)) return findInExpr(star->inner.get(), line, col);
    return nullptr;  // Literal / Name: no sub-functions
}

}  // namespace dispatch_detail

inline const ast::FunctionExpr* findFunctionBySpan(const ast::Program& prog, uint32_t line, uint32_t col) {
    return dispatch_detail::findInBlock(prog.stmts, line, col);
}

// ---------------------------------------------------------------------------------------------------
// Argument marshalling: spawn packs (positional, named) into a dumped 2-list [posList, namedDict];
// the worker unpacks it back into the call shape.
// ---------------------------------------------------------------------------------------------------
inline std::string packArgs(KiritoVM& vm, std::span<const Handle> positional, std::span<const NamedArg> named) {
    RootScope rs(vm);
    auto posList = std::make_unique<ListVal>();
    for (Handle h : positional) posList->elems.push_back(h);
    Handle posH = rs.add(vm.alloc(std::move(posList)));
    Handle namedH = rs.add(vm.alloc(std::make_unique<DictVal>()));
    for (const auto& na : named) {
        Handle key = rs.add(vm.makeString(na.name));
        static_cast<DictVal&>(vm.arena().deref(namedH)).set(vm.arena(), key, na.value);
    }
    auto packed = std::make_unique<ListVal>();
    packed->elems.push_back(posH);
    packed->elems.push_back(namedH);
    return dumpfmt::write(vm, rs.add(vm.alloc(std::move(packed))));
}

inline void unpackArgs(KiritoVM& vm, Handle packed, std::vector<Handle>& pos, std::vector<NamedArg>& named) {
    Object& o = vm.arena().deref(packed);
    if (o.kind() != ValueKind::List) throw KiritoError("parallel: corrupt spawn arguments");
    auto& outer = static_cast<ListVal&>(o);
    if (outer.elems.size() != 2) throw KiritoError("parallel: corrupt spawn arguments");
    Object& po = vm.arena().deref(outer.elems[0]);
    Object& no = vm.arena().deref(outer.elems[1]);
    if (po.kind() != ValueKind::List || no.kind() != ValueKind::Dict)
        throw KiritoError("parallel: corrupt spawn arguments");
    for (Handle h : static_cast<ListVal&>(po).elems) pos.push_back(h);
    auto& nd = static_cast<DictVal&>(no);
    for (Handle k : nd.keys()) {
        const Object& ko = vm.arena().deref(k);
        if (ko.kind() != ValueKind::String) continue;
        named.push_back(NamedArg{static_cast<const StrVal&>(ko).value(), *nd.find(vm.arena(), k)});
    }
}

// ---------------------------------------------------------------------------------------------------
// KiritoDispatcher — owns the main VM, the cross-VM primitives, and the worker threads.
// ---------------------------------------------------------------------------------------------------
class KiritoDispatcher {
public:
    KiritoDispatcher() = default;
    KiritoDispatcher(const KiritoDispatcher&) = delete;
    KiritoDispatcher& operator=(const KiritoDispatcher&) = delete;
    ~KiritoDispatcher() { shutdown(); }

    KiritoVM& mainVM() {
        if (!main_) {
            main_ = std::make_unique<KiritoVM>();
            configureVM(*main_);
        }
        return *main_;
    }
    void addLibPath(const std::string& d) {
        // libPaths_/maxCallDepth_ are read by worker threads in configureVM(); guard the write so a
        // set-during-spawn is not a data race (TSan). The lock is NOT held across mainVM()/the VM call.
        { std::lock_guard<std::mutex> lk(registryMutex_); libPaths_.push_back(d); }
        mainVM().addLibPath(d);
    }
    void setMaxCallDepth(std::size_t n) {
        { std::lock_guard<std::mutex> lk(registryMutex_); maxCallDepth_ = n; }
        if (main_) main_->setMaxCallDepth(n);
    }

    std::shared_ptr<ConcurrentQueue> createQueue(std::size_t maxsize) { return makeWaitable(queues_, maxsize); }
    std::shared_ptr<ConcurrentQueue> queueById(uint64_t id) { return byId(queues_, id); }
    std::shared_ptr<Lock> createLock() { return makeWaitable(locks_); }
    std::shared_ptr<Lock> lockById(uint64_t id) { return byId(locks_, id); }
    std::shared_ptr<Event> createEvent() { return makeWaitable(events_); }
    std::shared_ptr<Event> eventById(uint64_t id) { return byId(events_, id); }
    std::shared_ptr<Semaphore> createSemaphore(int64_t v) { return makeWaitable(semaphores_, v); }
    std::shared_ptr<Semaphore> semaphoreById(uint64_t id) { return byId(semaphores_, id); }
    std::shared_ptr<Barrier> createBarrier(int64_t n) { return makeWaitable(barriers_, n); }
    std::shared_ptr<Barrier> barrierById(uint64_t id) { return byId(barriers_, id); }

    uint64_t spawnTask(std::string sourceFile, uint32_t line, uint32_t col, std::string argsBlob) {
        std::lock_guard<std::mutex> lk(registryMutex_);
        // A spawn racing teardown would create a thread that shutdown's join sweep already passed,
        // leaving it un-joined (and dereferencing a freed Task) — refuse once shutdown has begun.
        if (shuttingDown_) throw KiritoError("parallel: the dispatcher is shutting down");
        uint64_t id = nextId_++;
        auto task = std::make_shared<Task>();
        task->id = id;
        Task* tp = task.get();
        tasks_[id] = std::move(task);
        tp->thread = std::thread([this, tp, sf = std::move(sourceFile), line, col,
                                  blob = std::move(argsBlob)]() { runWorker(tp, sf, line, col, blob); });
        return id;
    }
    bool taskDone(uint64_t id) {
        std::lock_guard<std::mutex> lk(registryMutex_);
        auto it = tasks_.find(id);
        // Absent => already join()ed and erased (an id comes only from a real spawn and is dropped
        // from the map only after reap(), which implies done). Present => report its live flag.
        return it == tasks_.end() || it->second->done.load();
    }
    // Join the worker thread (NOT holding the registry lock, so a worker that calls back in can't
    // deadlock) and return its result blob; `hasError`/`errorText` report a worker-side failure.
    std::string joinTask(uint64_t id, bool& hasError, std::string& errorText) {
        std::shared_ptr<Task> t;
        {
            std::lock_guard<std::mutex> lk(registryMutex_);
            auto it = tasks_.find(id);
            if (it == tasks_.end()) throw KiritoError("parallel: invalid task handle");
            t = it->second;   // a shared_ptr copy keeps the Task alive even after we drop it from the map
        }
        reap(t.get());   // join exactly once even if join()/shutdown() race on the same Task
        hasError = t->hasError;
        errorText = t->errorText;
        std::string result = std::move(t->result);
        {
            // Drop the map's reference now that it is joined — else `tasks_` grows one Task (thread +
            // result blob) per spawn for the dispatcher's whole life (A08-2/A19-2), unbounded in a
            // long-running spawn-per-request server. `t` keeps it alive until this call returns; a
            // concurrent shutdown() holds its own shared_ptr, so reap()/erase never dangle.
            std::lock_guard<std::mutex> lk(registryMutex_);
            tasks_.erase(id);
        }
        return result;
    }

    // Idempotent: abort EVERY waitable first (so blocked workers wake and unwind), then join all
    // threads. After this, no thread is running and no primitive blocks.
    void shutdown() {
        std::vector<std::shared_ptr<Waitable>> live;
        {
            std::lock_guard<std::mutex> lk(registryMutex_);
            shuttingDown_ = true;   // set before aborting so no new task can be spawned past the sweep
            for (auto& w : waitables_) if (auto sp = w.lock()) live.push_back(sp);
        }
        for (auto& sp : live) sp->abort();
        std::vector<std::shared_ptr<Task>> ts;
        {
            std::lock_guard<std::mutex> lk(registryMutex_);
            for (auto& [id, t] : tasks_) ts.push_back(t);   // shared_ptr copies: a concurrent join()'s
        }                                                    // erase can't free a Task we're about to reap
        for (auto& t : ts) reap(t.get());   // reap() each exactly once, coordinating with any live join()
    }

    static unsigned cpuCount() {
        unsigned n = std::thread::hardware_concurrency();
        return n ? n : 1u;
    }

    void configureVM(KiritoVM& vm);  // defined in stdlib_parallel.hpp (it installs ParallelModule)

private:
    // Join a worker's thread exactly once, even when join() (from Kirito) and shutdown() race on the
    // same Task: the first to claim `joining` does the std::thread::join(); any other caller waits for
    // the worker to finish (the claimer's join() then reaps it). Two join()s on one std::thread is UB.
    void reap(Task* t) {
        if (!t->joining.exchange(true)) {
            if (t->thread.joinable()) t->thread.join();
        } else {
            while (!t->done.load()) std::this_thread::yield();
        }
    }

    // Create a primitive of the map's element type, register it under a fresh id, and track it as a
    // Waitable for shutdown's abort fan-out. Explicit return type (not `auto`) so the create* methods
    // above can call it regardless of in-class declaration order.
    template <class Map, class... A>
    typename Map::mapped_type makeWaitable(Map& reg, A&&... args) {
        using T = typename Map::mapped_type::element_type;
        std::lock_guard<std::mutex> lk(registryMutex_);
        uint64_t id = nextId_++;
        std::shared_ptr<T> sp = std::make_shared<T>(id, std::forward<A>(args)...);
        reg[id] = sp;
        waitables_.push_back(std::weak_ptr<Waitable>(std::static_pointer_cast<Waitable>(sp)));
        // A worker that creates a primitive AFTER shutdown()'s abort fan-out (it holds registryMutex_
        // between its two phases, but a worker can still slip a makeWaitable in between the abort loop
        // and the join loop) would otherwise get a live primitive and block on it forever, hanging the
        // join. Pre-abort anything born during teardown so any wait on it returns Aborted immediately.
        if (shuttingDown_) sp->abort();
        return sp;
    }
    template <class Map>
    typename Map::mapped_type byId(Map& reg, uint64_t id) {
        std::lock_guard<std::mutex> lk(registryMutex_);
        auto it = reg.find(id);
        return it != reg.end() ? it->second : nullptr;
    }

    std::string readModuleSource(KiritoVM& vm, const std::string& sourceFile) {
        auto tryRead = [](const std::string& p, std::string& out) -> bool {
            std::ifstream in(p, std::ios::binary);
            if (!in) return false;
            std::stringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        };
        std::string src;
        if (tryRead(sourceFile, src)) return src;
        std::error_code ec;
        std::string base = std::filesystem::path(sourceFile).filename().string();
        for (const auto& dir : vm.libPaths())
            if (tryRead((std::filesystem::path(dir) / base).string(), src)) return src;
        throw KiritoError("parallel.spawn: cannot read source file '" + sourceFile +
                          "' (the spawned function must be defined in a loadable .ki file)");
    }

    void runWorker(Task* tp, const std::string& sourceFile, uint32_t line, uint32_t col,
                   const std::string& argsBlob) {
        try {
            KiritoVM worker;
            configureVM(worker);
            try {
                RootScope rs(worker);
                Handle modScope = rs.add(worker.newModuleScope(/*isMain=*/false));
                std::string src = readModuleSource(worker, sourceFile);
                worker.evalIn(src, modScope, sourceFile);  // populate modScope + index its Program
                const ast::Program* prog = worker.programForFile(sourceFile);
                if (!prog) throw KiritoError("parallel.spawn: could not load '" + sourceFile + "'");
                const ast::FunctionExpr* def = findFunctionBySpan(*prog, line, col);
                if (!def) throw KiritoError("parallel.spawn: could not locate the spawned function in '" +
                                            sourceFile + "'");
                auto fnv = std::make_unique<KiFunction>(def, modScope);
                fnv->sourceFile = sourceFile;
                Handle fnH = rs.add(worker.alloc(std::move(fnv)));
                Handle packed = rs.add(dumpfmt::read(worker, argsBlob));
                std::vector<Handle> pos;
                std::vector<NamedArg> named;
                unpackArgs(worker, packed, pos, named);
                for (Handle h : pos) rs.add(h);
                for (auto& na : named) rs.add(na.value);
                Handle r = static_cast<KiFunction&>(worker.arena().deref(fnH)).callFull(worker, pos, named);
                tp->result = dumpfmt::write(worker, r);  // throws if the result isn't serializable
            } catch (const KiritoError&) {
                // KiritoError derives from KiritoThrow, so this pass-through catch keeps
                // it going to the outer handler (which uses e.what()) instead of the
                // KiritoThrow arm (which would deref a Handle{}). Order matters.
                throw;
            } catch (const KiritoThrow& t) {
                tp->hasError = true;
                tp->errorText = worker.stringify(t.value);  // stringify while the worker VM is alive
            }
        } catch (const KiritoError& e) {
            tp->hasError = true;
            tp->errorText = e.what();
        } catch (const std::exception& e) {
            tp->hasError = true;
            tp->errorText = e.what();
        } catch (...) {
            tp->hasError = true;
            tp->errorText = "unknown error in worker";
        }
        tp->done.store(true);
    }

    std::unique_ptr<KiritoVM> main_;
    std::vector<std::string> libPaths_;
    std::size_t maxCallDepth_ = 0;  // 0 == leave the VM default

    std::mutex registryMutex_;  // guards the maps below; NEVER held across a thread join or VM call
    fum::unordered_map<uint64_t, std::shared_ptr<ConcurrentQueue>> queues_;
    fum::unordered_map<uint64_t, std::shared_ptr<Lock>> locks_;
    fum::unordered_map<uint64_t, std::shared_ptr<Event>> events_;
    fum::unordered_map<uint64_t, std::shared_ptr<Semaphore>> semaphores_;
    fum::unordered_map<uint64_t, std::shared_ptr<Barrier>> barriers_;
    std::vector<std::weak_ptr<Waitable>> waitables_;  // abort() fan-out for shutdown()
    fum::unordered_map<uint64_t, std::shared_ptr<Task>> tasks_;
    uint64_t nextId_ = 1;  // one id space across all primitives + tasks
    bool shuttingDown_ = false;  // guarded by registryMutex_; set once by shutdown(), read by spawnTask
};

}  // namespace kirito

#endif
