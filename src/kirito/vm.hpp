#ifndef KIRITO_VM_HPP
#define KIRITO_VM_HPP

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fum/unordered_map.hpp"
#include "fum/unordered_set.hpp"
#include "arena.hpp"
#include "builtins.hpp"
#include "environment.hpp"
#include "handle.hpp"
#include "object.hpp"

namespace kirito {

namespace ast { struct Program; }
class NativeModule;
class KiritoDispatcher;  // multiprocessing coordinator (dispatcher.hpp); a bare VM has none
struct Proto;            // a compiled body (bytecode.hpp); the optional second execution engine

// One KiritoVM == one fully-encapsulated Kirito process. It owns the whole process state by
// composing its sub-objects: the value arena, the global (built-ins) environment, and interned
// singletons. No global/static mutable state, so independent VMs never interfere.
class KiritoVM {
public:
    KiritoVM() {
        // Publish self as the thread's active VM so const/no-arg protocol slots (Object::hash,
        // Object::equals) can reach interpreter services when the value is a user-class instance
        // (`_hash_`, `_eq_`). The share-nothing rule — one OS thread per VM at a time — makes this
        // safe: the pointer is scoped to whichever VM most recently entered on this thread.
        _activeStack().push_back(this);
        tempRoots_.reserve(1024);  // avoid reallocation churn on the hot RootScope path
        none_ = arena_.alloc(std::make_unique<NoneVal>());
        true_ = arena_.alloc(std::make_unique<BoolVal>(true));
        false_ = arena_.alloc(std::make_unique<BoolVal>(false));
        // A distinct sentinel marking an as-yet-unwritten slot-addressed local. Never reaches Kirito
        // code: LoadLocal/AssignLocal detect it and fall back to a name lookup. Kept as a GC root.
        undefined_ = arena_.alloc(std::make_unique<NoneVal>());
        global_ = arena_.alloc(std::make_unique<EnvValue>());
        smallInts_.reserve(kSmallIntHi - kSmallIntLo + 1);
        for (int64_t v = kSmallIntLo; v <= kSmallIntHi; ++v)
            smallInts_.push_back(arena_.alloc(std::make_unique<IntVal>(v)));
        installBuiltins();
        installStandardLibrary();
        buildBuiltinIndex();  // freeze the global/builtin scope into a name->slot table for LoadGlobal
    }

    ObjectArena& arena() { return arena_; }
    const ObjectArena& arena() const { return arena_; }

    // The GC-aware allocator: every Kirito value flows through here, so a collection can be
    // triggered (before the new object exists) when allocation pressure crosses the threshold.
    // Generational cadence: crossing the threshold runs a cheap MINOR (nursery) collection; every
    // kMajorEveryMinors-th collection is instead a full MAJOR, which reclaims old-generation garbage
    // and old-spanning dead cycles a minor leaves floating. An explicit collectGarbage() is a major.
    Handle alloc(std::unique_ptr<Object> obj) {
        if (gcEnabled_) {
            ++allocsSinceMajor_;
            if (++allocsSinceGc_ >= minorThreshold_) {
                // Two independent cadences. A minor costs O(young), so it runs on a small FIXED
                // nursery — often, but each one cheap and the same size every time. A major costs
                // O(live), so it runs on a live-proportional cadence, which is what keeps its cost
                // amortized to O(1) per allocation. Sharing one threshold (as this did) forced a
                // choice between frequent expensive majors and a nursery so large that a "minor"
                // cost nearly as much as a major — bimodal pauses, and the whole point of the
                // generational split thrown away.
                if (allocsSinceMajor_ >= gcThreshold_) collectGarbage();
                else minorCollect();
            }
        }
        return arena_.alloc(std::move(obj));
    }

    // Interned singletons.
    Handle none() const { return none_; }
    Handle undefined() const { return undefined_; }  // sentinel for an unwritten slot-addressed local
    Handle makeBool(bool v) const { return v ? true_ : false_; }

    // Value-construction helpers — also the embedding surface for C++ callers.
    Handle makeInt(int64_t v) {
        if (v >= kSmallIntLo && v <= kSmallIntHi) return smallInts_[v - kSmallIntLo];  // interned
        return alloc(std::make_unique<IntVal>(v));
    }
    Handle makeFloat(double v) { return alloc(std::make_unique<FloatVal>(v)); }
    Handle makeString(std::string v) { return alloc(std::make_unique<StrVal>(std::move(v))); }

