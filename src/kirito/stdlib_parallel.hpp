#ifndef KIRITO_PARALLEL_HPP
#define KIRITO_PARALLEL_HPP

// The `parallel` module — Kirito's multiprocessing surface over the KiritoDispatcher: spawn worker
// VMs, hand values between them through a thread-safe Queue, and coordinate with Lock / Event /
// Semaphore / Barrier. Every primitive crosses VMs BY IDENTITY (its _getstate_ emits a dispatcher
// id; _setstate_ rebinds to the same C++ object), so passing one into spawn(...) or through a Queue
// references the same underlying object. Installed by KiritoDispatcher::configureVM (defined at the
// end of this file), so a bare embedded KiritoVM has no `parallel` module.
//
// Blocking conditions throw clear, catchable errors (caught by a bare `catch as e`): a closed queue,
// an empty/full queue (timeout / nowait), a non-reentrant lock, a broken barrier, or a dispatcher
// shutdown ("operation aborted"). Typed exception classes are a future enrichment.

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "builtins.hpp"
#include "collections.hpp"
#include "dispatcher.hpp"
#include "native.hpp"
#include "stdlib_dump.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr `vm`/`self` (same VM, by design). Silence -Wshadow for
// these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

inline KiritoDispatcher& requireDispatcher(KiritoVM& vm, const char* who) {
    KiritoDispatcher* d = vm.dispatcher();
    if (!d)
        throw KiritoError(std::string(who) +
                          ": requires running under a KiritoDispatcher (the `ki` interpreter provides "
                          "one; a bare embedded KiritoVM does not)");
    return *d;
}

inline bool argTruthy(KiritoVM& vm, std::span<const Handle> a, std::size_t i, bool dflt) {
    if (a.size() <= i) return dflt;
    const Object& o = vm.arena().deref(a[i]);
    if (o.kind() == ValueKind::None) return dflt;
    return o.truthy();
}
inline std::optional<double> argTimeout(KiritoVM& vm, std::span<const Handle> a, std::size_t i) {
    if (a.size() <= i) return std::nullopt;
    const Object& o = vm.arena().deref(a[i]);
    if (o.kind() == ValueKind::None) return std::nullopt;
    if (o.kind() == ValueKind::Integer) return static_cast<double>(static_cast<const IntVal&>(o).value());
    if (o.kind() == ValueKind::Float) return static_cast<const FloatVal&>(o).value();
    throw KiritoError("parallel: timeout must be a number or None");
}

// Map a non-Ok wait outcome to a catchable Kirito error. (Ok is handled by the caller.)
[[noreturn]] inline void throwWait(WaitResult r, const char* who) {
    switch (r) {
        case WaitResult::Closed: { throw KiritoError(std::string(who) + ": queue is closed"); } break;
        case WaitResult::Aborted: { throw KiritoError("parallel: operation aborted (dispatcher shut down)"); } break;
        case WaitResult::Reentrant: {
            throw KiritoError(std::string(who) + ": lock is not reentrant (already held by this worker)");
        } break;
        case WaitResult::Broken: { throw KiritoError(std::string(who) + ": barrier is broken"); } break;
        case WaitResult::TimedOut: { throw KiritoError(std::string(who) + ": timed out"); } break;
        case WaitResult::Ok: break;
    }
    throw KiritoError(std::string(who) + ": unexpected wait result");
}

// ---------------------------------------------------------------------------------------------------
// Queue — the central cross-VM transfer primitive.
// ---------------------------------------------------------------------------------------------------
class QueueVal : public NativeClass<QueueVal> {
public:
    static constexpr const char* kTypeName = "Queue";
    std::shared_ptr<ConcurrentQueue> q;