    Handle global() const { return global_; }
    // Compile-time global resolution: builtinSlot(name) returns a fixed index into the global scope so
    // the compiler can emit a direct LoadGlobal, or -1 only if the name is not a global at all. A
    // standard builtin hits the frozen index table; an embedder-added global (registered after the
    // freeze) is found by its position in the global scope — stable because the scope only ever grows
    // by append — so it too resolves to LoadGlobal, never a name walk. globalSlot reads it positionally.
    int builtinSlot(const std::string& name) const {
        auto it = builtinIndex_.find(name);
        if (it != builtinIndex_.end()) return static_cast<int>(it->second);
        const auto& g = static_cast<const EnvValue&>(arena_.deref(global_));
        for (std::size_t i = 0; i < g.size(); ++i)
            if (g.nameAt(i) == name) return static_cast<int>(i);
        return -1;
    }
    Handle globalSlot(uint32_t i) const {
        return static_cast<const EnvValue&>(arena_.deref(global_)).at(i);
    }
    // A fresh scope whose parent is the given one (defaults to a module scope under global).
    Handle newScope(Handle parent) { return alloc(std::make_unique<EnvValue>(parent)); }
    // A module/file scope under global, with the per-file `arglist` and `argmain` bound into it.
    // `isMain` is true for a directly-run file (or the REPL), false for an imported module — so a
    // file can do `if argmain:` to run code only when executed directly. Defined in runtime.hpp
    // (it needs ListVal). The command-line arguments are set once by the embedder via setArgs().
    Handle newModuleScope(bool isMain = true);
    void setArgs(const std::vector<std::string>& args);

    // --- garbage collection ---
    // Temporary roots: handles held in C++ locals across an allocation must be protected here
    // (see RootScope below) so a mid-expression collection cannot reclaim them.
    void pushTemp(Handle h) { tempRoots_.push_back(h); }
    std::size_t tempMark() const { return tempRoots_.size(); }
    void popTempTo(std::size_t mark) { tempRoots_.resize(mark); }

    // Pinned roots: an out-of-order companion to tempRoots_. `pinHandle(h)` refcount-increments,
    // `unpinHandle(h)` decrements (removing at zero), and the GC treats every pinned handle as
    // reachable. Used by value.hpp's typed wrappers to keep freshly-allocated containers alive across
    // subsequent allocations without pinning to LIFO order — a wrapper's h_ can outlive whichever
    // frame allocated it (returned, stored in a Dict field, etc.).
    void pinHandle(Handle h) { ++pinnedRoots_[h]; }
    void unpinHandle(Handle h) {
        auto it = pinnedRoots_.find(h);
        if (it == pinnedRoots_.end()) return;   // no-op if not pinned (defensive)
        if (--it->second <= 0) pinnedRoots_.erase(it);
    }

    // Auxiliary root regions: the bytecode engine's operand stack lives in a C++ vector held by the
    // running BytecodeVM, not in tempRoots_. Registering that vector here (RAII, in execution order)
    // makes every operand it holds a GC root for as long as the frame is live. Nested frames each
    // register their own. Pointers stay valid because each frame owns its vector for its lifetime.
    void pushAuxRoots(const std::vector<Handle>* v) { auxRoots_.push_back(v); }
    void popAuxRoots() { auxRoots_.pop_back(); }

    // --- bytecode engine (the sole execution engine, behind the AST boundary) ---
    // A compiled Proto's literal constants are pinned here so they live for the VM's lifetime (the
    // Proto is cached as long as its AST is retained), giving O(1) LoadConst with no re-allocation.
    void pinConst(Handle h) { bytecodeConsts_.push_back(h); }
    // Per-body Proto cache, keyed by the body's stable address: each body is compiled lazily on first
    // execution and the Proto reused thereafter. The compiler handles every body (a genuine program
    // error propagates as a KiritoError instead), so a cached entry is always a real Proto.
    bool protoTried(const void* key) const { return protoCache_.find(key) != protoCache_.end(); }
    const Proto* protoGet(const void* key) const {
        auto it = protoCache_.find(key);
        return it != protoCache_.end() ? it->second.get() : nullptr;
    }
    void protoPut(const void* key, std::unique_ptr<Proto> p) { protoCache_[key] = std::move(p); }