    std::vector<std::string> inspectMembers() const override {
        return {"put(item, block, timeout)", "get(block, timeout) -> value", "putnowait(item)",
                "getnowait() -> value", "qsize() -> Integer", "empty() -> Bool", "full() -> Bool",
                "close()"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto que = [](KiritoVM& vm, Handle self) -> ConcurrentQueue& {
            auto& qv = static_cast<QueueVal&>(vm.arena().deref(self));
            if (!qv.q) throw KiritoError("parallel.Queue: uninitialized queue");
            return *qv.q;
        };
        if (name == "put")
            return bind("put", {"item", "block", "timeout"},
                        [self, que](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                            if (a.empty()) throw KiritoError("Queue.put: missing item");
                            std::string blob = dumpfmt::write(vm, a[0]);
                            WaitResult r = que(vm, self).put(std::move(blob), argTruthy(vm, a, 1, true),
                                                             argTimeout(vm, a, 2));
                            if (r == WaitResult::TimedOut)
                                throw KiritoError("parallel.Queue: put to a full queue");
                            if (r != WaitResult::Ok) throwWait(r, "parallel.Queue.put");
                            return vm.none();
                        });
        if (name == "putnowait")
            return bind("putnowait", {"item"},
                        [self, que](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                            if (a.empty()) throw KiritoError("Queue.putnowait: missing item");
                            std::string blob = dumpfmt::write(vm, a[0]);
                            WaitResult r = que(vm, self).put(std::move(blob), false, std::nullopt);
                            if (r == WaitResult::TimedOut)
                                throw KiritoError("parallel.Queue: put to a full queue");
                            if (r != WaitResult::Ok) throwWait(r, "parallel.Queue.put");
                            return vm.none();
                        });
        if (name == "get")
            return bind("get", {"block", "timeout"},
                        [self, que](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                            std::string blob;
                            WaitResult r = que(vm, self).get(argTruthy(vm, a, 0, true),
                                                             argTimeout(vm, a, 1), blob);
                            if (r == WaitResult::TimedOut)
                                throw KiritoError("parallel.Queue: get from an empty queue");
                            if (r != WaitResult::Ok) throwWait(r, "parallel.Queue.get");
                            return dumpfmt::read(vm, blob);
                        });
        if (name == "getnowait")
            return bind("getnowait", {}, [self, que](KiritoVM& vm, std::span<const Handle>) -> Handle {
                std::string blob;
                WaitResult r = que(vm, self).get(false, std::nullopt, blob);
                if (r == WaitResult::TimedOut) throw KiritoError("parallel.Queue: get from an empty queue");
                if (r != WaitResult::Ok) throwWait(r, "parallel.Queue.get");
                return dumpfmt::read(vm, blob);
            });
        if (name == "qsize")
            return bind("qsize", {}, [self, que](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(que(vm, self).size()));
            });
        if (name == "empty")
            return bind("empty", {}, [self, que](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeBool(que(vm, self).empty());
            });
        if (name == "full")
            return bind("full", {}, [self, que](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeBool(que(vm, self).full());
            });
        if (name == "close")
            return bind("close", {}, [self, que](KiritoVM& vm, std::span<const Handle>) -> Handle {
                que(vm, self).close();
                return vm.none();
            });
        if (name == "_getstate_")
            return bind("_getstate_", {}, [self, que](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(que(vm, self).id()));
            });
        if (name == "_setstate_")
            return bind("_setstate_", {"state"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "_setstate_").require(1);
                KiritoDispatcher& d = requireDispatcher(vm, "parallel.Queue");
                auto& qv = static_cast<QueueVal&>(vm.arena().deref(self));
                qv.q = d.queueById(static_cast<uint64_t>(Value(vm, a[0]).asInt("Queue _setstate_")));
                if (!qv.q)
                    throw KiritoError("parallel.Queue: cannot rebind (unknown id; queues do not cross dispatchers)");
                return vm.none();
            });
        return Object::getAttr(vm, self, name);
    }
};

// ---------------------------------------------------------------------------------------------------
// Task — the future returned by spawn.
// ---------------------------------------------------------------------------------------------------
class TaskVal : public NativeClass<TaskVal> {
public:
    static constexpr const char* kTypeName = "Task";
    uint64_t taskId = 0;

    std::vector<std::string> inspectMembers() const override {
        return {"join() -> value", "done() -> Bool"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        if (name == "join")
            return bind("join", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
                KiritoDispatcher& d = requireDispatcher(vm, "parallel.Task.join");
                uint64_t id = static_cast<TaskVal&>(vm.arena().deref(self)).taskId;
                bool hasError = false;
                std::string err;
                std::string blob = d.joinTask(id, hasError, err);
                if (hasError) throw KiritoError("parallel: worker thrown: " + err);
                return dumpfmt::read(vm, blob);
            });
        if (name == "done")
            return bind("done", {}, [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
                KiritoDispatcher& d = requireDispatcher(vm, "parallel.Task.done");
                return vm.makeBool(d.taskDone(static_cast<TaskVal&>(vm.arena().deref(self)).taskId));
            });
        return Object::getAttr(vm, self, name);
    }
};

// ---------------------------------------------------------------------------------------------------
// Lock / Event / Semaphore / Barrier.
// ---------------------------------------------------------------------------------------------------
class LockVal : public NativeClass<LockVal> {
public:
    static constexpr const char* kTypeName = "Lock";
    std::shared_ptr<Lock> lock;

    std::vector<std::string> inspectMembers() const override {
        return {"acquire(block, timeout) -> Bool", "release()", "locked() -> Bool", "_enter_()", "_exit_()"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto lk = [](KiritoVM& vm, Handle self) -> Lock& {
            auto& lv = static_cast<LockVal&>(vm.arena().deref(self));
            if (!lv.lock) throw KiritoError("parallel.Lock: uninitialized lock");
            return *lv.lock;
        };
        if (name == "acquire")
            return bind("acquire", {"block", "timeout"},
                        [self, lk](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                            WaitResult r = lk(vm, self).acquire(argTruthy(vm, a, 0, true), argTimeout(vm, a, 1));
                            if (r == WaitResult::Ok) return vm.makeBool(true);
                            if (r == WaitResult::TimedOut) return vm.makeBool(false);
                            throwWait(r, "parallel.Lock.acquire");
                        });
        if (name == "release")
            return bind("release", {}, [self, lk](KiritoVM& vm, std::span<const Handle>) -> Handle {
                if (!lk(vm, self).release())
                    throw KiritoError("parallel.Lock: release of an unlocked lock");
                return vm.none();
            });
        if (name == "locked")
            return bind("locked", {}, [self, lk](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeBool(lk(vm, self).locked());
            });
        if (name == "_enter_")
            return bind("_enter_", {}, [self, lk](KiritoVM& vm, std::span<const Handle>) -> Handle {
                WaitResult r = lk(vm, self).acquire(true, std::nullopt);
                if (r != WaitResult::Ok) throwWait(r, "parallel.Lock.acquire");
                return self;
            });
        if (name == "_exit_")
            return bind("_exit_", {}, [self, lk](KiritoVM& vm, std::span<const Handle>) -> Handle {
                lk(vm, self).release();
                return vm.none();
            });
        if (name == "_getstate_")
            return bind("_getstate_", {}, [self, lk](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(lk(vm, self).id()));
            });
        if (name == "_setstate_")
            return bind("_setstate_", {"state"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "_setstate_").require(1);
                KiritoDispatcher& d = requireDispatcher(vm, "parallel.Lock");
                auto& lv = static_cast<LockVal&>(vm.arena().deref(self));
                lv.lock = d.lockById(static_cast<uint64_t>(Value(vm, a[0]).asInt("Lock _setstate_")));
                if (!lv.lock) throw KiritoError("parallel.Lock: cannot rebind (unknown id)");
                return vm.none();
            });
        return Object::getAttr(vm, self, name);
    }
};

class EventVal : public NativeClass<EventVal> {
public:
    static constexpr const char* kTypeName = "Event";
    std::shared_ptr<Event> event;