    // Every GC root, in one place, so the minor and major collectors agree on reachability: interned
    // singletons, globals, the module/path caches, the REPL scope, `arglist`, the class registry, temp
    // roots, C++-side pinned wrappers, the pinned bytecode literal pool, and every live operand stack.
    template <class F>
    void forEachRoot(F&& f) const {
        f(none_); f(true_); f(false_); f(undefined_); f(global_);
        for (Handle h : smallInts_) f(h);
        if (replScopeReady_) f(replScope_);
        for (const auto& [name, h] : moduleCache_) f(h);
        for (const auto& [p, h] : pathCache_) f(h);
        if (arglist_.slot) f(arglist_);  // the per-file `arglist`, shared by every module scope
        for (const auto& [name, h] : classRegistry_) f(h);  // keep deserializable classes alive
        for (Handle h : tempRoots_) f(h);
        for (const auto& [h, cnt] : pinnedRoots_) { (void)cnt; f(h); }  // C++-side pinned wrappers
        for (Handle h : bytecodeConsts_) f(h);              // pinned bytecode literal pool
        for (const std::vector<Handle>* region : auxRoots_)  // live bytecode operand stacks
            for (Handle h : *region) f(h);
    }

    // MAJOR collection: the full scan-everything mark-sweep. Reclaims young AND old dead objects,
    // including old-spanning dead cycles that a minor leaves floating. Explicit collectGarbage() is a
    // major, and the automatic cadence promotes every kMajorEveryMinors-th collection to one.
    void collectGarbage() {
        GcTimer timer(majorNanos_);
        arena_.clearMarks();
        std::vector<Handle> work;
        auto enqueue = [&](Handle h) { if (arena_.markIfUnmarked(h)) work.push_back(h); };
        forEachRoot(enqueue);
        std::vector<Handle> childbuf;
        while (!work.empty()) {
            Handle h = work.back();
            work.pop_back();
            childbuf.clear();
            arena_.deref(h).children(childbuf);
            for (Handle c : childbuf) enqueue(c);
        }
        arena_.sweep();
        ++majorCount_;
        allocsSinceMajor_ = 0;
        allocsSinceGc_ = 0;
        // Adaptive retarget: the next MAJOR after ~4× the live set (floored). Proportional to live is
        // what amortizes a major's O(live) cost to O(1) per allocation, so this is deliberately NOT
        // capped: a hard ceiling would make a genuinely large heap pay a full mark+sweep every fixed
        // number of allocations, which is the one way to turn this quadratic. The nursery bounds the
        // PAUSE (see kMinorNursery); this bounds the total WORK. Pinned off by setGcThreshold().
        // Measured after the sweep, so `live` is the true surviving set, not floating garbage.
        if (gcAdaptive_)
            gcThreshold_ = std::max(kGcThresholdFloor, arena_.liveCount() * 4);
    }

    // MINOR collection: scans only the young generation. Marks young objects reachable from the roots
    // and from the remembered set (old objects the write barrier flagged as pointing into the nursery),
    // stopping the trace at old-object boundaries; then frees dead young objects and promotes the
    // survivors in place. O(young + remembered), not O(heap) — the churn-reclaiming fast path.
    void minorCollect() {
        GcTimer timer(minorNanos_);
        arena_.clearYoungMarks();
        std::vector<Handle> work;
        auto enqueue = [&](Handle h) { if (arena_.markIfYoungUnmarked(h)) work.push_back(h); };
        forEachRoot(enqueue);
        std::vector<Handle> childbuf;
        // Seed from the remembered set: an old object's young children are roots for this minor.
        for (Object* c : arena_.remembered()) {
            childbuf.clear();
            c->children(childbuf);
            for (Handle ch : childbuf) enqueue(ch);
        }
        while (!work.empty()) {
            Handle h = work.back();
            work.pop_back();
            childbuf.clear();
            arena_.deref(h).children(childbuf);
            for (Handle c : childbuf) enqueue(c);
        }
        arena_.sweepYoung();
        arena_.resetRemembered();
        ++minorCount_;
        allocsSinceGc_ = 0;   // NOT allocsSinceMajor_: a major is spaced by allocation, not by minors
    }

    // Pin an explicit collection threshold; disables the adaptive retarget so the value sticks
    // exactly (tests use setGcThreshold(1) to collect on every allocation).
    void setGcThreshold(std::size_t n) {
        // Pins BOTH cadences, preserving this call's original meaning: a collection every n
        // allocations, every kMajorEveryMinors-th of them a major. setGcThreshold(1) therefore still
        // collects on every allocation (the write-barrier soak depends on it).
        minorThreshold_ = n;
        gcThreshold_ = n * kMajorEveryMinors;
        gcAdaptive_ = false;
    }
    void setGcEnabled(bool on) { gcEnabled_ = on; }
    bool gcEnabled() const { return gcEnabled_; }   // current setting (for a scoped GcPauseScope)
    std::size_t liveCount() const { return arena_.liveCount(); }

    // --- GC statistics (the `--gc-stats` yardstick) — counting is free; timing reads a steady clock
    // twice per collection, immaterial since collections are infrequent. `youngCount` is the current
    // nursery occupancy; `arenaCapacity` the slot high-water mark. ---
    std::size_t gcMinorCount() const { return minorCount_; }
    std::size_t gcMajorCount() const { return majorCount_; }
    uint64_t gcMinorNanos() const { return minorNanos_; }
    uint64_t gcMajorNanos() const { return majorNanos_; }
    std::size_t youngCount() const { return arena_.youngCount(); }
    std::size_t arenaCapacity() const { return arena_.capacity(); }

    // Call-depth guard: the VM still recurses on the native C++ stack (one nested call/compile
    // descends into BytecodeVM::run again), so unbounded Kirito recursion would overflow it and crash
    // the host. A RAII CallGuard throws a catchable error instead. TWO limits, whichever trips first:
    //   1. a fixed call COUNT (maxCallDepth_) — the cheap common-case bound for shallow Kirito frames;
    //   2. actual native STACK USAGE (maxStackBytes_) — because a call routed through a native
    //      higher-order builtin (`sorted(key=g)`, `xs.sort(key=g)`, `apply(g)`, `min/max(key=g)`)
    //      carries a much deeper C++ frame, so a *count* calibrated for a bare Kirito call overflows
    //      the stack long before it fires (A04-1: a hard SIGSEGV). We estimate usage from the address
    //      of a stack local relative to a base captured at the outermost call of this run.
    void enterCall() {
        char probe;
        auto cur = reinterpret_cast<std::uintptr_t>(&probe);
        if (callDepth_ == 0) {
            stackBase_ = cur;                          // top of this run's native stack
        } else {
            // uintptr_t arithmetic (not pointer subtraction of unrelated objects) so it stays defined.
            std::uintptr_t used = stackBase_ > cur ? stackBase_ - cur : cur - stackBase_;
            if (used > maxStackBytes_)
                throw KiritoError("maximum recursion depth exceeded");
        }
        if (++callDepth_ > maxCallDepth_) {
            --callDepth_;
            throw KiritoError("maximum recursion depth exceeded");
        }
    }
    void leaveCall() { --callDepth_; }
    void setMaxCallDepth(std::size_t n) { maxCallDepth_ = n; }
    void setMaxStackBytes(std::size_t n) { maxStackBytes_ = n; }

    // Expose a C++ callable (or any value) as a Kirito global — the simplest extension point.
    void registerGlobal(const std::string& name, Handle value) {
        static_cast<EnvValue&>(arena_.deref(global_)).define(arena_, name, value);
    }

    // --- extension / module API (defined in runtime.hpp) ---
    using ModuleFactory = std::function<Handle(KiritoVM&)>;
    void registerModule(std::string name, ModuleFactory factory);
    template <class T> void install();              // install a NativeModule subclass (one-liner)
    void installStandardLibrary();                  // register the bundled stdlib modules
    // Register a module whose body is Kirito source compiled into the binary (a "frozen" module):
    // its top-level bindings become the module's members, evaluated once per VM on first import.
    void registerSourceModule(std::string name, std::string_view source);
    Handle importModule(const std::string& name);   // native module, then <name>.ki on the lib path