    std::vector<std::string> inspectMembers() const override {
        return {"set()", "clear()", "isset() -> Bool", "wait(timeout) -> Bool"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto ev = [](KiritoVM& vm, Handle self) -> Event& {
            auto& v = static_cast<EventVal&>(vm.arena().deref(self));
            if (!v.event) throw KiritoError("parallel.Event: uninitialized event");
            return *v.event;
        };
        if (name == "set")
            return bind("set", {}, [self, ev](KiritoVM& vm, std::span<const Handle>) -> Handle {
                ev(vm, self).set();
                return vm.none();
            });
        if (name == "clear")
            return bind("clear", {}, [self, ev](KiritoVM& vm, std::span<const Handle>) -> Handle {
                ev(vm, self).clear();
                return vm.none();
            });
        if (name == "isset")
            return bind("isset", {}, [self, ev](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeBool(ev(vm, self).isset());
            });
        if (name == "wait")
            return bind("wait", {"timeout"}, [self, ev](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                WaitResult r = ev(vm, self).wait(argTimeout(vm, a, 0));
                if (r == WaitResult::Ok) return vm.makeBool(true);
                if (r == WaitResult::TimedOut) return vm.makeBool(false);
                throwWait(r, "parallel.Event.wait");
            });
        if (name == "_getstate_")
            return bind("_getstate_", {}, [self, ev](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(ev(vm, self).id()));
            });
        if (name == "_setstate_")
            return bind("_setstate_", {"state"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "_setstate_").require(1);
                KiritoDispatcher& d = requireDispatcher(vm, "parallel.Event");
                auto& v = static_cast<EventVal&>(vm.arena().deref(self));
                v.event = d.eventById(static_cast<uint64_t>(Value(vm, a[0]).asInt("Event _setstate_")));
                if (!v.event) throw KiritoError("parallel.Event: cannot rebind (unknown id)");
                return vm.none();
            });
        return Object::getAttr(vm, self, name);
    }
};

class SemaphoreVal : public NativeClass<SemaphoreVal> {
public:
    static constexpr const char* kTypeName = "Semaphore";
    std::shared_ptr<Semaphore> sem;

    std::vector<std::string> inspectMembers() const override {
        return {"acquire(block, timeout) -> Bool", "release()", "_enter_()", "_exit_()"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto sm = [](KiritoVM& vm, Handle self) -> Semaphore& {
            auto& v = static_cast<SemaphoreVal&>(vm.arena().deref(self));
            if (!v.sem) throw KiritoError("parallel.Semaphore: uninitialized semaphore");
            return *v.sem;
        };
        if (name == "acquire")
            return bind("acquire", {"block", "timeout"},
                        [self, sm](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                            WaitResult r = sm(vm, self).acquire(argTruthy(vm, a, 0, true), argTimeout(vm, a, 1));
                            if (r == WaitResult::Ok) return vm.makeBool(true);
                            if (r == WaitResult::TimedOut) return vm.makeBool(false);
                            throwWait(r, "parallel.Semaphore.acquire");
                        });
        if (name == "release")
            return bind("release", {}, [self, sm](KiritoVM& vm, std::span<const Handle>) -> Handle {
                sm(vm, self).release();
                return vm.none();
            });
        if (name == "_enter_")
            return bind("_enter_", {}, [self, sm](KiritoVM& vm, std::span<const Handle>) -> Handle {
                WaitResult r = sm(vm, self).acquire(true, std::nullopt);
                if (r != WaitResult::Ok) throwWait(r, "parallel.Semaphore.acquire");
                return self;
            });
        if (name == "_exit_")
            return bind("_exit_", {}, [self, sm](KiritoVM& vm, std::span<const Handle>) -> Handle {
                sm(vm, self).release();
                return vm.none();
            });
        if (name == "_getstate_")
            return bind("_getstate_", {}, [self, sm](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(sm(vm, self).id()));
            });
        if (name == "_setstate_")
            return bind("_setstate_", {"state"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "_setstate_").require(1);
                KiritoDispatcher& d = requireDispatcher(vm, "parallel.Semaphore");
                auto& v = static_cast<SemaphoreVal&>(vm.arena().deref(self));
                v.sem = d.semaphoreById(static_cast<uint64_t>(Value(vm, a[0]).asInt("Semaphore _setstate_")));
                if (!v.sem) throw KiritoError("parallel.Semaphore: cannot rebind (unknown id)");
                return vm.none();
            });
        return Object::getAttr(vm, self, name);
    }
};