    // Directories searched (in order) when importing a `.ki` module file.
    void addLibPath(std::string dir) { libPaths_.push_back(std::move(dir)); }
    const std::vector<std::string>& libPaths() const { return libPaths_; }

    // The chunk (file / frozen-module name) currently being evaluated. Functions stamp this at
    // definition time so an error escaping a later call is attributed to its defining file, not to
    // whichever script invoked it. RAII scope keeps the stack balanced across nested imports.
    class ChunkFileScope {
    public:
        ChunkFileScope(KiritoVM& vm, std::string f) : vm_(vm) { vm_.chunkFiles_.push_back(std::move(f)); }
        ~ChunkFileScope() { vm_.chunkFiles_.pop_back(); }
        ChunkFileScope(const ChunkFileScope&) = delete;
        ChunkFileScope& operator=(const ChunkFileScope&) = delete;
    private:
        KiritoVM& vm_;
    };
    const std::string& currentChunkFile() const {
        static const std::string empty;
        return chunkFiles_.empty() ? empty : chunkFiles_.back();
    }

    // VM-local traceback of the most recent error (the call chain it unwound through, innermost-first).
    // Filled by the bytecode VM as an exception escapes its frames; read by `sys.traceback()` inside a
    // `catch`, and by the CLI to print a traceback for an uncaught error.
    void setLastTraceback(std::vector<TraceFrame> tb) { lastTraceback_ = std::move(tb); }
    const std::vector<TraceFrame>& lastTraceback() const { return lastTraceback_; }

    // Class + deserializer registries (used by serialize/dump to reconstruct objects by class name).
    void registerClass(const std::string& name, Handle cls) { classRegistry_[name] = cls; }
    // The class registered under `name`, or nullptr if none.
    const Handle* findClass(const std::string& name) const {
        auto it = classRegistry_.find(name);
        return it != classRegistry_.end() ? &it->second : nullptr;
    }
    // A native type opts into deserialization by registering reconstructor(vm, state) -> object.
    void registerDeserializer(std::string name, std::function<Handle(KiritoVM&, Handle)> fn) {
        deserializers_[std::move(name)] = std::move(fn);
    }
    const std::function<Handle(KiritoVM&, Handle)>* findDeserializer(const std::string& name) const {
        auto it = deserializers_.find(name);
        return it != deserializers_.end() ? &it->second : nullptr;
    }

    std::string stringify(Handle h) const {
        StringifyCtx ctx{arena_, {}, const_cast<KiritoVM*>(this)};
        return arena_.deref(h).str(ctx);
    }

    // --- multiprocessing hookup (dispatcher.hpp) ---
    // The dispatcher coordinating worker VMs. Null for a bare/embedded VM (then the `parallel` module
    // is absent); the `ki` interpreter builds every VM through a KiritoDispatcher.
    void setDispatcher(KiritoDispatcher* d) { dispatcher_ = d; }
    KiritoDispatcher* dispatcher() const { return dispatcher_; }
    // True while a worker VM is re-evaluating a spawned function's source-file top level to rebuild its
    // closure (the "bootstrap" phase). `parallel.spawn` refuses to run in this window: a top-level
    // spawn re-executed by every worker load is an unbounded fork bomb (guard such calls with
    // `if argmain:`). The main entry run does NOT set this — its first spawn is the legitimate kickoff.
    void setBootstrapping(bool b) { bootstrapping_ = b; }
    bool bootstrapping() const { return bootstrapping_; }
    // The retained AST of a chunk by its file/chunk name (set when evaluated). Used by the dispatcher
    // to reconstruct a spawned function by its source span in a worker VM. Null if not loaded here.
    const ast::Program* programForFile(const std::string& f) const {
        auto it = programByFile_.find(f);
        return it != programByFile_.end() ? it->second : nullptr;
    }

    // Lex, parse, and evaluate a chunk of Kirito source in a fresh module scope; returns the
    // handle of the last expression's value (or None). Defined in runtime.hpp.
    Handle runSource(std::string_view source, std::string_view chunkName = "<main>");

    // Like runSource but reuses one persistent module scope across calls, so bindings survive
    // between lines — for the REPL. Defined in runtime.hpp.
    Handle runRepl(std::string_view source);

    // Keep a parsed chunk alive for the VM's lifetime so function bodies (referenced by closures
    // that may outlive the call) never dangle. Defined in runtime.hpp (needs a complete Program).
    void retainChunk(std::unique_ptr<ast::Program> chunk);

    // Register built-in globals (len, ...). Defined in runtime.hpp; called from the constructor.
    void installBuiltins();

    // Snapshot the global scope's bindings into a name->index table, once, after all builtins are
    // installed. Every builtin keeps its position (the global scope only ever grows by append), so a
    // compile-time LoadGlobal(index) reads it in O(1) with no name walk.
    void buildBuiltinIndex() {
        const auto& g = static_cast<const EnvValue&>(arena_.deref(global_));
        uint32_t i = 0;
        for (const auto& [name, h] : g.locals()) { (void)h; builtinIndex_.emplace(name, i++); }
    }

    // Shared lex->parse->retain->evaluate against a given scope. Defined in runtime.hpp.
    // `indexTopLevel` gives the scope's top-level bindings fixed slots + LoadVar addressing (a real
    // module/script scope); it is left false for the REPL's persistent dynamic scope and bare evals.
    Handle evalIn(std::string_view source, Handle scope, std::string_view chunkName = {},
                  bool indexTopLevel = false);

    ~KiritoVM();  // out-of-line so unique_ptr<ast::Program> sees a complete type