class BarrierVal : public NativeClass<BarrierVal> {
public:
    static constexpr const char* kTypeName = "Barrier";
    std::shared_ptr<Barrier> bar;

    std::vector<std::string> inspectMembers() const override {
        return {"wait(timeout) -> Integer", "parties() -> Integer", "nwaiting() -> Integer", "reset()", "abort()"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto br = [](KiritoVM& vm, Handle self) -> Barrier& {
            auto& v = static_cast<BarrierVal&>(vm.arena().deref(self));
            if (!v.bar) throw KiritoError("parallel.Barrier: uninitialized barrier");
            return *v.bar;
        };
        if (name == "wait")
            return bind("wait", {"timeout"}, [self, br](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                int64_t index = 0;
                WaitResult r = br(vm, self).wait(argTimeout(vm, a, 0), index);
                if (r == WaitResult::Ok) return vm.makeInt(index);
                throwWait(r, "parallel.Barrier.wait");
            });
        if (name == "parties")
            return bind("parties", {}, [self, br](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(br(vm, self).parties());
            });
        if (name == "nwaiting")
            return bind("nwaiting", {}, [self, br](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(br(vm, self).nwaiting());
            });
        if (name == "reset")
            return bind("reset", {}, [self, br](KiritoVM& vm, std::span<const Handle>) -> Handle {
                br(vm, self).resetBarrier();
                return vm.none();
            });
        if (name == "abort")
            return bind("abort", {}, [self, br](KiritoVM& vm, std::span<const Handle>) -> Handle {
                br(vm, self).abort();
                return vm.none();
            });
        if (name == "_getstate_")
            return bind("_getstate_", {}, [self, br](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(br(vm, self).id()));
            });
        if (name == "_setstate_")
            return bind("_setstate_", {"state"}, [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "_setstate_").require(1);
                KiritoDispatcher& d = requireDispatcher(vm, "parallel.Barrier");
                auto& v = static_cast<BarrierVal&>(vm.arena().deref(self));
                v.bar = d.barrierById(static_cast<uint64_t>(Value(vm, a[0]).asInt("Barrier _setstate_")));
                if (!v.bar) throw KiritoError("parallel.Barrier: cannot rebind (unknown id)");
                return vm.none();
            });
        return Object::getAttr(vm, self, name);
    }
};

// ---------------------------------------------------------------------------------------------------
// The module.
// ---------------------------------------------------------------------------------------------------
class ParallelModule : public NativeModule {
public:
    std::string name() const override { return "parallel"; }
    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
        // Cross-VM identity: rebuild a bare value; _setstate_(id) rebinds it to the shared C++ object.
        vm.registerDeserializer("Queue", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<QueueVal>());
        });
        vm.registerDeserializer("Lock", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<LockVal>());
        });
        vm.registerDeserializer("Event", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<EventVal>());
        });
        vm.registerDeserializer("Semaphore", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<SemaphoreVal>());
        });
        vm.registerDeserializer("Barrier", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<BarrierVal>());
        });

        m.fn("Queue", {{"maxsize", "Integer", vm.makeInt(0)}}, "Queue",
             [](KiritoVM& v, std::span<const Handle> a) -> Handle {
                 KiritoDispatcher& d = requireDispatcher(v, "parallel.Queue");
                 int64_t maxsize = a.empty() ? 0 : Args(v, a, "Queue")[0].asInt("Queue maxsize");
                 if (maxsize < 0) throw KiritoError("parallel.Queue: maxsize must be >= 0");
                 auto qv = std::make_unique<QueueVal>();
                 qv->q = d.createQueue(static_cast<std::size_t>(maxsize));
                 return v.alloc(std::move(qv));
             });
        m.fn("Lock", {}, "Lock", [](KiritoVM& v, std::span<const Handle>) -> Handle {
            KiritoDispatcher& d = requireDispatcher(v, "parallel.Lock");
            auto lv = std::make_unique<LockVal>();
            lv->lock = d.createLock();
            return v.alloc(std::move(lv));
        });
        m.fn("Event", {}, "Event", [](KiritoVM& v, std::span<const Handle>) -> Handle {
            KiritoDispatcher& d = requireDispatcher(v, "parallel.Event");
            auto ev = std::make_unique<EventVal>();
            ev->event = d.createEvent();
            return v.alloc(std::move(ev));
        });
        m.fn("Semaphore", {{"value", "Integer", vm.makeInt(1)}}, "Semaphore",
             [](KiritoVM& v, std::span<const Handle> a) -> Handle {
                 KiritoDispatcher& d = requireDispatcher(v, "parallel.Semaphore");
                 int64_t value = a.empty() ? 1 : Args(v, a, "Semaphore")[0].asInt("Semaphore value");
                 if (value < 0) throw KiritoError("parallel.Semaphore: value must be >= 0");
                 auto sv = std::make_unique<SemaphoreVal>();
                 sv->sem = d.createSemaphore(value);
                 return v.alloc(std::move(sv));
             });
        m.fn("Barrier", {{"parties", "Integer"}}, "Barrier",
             [](KiritoVM& v, std::span<const Handle> a) -> Handle {
                 KiritoDispatcher& d = requireDispatcher(v, "parallel.Barrier");
                 int64_t parties = Args(v, a, "Barrier")[0].asInt("Barrier parties");
                 if (parties < 1) throw KiritoError("parallel.Barrier: parties must be >= 1");
                 auto bv = std::make_unique<BarrierVal>();
                 bv->bar = d.createBarrier(parties);
                 return v.alloc(std::move(bv));
             });

        // spawn(fn, *args, **kwargs) -> Task. fn is passed directly; it is resolved in the worker by
        // its source location (see dispatcher.hpp), so it must be a Kirito function in a loadable file.
        m.kwfn("spawn", [](KiritoVM& v, std::span<const Handle> pos, std::span<const NamedArg> named) -> Handle {
            KiritoDispatcher& d = requireDispatcher(v, "parallel.spawn");
            if (pos.empty()) throw KiritoError("parallel.spawn: missing function argument");
            Object& o = v.arena().deref(pos[0]);
            if (o.kind() != ValueKind::Function)
                throw KiritoError("parallel.spawn: first argument must be a Kirito function "
                                  "(defined in a .ki file)");
            auto& kf = static_cast<KiFunction&>(o);
            std::string sf = kf.sourceFile;
            if (sf.empty() || sf == "<main>")
                throw KiritoError("parallel.spawn: the function must be defined in a loadable .ki file "
                                  "(its source is " + (sf.empty() ? std::string("<unknown>") : sf) + ")");
            uint32_t line = kf.def().span.line;
            uint32_t col = kf.def().span.col;
            std::string blob = packArgs(v, pos.subspan(1), named);
            uint64_t id = d.spawnTask(std::move(sf), line, col, std::move(blob));
            auto tv = std::make_unique<TaskVal>();
            tv->taskId = id;
            return v.alloc(std::move(tv));
        });

        m.fn("cpucount", {}, "Integer", [](KiritoVM& v, std::span<const Handle>) -> Handle {
            return v.makeInt(static_cast<int64_t>(KiritoDispatcher::cpuCount()));  // no dispatcher needed
        });
    }
};

// Install the `parallel` module on every VM the dispatcher owns (the main VM + each worker) and force
// its setup so the cross-VM deserializers are registered before any value is rebuilt. Defined here so
// it can see ParallelModule (dispatcher.hpp, included earlier, only declares this member).
inline void KiritoDispatcher::configureVM(KiritoVM& vm) {
    vm.setDispatcher(this);
    // Snapshot the config under the registry lock (this runs on a worker thread; the main thread may
    // concurrently addLibPath/setMaxCallDepth). Apply to the worker VM outside the lock.
    std::vector<std::string> paths;
    std::size_t depth;
    { std::lock_guard<std::mutex> lk(registryMutex_); paths = libPaths_; depth = maxCallDepth_; }
    for (const auto& p : paths) vm.addLibPath(p);
    if (depth) vm.setMaxCallDepth(depth);
    vm.install<ParallelModule>();
    vm.importModule("parallel");  // run setup() now -> registers the cross-VM deserializers
}

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

}  // namespace kirito

#endif