    // The active VM on the current thread — used by the InstanceValue hash/equals slot so a
    // `_hash_` or `_eq_` method defined on a user class can be invoked from const, no-arena
    // protocol methods that Dict/Set call into. `nullptr` outside any live VM.
    static KiritoVM* activeVM() { return _activeStack().empty() ? nullptr : _activeStack().back(); }

private:
    // A thread-local stack of the live VMs entered on this thread (most-recent at the back is active).
    // A plain prev/restore pair dangled on non-LIFO teardown — destroying an outer VM before an inner
    // one restored `_activeVM` to the already-freed outer (A06-1). The stack tolerates any destruction
    // order: each dtor removes exactly itself, and activeVM() is always a still-live VM (or null).
    static std::vector<KiritoVM*>& _activeStack() {
        static thread_local std::vector<KiritoVM*> s;
        return s;
    }
    ObjectArena arena_;
    Handle none_;
    Handle true_;
    Handle false_;
    Handle undefined_;
    Handle global_;
    fum::unordered_map<std::string, uint32_t> builtinIndex_;  // builtin name -> global slot (LoadGlobal)
    std::vector<std::unique_ptr<ast::Program>> chunks_;
    std::vector<std::unique_ptr<NativeModule>> nativeModules_;
    fum::unordered_map<std::string, ModuleFactory> moduleFactories_;
    fum::unordered_map<std::string, Handle> moduleCache_;   // keyed by module name
    fum::unordered_map<std::string, Handle> pathCache_;     // keyed by resolved absolute path
    // chunk file/name -> its retained Program (the AST lives in chunks_; this just indexes it by file
    // so the dispatcher can find a spawned function's definition by source span in a worker VM).
    fum::unordered_map<std::string, const ast::Program*> programByFile_;
    KiritoDispatcher* dispatcher_ = nullptr;
    bool bootstrapping_ = false;  // worker re-evaluating a spawned fn's module top level (A19-1 guard)
    // Circular-import guard: names/paths currently mid-load, and the active chain (for diagnostics).
    // A module is published to moduleCache_ only after its body finishes, so a re-entrant import of
    // an in-progress module is a cycle — detected here instead of recursing until the stack blows.
    fum::unordered_set<std::string> importing_;
    std::vector<std::string> importStack_;
    std::vector<std::string> libPaths_;
    std::vector<std::string> chunkFiles_;  // see ChunkFileScope
    std::vector<TraceFrame> lastTraceback_;  // call chain of the most recent error (see setLastTraceback)
    Handle replScope_{};
    bool replScopeReady_ = false;
    Handle arglist_{};  // the command-line arguments as a List, bound as `arglist` in every module scope
    std::vector<Handle> tempRoots_;
    // Pinned C++-side roots — see pinHandle/unpinHandle. Refcount-map keyed by handle so the same h
    // can be pinned by multiple wrappers (shared_ptr<Pin> copies).
    fum::unordered_map<Handle, int> pinnedRoots_;
    // Class + deserializer registries for object-graph deserialization (serialize/dump): a class is
    // registered by name when defined, so a serialized instance can be reconstructed by looking its
    // class up here; a native type can register a reconstructor(state)->object to participate.
    fum::unordered_map<std::string, Handle> classRegistry_;
    fum::unordered_map<std::string, std::function<Handle(KiritoVM&, Handle)>> deserializers_;
    std::vector<Handle> smallInts_;
    static constexpr int64_t kSmallIntLo = -256;
    static constexpr int64_t kSmallIntHi = 256;
    // --- bytecode engine state ---
    std::vector<const std::vector<Handle>*> auxRoots_;   // live operand stacks (GC roots)
    std::vector<Handle> bytecodeConsts_;                 // pinned literal pool of every compiled Proto
    fum::unordered_map<const void*, std::unique_ptr<Proto>> protoCache_;  // per-body compiled cache
    // RAII: accumulate one collection's wall time into a counter. Two steady-clock reads per GC — GCs
    // are infrequent, so this never shows up in throughput; it's the yardstick for the churn work.
    struct GcTimer {
        uint64_t& sink;
        std::chrono::steady_clock::time_point t0;
        explicit GcTimer(uint64_t& s) : sink(s), t0(std::chrono::steady_clock::now()) {}
        ~GcTimer() {
            sink += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
        }
        GcTimer(const GcTimer&) = delete;
        GcTimer& operator=(const GcTimer&) = delete;
    };
    std::size_t allocsSinceGc_ = 0;      // since the last collection of either kind (minor trigger)
    std::size_t gcThreshold_ = 100000;   // allocations between MAJOR collections (adaptive; see below)
    // The nursery: allocations between MINOR collections. Fixed and small, because a minor costs
    // O(young) — bounding the nursery bounds the pause, so the pause stream is many uniform blips
    // instead of a few lumps, and the arena's high-water is bounded near live + this.
    //
    // 32768 is the measured knee, not a guess (tools/tests/bench, intra-run per-rep CV — the jitter a
    // user actually feels, as opposed to whole-run wall time, which averages it away). Sweeping
    // 16384/32768/65536/131072: smaller keeps steadying dict_ops but costs sum_loop 22% throughput,
    // because a minor rescans every remembered old container IN FULL, and a growing List re-remembers
    // itself on every append — so halving the nursery doubles the rescan of the same list. (Card
    // marking is the real answer to that and is a much larger change.) Larger walks back toward the
    // old bimodal pauses. At 32768, against 1.15.0: no workload regressed, sort and string_ops each
    // ~25% faster, per-rep CV 0.265->0.055 (sum_loop) / 0.509->0.374 (dict_ops) / 0.589->0.410
    // (string_ops), arena high-water 1.6-6.5x smaller.
    // setGcThreshold() pins BOTH cadences, so setGcThreshold(1) still collects on every allocation.
    static constexpr std::size_t kMinorNursery = 32768;
    std::size_t minorThreshold_ = kMinorNursery;
    // How many minors' worth of allocation a major is spaced by when a threshold is PINNED (the
    // adaptive path uses the live set instead). Keeps setGcThreshold(n)'s old meaning: a collection
    // every n allocations, every 8th of them a major.
    static constexpr std::size_t kMajorEveryMinors = 8;
    std::size_t allocsSinceMajor_ = 0;
    std::size_t minorCount_ = 0;
    std::size_t majorCount_ = 0;
    uint64_t minorNanos_ = 0;
    uint64_t majorNanos_ = 0;
    // When adaptive (the default — nobody pinned a threshold via setGcThreshold), each MAJOR retargets
    // the next one to a multiple of the set that survived it. Proportional to live is what amortizes a
    // major's O(live) cost to O(1) per allocation. An explicit setGcThreshold() pins it and turns this
    // off (tests rely on e.g. setGcThreshold(1) collecting on every allocation).
    bool gcAdaptive_ = true;
    // The floor only binds when the live set is small (< floor/4): allocation-heavy but low-live
    // workloads — tight loops and deep recursion (fib) — which churn transient boxes while keeping
    // almost nothing live. It must stay a comfortable multiple of kMinorNursery, or the major trigger
    // fires first every time and minors never run at all (measured: a floor at or below the nursery
    // turns dict_ops major-only and its CV back up to 0.785 — worse than before the generational GC).
    // 131072 = 4 nurseries. Larger-live workloads never reach it (the adaptive 4x live dominates).
    // Note majors are NOT rationed beyond this: the floating garbage a promote-on-first-survival
    // policy leaves behind means rarer majors grow the arena, and a big arena costs more to sweep
    // than it saves in skipped majors — spacing majors 8x further apart measured WORSE on every
    // workload (sum_loop 623M vs 483M, arena 18M vs 4M). setGcThreshold() still pins an exact value.
    static constexpr std::size_t kGcThresholdFloor = 131072;
    bool gcEnabled_ = true;
    std::size_t callDepth_ = 0;
    // Conservative default for an 8 MB stack with deep per-call expression nesting; embedders with
    // a smaller stack can lower it via setMaxCallDepth(). Sanitizer builds use far larger native
    // frames (redzones + shadow), so the same Kirito depth overflows the stack long before this
    // guard would fire — drop the default under ASan so the guard still throws cleanly.
#if defined(KIRITO_SANITIZER_BUILD)
    std::size_t maxCallDepth_ = 500;
    // Sanitizer frames are far larger (redzones + shadow), so bound native stack usage tightly too.
    std::size_t maxStackBytes_ = 2u * 1024 * 1024;
#else
    std::size_t maxCallDepth_ = 3000;
    // ~6 MB of an 8 MB stack: leaves headroom for the frames below the captured base + the throw path,
    // and trips a deep-native-frame recursion (A04-1) before it can overflow.
    std::size_t maxStackBytes_ = 6u * 1024 * 1024;
#endif
    std::uintptr_t stackBase_ = 0;   // native-stack top captured at the outermost (depth-0) call
};

// RAII call-depth guard: increments on entry, decrements on scope exit (even when unwinding).
struct CallGuard {
    KiritoVM& vm;
    explicit CallGuard(KiritoVM& v) : vm(v) { vm.enterCall(); }
    ~CallGuard() { vm.leaveCall(); }
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;
};

// RAII protector: handles added here are GC roots until the scope ends. Use wherever the
// evaluator holds a handle in a C++ local while doing more work that may allocate.
struct RootScope {
    KiritoVM& vm;
    std::size_t mark;
    explicit RootScope(KiritoVM& v) : vm(v), mark(v.tempMark()) {}
    ~RootScope() { vm.popTempTo(mark); }
    RootScope(const RootScope&) = delete;
    RootScope& operator=(const RootScope&) = delete;
    Handle add(Handle h) {
        vm.pushTemp(h);
        return h;
    }
    // Root every handle in a snapshot vector at once — for the apply/sort methods that copy a
    // container's elements into a local vector and then run user code (fn / key / _lt_) that may
    // clear the source + allocate, which would otherwise sweep the not-yet-consumed snapshot entries.
    void addAll(const std::vector<Handle>& hs) { for (Handle h : hs) vm.pushTemp(h); }
};

// RAII: pause automatic garbage collection for a critical section and RESTORE the previous setting
// on scope exit (even when unwinding). Nests correctly — an inner scope restores the outer's state,
// not unconditionally "on". Use to batch allocations where a mid-run sweep would be wasteful; prefer
// `RootScope` / `PinnedHandle` (value.hpp) to keep *specific* values alive — reach for this only to
// suspend collection wholesale.
struct GcPauseScope {
    KiritoVM& vm;
    bool prev;
    explicit GcPauseScope(KiritoVM& v) : vm(v), prev(v.gcEnabled()) { vm.setGcEnabled(false); }
    ~GcPauseScope() { vm.setGcEnabled(prev); }
    GcPauseScope(const GcPauseScope&) = delete;
    GcPauseScope& operator=(const GcPauseScope&) = delete;
};

}  // namespace kirito

#endif
