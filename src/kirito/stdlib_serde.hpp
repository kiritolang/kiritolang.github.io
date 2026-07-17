#ifndef KIRITO_STDLIB_SERDE_HPP
#define KIRITO_STDLIB_SERDE_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fum/unordered_map.hpp"
#include "builtins.hpp"
#include "class_value.hpp"
#include "collections.hpp"
#include "environment.hpp"
#include "function.hpp"
#include "locals.hpp"
#include "module.hpp"
#include "native.hpp"

namespace kirito {

// Shared object-graph (de)serialization core for the `serialize` (human-readable text) and `dump`
// (compact binary) modules. Both preserve shared references and cycles in exactly the same way —
// they differ ONLY in how the flat object table below is encoded to / decoded from bytes. The graph
// walk (identity ids, cycle termination, supported-type knowledge) and the two-pass reconstruction
// live here once; each module supplies just its byte format.
namespace serde {

// Format-neutral view of one serialized object: a kind tag, the scalar payload (only the field
// matching `tag` is meaningful), and — for containers — the ids of referenced objects (List/Set: the
// elements; Dict: key0, val0, key1, val1, …). Tag values are the binary wire tags too.
// Object: a user-class instance flattened by attributes — `s` is the class name, `links` are
//   key0,val0,key1,val1,… (keys are String ids). Reconstructed by looking the class up by name and
//   restoring the attributes.
// Stateful: an instance whose class defines `_getstate_`/`_setstate_` — `s` is the class/type name,
//   `links` is a single id pointing at the value `_getstate_` returned; on load the object is created
//   and `_setstate_(state)` is called.
// Function: a Kirito function value, self-contained — `s` is its verbatim source text, `links` are
//   name/value id pairs (like a Dict) for its captured free variables (closure); re-parsed on load. `i`
//   is 0 (a function has no definition-time free variables — its body runs only when called).
// Class: a Kirito class value — `s` is its verbatim `class ...:` source, `s2` its name, `links` the
//   captured free-variable name/value pairs (EAGER ones first), and `i` the eager count (free variables
//   the base/class-var initializers need at build time; the rest are lazy method references). Re-run on
//   load, which also re-registers the class so its instances deserialize.
// Module: a standard/user module — `s` is its import name; reconstructed by re-importing it (not by
//   value), so a serialized function that uses `math`/`json`/… reconnects to the loading VM's module.
enum class Tag : uint8_t {
    None = 0, Bool = 1, Integer = 2, Float = 3, String = 4, List = 5, Dict = 6, Set = 7,
    Object = 8, Stateful = 9, Function = 10, Class = 11, Module = 12
};

struct Node {
    Tag tag = Tag::None;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    std::string s2;   // Class: the class name (Function/others leave it empty)
    std::vector<uint32_t> links;
};

// The value of a free variable `name` looked up from `scope`, but ONLY when it is bound in a NON-global
// scope — a genuine closure capture that must travel with a serialized function/class. A name resolved
// in the global/builtin scope (or unbound) returns nullopt: the loading VM re-resolves it to its own
// builtin, so it need not be serialized. This is the "std reimport / user travels" boundary in action.
inline std::optional<Handle> closureCapture(KiritoVM& vm, Handle scope, const std::string& name) {
    Handle cur = scope;
    while (cur.slot) {
        if (cur == vm.global()) return std::nullopt;   // reached builtins -> not a captured variable
        const auto& e = static_cast<const EnvValue&>(vm.arena().deref(cur));
        if (const Handle* h = e.findLocal(name)) return *h;
        if (!e.hasParent()) return std::nullopt;
        cur = e.parent();
    }
    return std::nullopt;
}

// The bound method handle for `name` if `h`'s object defines it as a callable, else nullopt. Works
// for user instances (via the class chain) and native objects (which expose methods through getAttr).
inline std::optional<Handle> serdeMethod(KiritoVM& vm, Handle h, const char* name) {
    Object& o = vm.arena().deref(h);
    if (auto* inst = dynamic_cast<InstanceValue*>(&o))
        if (!inst->findMethod(vm.arena(), name)) return std::nullopt;
    try {
        Handle m = o.getAttr(vm, h, name);
        ValueKind k = vm.arena().deref(m).kind();
        if (k == ValueKind::Function || k == ValueKind::NativeFunction) return m;
    } catch (const KiritoError&) {
    }
    return std::nullopt;
}

// Flatten a Kirito function value into `n`: its verbatim source plus a snapshot of the free variables
// it captures (each recursively visited). Deliberately a SEPARATE function, not inline in `visit`'s
// switch below: its locals (a NameSet, vectors) would otherwise inflate the per-node stack frame of the
// recursive walk, so a deeply nested PLAIN-DATA graph would overflow the native stack before the depth
// guard could fire. Here they live only on the (rare) function-node frame.
inline void flattenFunction(KiritoVM& vm, KiFunction& fn, Node& n, RootScope& roots,
                            const std::function<uint32_t(Handle)>& visit, const char* verb) {
    if (fn.def().source.empty())
        throw KiritoError(std::string("cannot ") + verb + " this function: its source text was not "
            "captured (functions defined inside an f-string are not serializable)");
    n.tag = Tag::Function;
    n.s = fn.def().source;
    n.i = 0;  // a function has no definition-time (eager) free variables
    NameSet fvSet = freeVariables(fn.def());
    std::vector<std::string> names(fvSet.begin(), fvSet.end());
    std::sort(names.begin(), names.end());   // deterministic output
    for (const auto& fv : names) {
        auto val = closureCapture(vm, fn.closure(), fv);
        if (!val) continue;                  // a builtin/global — re-resolved on load
        n.links.push_back(visit(roots.add(vm.makeString(fv))));
        n.links.push_back(visit(*val));
    }
}

// Flatten a Kirito class value into `n`: its source, name, and free-variable snapshot with the EAGER
// ones (base + class-variable initializers) first and `n.i` counting them. Separate from `visit` for
// the same stack-frame reason as flattenFunction.
inline void flattenClass(KiritoVM& vm, ClassValue& cls, Node& n, RootScope& roots,
                         const std::function<uint32_t(Handle)>& visit, const char* verb) {
    if (!cls.def || cls.def->source.empty())
        throw KiritoError(std::string("cannot ") + verb + " class '" + cls.name +
                          "': its source text was not captured");
    n.tag = Tag::Class;
    n.s = cls.def->source;
    n.s2 = cls.name;
    NameSet allSet = freeVariables(*cls.def);
    NameSet eagerSet = eagerFreeVariables(*cls.def);
    std::vector<std::string> eager, lazy;
    for (const auto& fv : allSet) (eagerSet.count(fv) ? eager : lazy).push_back(fv);
    std::sort(eager.begin(), eager.end());
    std::sort(lazy.begin(), lazy.end());
    uint32_t eagerCount = 0;
    for (const auto& fv : eager) {
        auto val = closureCapture(vm, cls.closure, fv);
        if (!val) continue;
        n.links.push_back(visit(roots.add(vm.makeString(fv))));
        n.links.push_back(visit(*val));
        ++eagerCount;
    }
    for (const auto& fv : lazy) {
        auto val = closureCapture(vm, cls.closure, fv);
        if (!val) continue;
        n.links.push_back(visit(roots.add(vm.makeString(fv))));
        n.links.push_back(visit(*val));
    }
    n.i = static_cast<int64_t>(eagerCount);
}

// Walk the graph rooted at `root`, giving every reachable object an id by identity (a value reachable
// by two paths is recorded once; an id is reserved before recursing so cycles terminate) and flatten
// it into a Node table. `verb` ("serialize" / "dump") names the operation in error messages. Returns
// the table and the root's id. Supported kinds: None/Bool/Integer/Float/String/Bytes/List/Dict/Set,
// user `class` instances (by attributes, or via the _getstate_/_setstate_ protocol), serializable
// native value types (Matrix/Complex/Tensor/DateTime/Random), and Function/Class/Module values.
inline std::pair<std::vector<Node>, uint32_t> flatten(KiritoVM& vm, Handle root, const char* verb) {
    fum::unordered_map<const Object*, uint32_t> ids;
    std::vector<Node> nodes;
    RootScope roots(vm);   // keep synthesized attribute keys + _getstate_ results alive during the walk
    int depth = 0;
    std::function<uint32_t(Handle)> visit = [&](Handle h) -> uint32_t {
        const Object* op = &vm.arena().deref(h);
        auto found = ids.find(op);
        if (found != ids.end()) return found->second;
        uint32_t id = static_cast<uint32_t>(nodes.size());
        ids[op] = id;
        nodes.emplace_back();   // reserve the slot first, so a reference back here (a cycle) resolves
        // Guard against overflowing the native stack on a pathologically deep graph — the walk must
        // THROW well before the recursion exhausts the ~8 MB stack. The ceiling is a frame-count proxy
        // for stack usage, so it carries margin below the measured overflow point (~9700 frames in a
        // debug build); real data never nests thousands deep (JSON parsers cap near 1000). ASan enlarges
        // every frame, so the walk overflows far sooner there — use a much smaller ceiling.
#if defined(KIRITO_SANITIZER_BUILD)
        constexpr int kMaxFlattenDepth = 1500;
#else
        constexpr int kMaxFlattenDepth = 8000;
#endif
        if (++depth > kMaxFlattenDepth)
            throw KiritoError(std::string("structure too deeply nested to ") + verb);
        Node n;
        Object& o = vm.arena().deref(h);
        switch (o.kind()) {
            case ValueKind::None: { n.tag = Tag::None; } break;
            case ValueKind::Bool: { n.tag = Tag::Bool; n.b = static_cast<BoolVal&>(o).value(); } break;
            case ValueKind::Integer: { n.tag = Tag::Integer; n.i = static_cast<IntVal&>(o).value(); } break;
            case ValueKind::Float: { n.tag = Tag::Float; n.f = static_cast<FloatVal&>(o).value(); } break;
            case ValueKind::String: { n.tag = Tag::String; n.s = static_cast<StrVal&>(o).value(); } break;
            case ValueKind::List: {
                n.tag = Tag::List;
                for (Handle e : static_cast<ListVal&>(o).elems) n.links.push_back(visit(e));
            } break;
            case ValueKind::Dict: {
                n.tag = Tag::Dict;
                auto& d = static_cast<DictVal&>(o);
                for (Handle k : d.keys()) {
                    uint32_t kid = visit(k);
                    uint32_t vid = visit(*d.find(vm.arena(), k));
                    n.links.push_back(kid);
                    n.links.push_back(vid);
                }
            } break;
            case ValueKind::Set: {
                n.tag = Tag::Set;
                for (Handle e : static_cast<SetVal&>(o).items()) n.links.push_back(visit(e));
            } break;
            case ValueKind::Instance: {
                InstanceValue* inst = dynamic_cast<InstanceValue*>(&o);
                // A USER-class instance pulls its CLASS into the graph too (as a Class node), so the
                // whole thing round-trips self-contained — a fresh VM rebuilds the class from the blob
                // and the instance below reconnects to it, needing no import. A NATIVE type that opts in
                // via _getstate_ (Matrix/DateTime/…) reconnects through its registered factory instead,
                // so its "class" does not travel. The class id is discarded: reachability alone puts the
                // Class node in the table, where rebuild's class pass materialises + re-registers it.
                if (inst) visit(inst->cls);
                // A `_getstate_` override (user class OR a native type that opts in) wins: serialize
                // whatever it returns, tagged with the type name so `_setstate_` can restore it.
                if (auto gs = serdeMethod(vm, h, "_getstate_")) {
                    Handle state = roots.add(vm.arena().deref(*gs).call(vm, {}));
                    n.tag = Tag::Stateful;
                    n.s = o.typeName();
                    n.links.push_back(visit(state));
                    break;
                }
                // Symmetric validation: a class defining _setstate_ but NOT _getstate_ would fall through
                // to auto-serializing its attributes and _setstate_ would never run on load — silently
                // half-initializing any derived state. That is the mirror of _getstate_-without-_setstate_,
                // which already hard-errors on rebuild, so reject it here at flatten time too.
                if (inst && serdeMethod(vm, h, "_setstate_"))
                    throw KiritoError("cannot serialize '" + inst->className +
                                      "': it defines _setstate_ but no _getstate_");
                // Otherwise a plain user-class instance auto-serializes its attributes.
                if (inst) {
                    n.tag = Tag::Object;
                    n.s = inst->className;
                    for (const auto& [k, v] : inst->attrs) {
                        uint32_t kid = visit(roots.add(vm.makeString(k)));
                        uint32_t vid = visit(v);
                        n.links.push_back(kid);
                        n.links.push_back(vid);
                    }
                    break;
                }
                throw KiritoError(std::string("cannot ") + verb + " type '" + o.typeName() +
                                  "' (define _getstate_/_setstate_ to make it serializable)");
            } break;
            case ValueKind::Module: {
                // A module reconnects by NAME on load (re-imported into the loading VM), not by value —
                // so a serialized closure over `math`/`json`/a user `.ki` module stays a live module.
                n.tag = Tag::Module;
                n.s = static_cast<ModuleValue&>(o).name();
            } break;
            case ValueKind::Function: {
                // Store the function's source + captured free variables (referenced helpers travel too;
                // builtins/modules are handled by closureCapture/the Module case). Kept in a helper so
                // its locals don't bloat this recursive frame — see flattenFunction.
                flattenFunction(vm, static_cast<KiFunction&>(o), n, roots, visit, verb);
            } break;
            case ValueKind::Class: {
                // Store the class's source + free-variable snapshot; re-running it on load also
                // re-registers the class, so its instances deserialize with no import. See flattenClass.
                flattenClass(vm, static_cast<ClassValue&>(o), n, roots, visit, verb);
            } break;
            case ValueKind::NativeFunction: {
                throw KiritoError(std::string("cannot ") + verb + " a native/built-in function '" +
                    static_cast<NativeFunction&>(o).name() +
                    "' (only Kirito-defined functions are serializable; a module reconnects by import)");
            } break;
            default: {
                throw KiritoError(std::string("cannot ") + verb + " type '" + o.typeName() + "'");
            } break;
        }
        --depth;
        nodes[id] = std::move(n);
        return id;
    };
    uint32_t rootId = visit(root);
    return {std::move(nodes), rootId};
}

// Rebuild the graph from a Node table in dependency order — leaves + modules, then function shells,
// then classes (which re-run their `class ...:` source, so a base class and class-variable initializers
// must already exist), then containers + instances — after which container links, instance attributes,
// and stateful state are wired exactly as before, and finally each function/class has its captured free
// variables bound into its scope (deferred so closure cycles and cross-references resolve). Shared
// references and cycles are preserved throughout; the root id and every link id are bounds-checked.
inline Handle rebuild(KiritoVM& vm, const std::vector<Node>& nodes, uint32_t rootId) {
    uint32_t n = static_cast<uint32_t>(nodes.size());
    if (rootId >= n) throw KiritoError("serialized root id out of range");
    RootScope roots(vm);
    std::vector<Handle> objs(n);
    std::vector<Handle> deserScope(n);   // a Function/Class node -> the scope holding its free variables
    const std::string deserName = "__kirito_deser_result__";
    auto checkId = [&](uint32_t id) -> uint32_t {
        if (id >= n) throw KiritoError("serialized child id out of range");
        return id;
    };
    auto nameOf = [&](uint32_t id) -> std::string {
        Object& o = vm.arena().deref(objs[checkId(id)]);
        if (o.kind() != ValueKind::String)
            throw KiritoError("cannot deserialize: a free-variable name is not a String");
        return static_cast<StrVal&>(o).value();
    };
    // Bind a function/class node's captured free-variable NAMES into `env`: eager ones to their real
    // value (only when `eagerReal` — a class build needs its base/class-var refs now), the rest to a
    // None placeholder overwritten in the final wiring pass. The names must be present up front so the
    // re-parse's name resolution succeeds and each reference addresses the right slot.
    auto declareFreeVars = [&](EnvValue& env, const Node& nd, bool eagerReal) {
        std::size_t pairs = nd.links.size() / 2;
        std::size_t eagerCount = static_cast<std::size_t>(nd.i);
        for (std::size_t k = 0; k < pairs; ++k) {
            bool eager = k < eagerCount;
            Handle val = (eagerReal && eager) ? objs[checkId(nd.links[2 * k + 1])] : vm.none();
            env.define(vm.arena(), nameOf(nd.links[2 * k]), val);
        }
    };

    // Pass 0a: leaf values (valued) + modules (re-imported by name). A function/class rebuild may need
    // these already present — a captured scalar, the String nodes naming its free variables, a module.
    for (uint32_t i = 0; i < n; ++i) {
        const Node& nd = nodes[i];
        switch (nd.tag) {
            case Tag::None: { objs[i] = vm.none(); } break;
            case Tag::Bool: { objs[i] = vm.makeBool(nd.b); } break;
            case Tag::Integer: { objs[i] = roots.add(vm.makeInt(nd.i)); } break;
            case Tag::Float: { objs[i] = roots.add(vm.makeFloat(nd.f)); } break;
            case Tag::String: { objs[i] = roots.add(vm.makeString(nd.s)); } break;
            case Tag::Module: { objs[i] = roots.add(vm.importModule(nd.s)); } break;
            default: break;  // containers/instances/stateful/function/class -> later passes
        }
    }
    // A class's eager class-variable initializers RUN during its rebuild (pass 0c), so everything they
    // can reach must be real by then — including through a captured helper's own free variables and
    // through a captured container. `earlyBuilt` marks the nodes that exist before pass 0c: leaves,
    // modules, function shells, and containers holding only such nodes. A class, instance or native
    // stateful is excluded — those are built in pass 0c/1/3 — and a container transitively holding one
    // is demoted below, so it keeps its original (pass 1/2) build.
    std::vector<char> earlyBuilt(n, 0);
    for (uint32_t i = 0; i < n; ++i) {
        switch (nodes[i].tag) {
            case Tag::None: case Tag::Bool: case Tag::Integer: case Tag::Float:
            case Tag::String: case Tag::Module: case Tag::Function:
            case Tag::List: case Tag::Set: case Tag::Dict: earlyBuilt[i] = 1; break;
            default: break;
        }
    }
    for (bool changed = true; changed;) {   // a container is early only if every member is
        changed = false;
        for (uint32_t i = 0; i < n; ++i) {
            const Node& nd = nodes[i];
            if (!earlyBuilt[i] || (nd.tag != Tag::List && nd.tag != Tag::Set && nd.tag != Tag::Dict))
                continue;
            for (uint32_t m : nd.links)
                if (!earlyBuilt[checkId(m)]) { earlyBuilt[i] = 0; changed = true; break; }
        }
    }
    // Pass 0b: function shells — re-parse each function's source in a fresh scope whose free variables
    // are pre-declared as placeholders. A body never runs at definition, so real values can wait for the
    // final wiring pass; this is also what lets closure cycles (e.g. self-recursion) reconstruct.
    for (uint32_t i = 0; i < n; ++i) {
        if (nodes[i].tag != Tag::Function) continue;
        const Node& nd = nodes[i];
        Handle S = roots.add(vm.newScope(vm.global()));
        declareFreeVars(static_cast<EnvValue&>(vm.arena().deref(S)), nd, /*eagerReal=*/false);
        try {
            vm.evalIn("var " + deserName + " = " + nd.s, S, "<deserialized function>",
                      /*indexTopLevel=*/true);
        } catch (const KiritoError& e) {
            throw KiritoError(std::string("cannot deserialize function: ") + e.what());
        }
        const Handle* f = static_cast<EnvValue&>(vm.arena().deref(S)).findLocal(deserName);
        if (!f) throw KiritoError("cannot deserialize function: re-parse produced no value");
        objs[i] = roots.add(*f);
        deserScope[i] = S;
    }
    // Pass 0b2: the `earlyBuilt` containers — allocated AND wired here, before any class body runs, so
    // an eager class-variable initializer that reads a captured container (directly or through a helper)
    // sees it populated rather than empty. Their members are leaves/modules/functions/early containers,
    // all built above; pass 1/2 skip them.
    for (uint32_t i = 0; i < n; ++i) {
        if (!earlyBuilt[i]) continue;
        switch (nodes[i].tag) {
            case Tag::List: { objs[i] = roots.add(vm.alloc(std::make_unique<ListVal>())); } break;
            case Tag::Set:  { objs[i] = roots.add(vm.alloc(std::make_unique<SetVal>()));  } break;
            case Tag::Dict: { objs[i] = roots.add(vm.alloc(std::make_unique<DictVal>())); } break;
            default: break;
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (!earlyBuilt[i]) continue;
        const Node& nd = nodes[i];
        if (nd.tag == Tag::List) {
            auto& l = static_cast<ListVal&>(vm.arena().deref(objs[i]));
            for (uint32_t id : nd.links) l.append(vm.arena(), objs[checkId(id)]);   // barriered
        } else if (nd.tag == Tag::Set) {
            auto& s = static_cast<SetVal&>(vm.arena().deref(objs[i]));
            for (uint32_t id : nd.links) s.add(vm.arena(), objs[checkId(id)]);
        } else if (nd.tag == Tag::Dict) {
            auto& d = static_cast<DictVal&>(vm.arena().deref(objs[i]));
            for (std::size_t k = 0; k + 1 < nd.links.size(); k += 2)
                d.set(vm.arena(), objs[checkId(nd.links[k])], objs[checkId(nd.links[k + 1])]);
        }
    }
    // Pass 0c: classes, in eager-dependency order — a class's base and class-variable initializers run
    // when it is rebuilt, so any user class they reference must exist first. Method references are lazy
    // (placeholders), so mutually recursive classes still deserialize. Re-running the source also
    // re-registers the class by name, so instances (built below) find it.
    {
        std::vector<uint32_t> pending;
        for (uint32_t i = 0; i < n; ++i) if (nodes[i].tag == Tag::Class) pending.push_back(i);
        std::vector<char> built(n, 0);
        // Clamp the eager count against the real pair count: a corrupt blob could claim more eager free
        // variables than it carries, and an unclamped loop would read past `links` (deserialize is a
        // trust boundary). A legitimate blob always has eagerCount <= links.size()/2.
        auto eagerCountOf = [&](const Node& nd) {
            return std::min(static_cast<std::size_t>(nd.i), nd.links.size() / 2);
        };
        // The ids a class's eager initializers can dereference: its own eager free variables, plus —
        // transitively — everything reachable through a captured helper's free variables and a captured
        // container's members. An eagerly-called helper dereferences its OWN free variables, so those
        // must be real too (A10-2); reachability stops at an instance/native-stateful, which cannot
        // exist before pass 1/3 either way.
        auto eagerFrontier = [&](uint32_t start) {
            std::vector<uint32_t> out;
            std::vector<char> seen(n, 0);
            std::vector<uint32_t> stack;
            auto pushLinks = [&](uint32_t id, bool eagerOnly) {
                const Node& d = nodes[id];
                if (d.tag == Tag::Function || d.tag == Tag::Class) {
                    std::size_t lim = eagerOnly ? eagerCountOf(d) : d.links.size() / 2;
                    for (std::size_t k = 0; k < lim; ++k) stack.push_back(checkId(d.links[2 * k + 1]));
                } else if (d.tag == Tag::List || d.tag == Tag::Set || d.tag == Tag::Dict) {
                    for (uint32_t m : d.links) stack.push_back(checkId(m));
                }
            };
            pushLinks(start, /*eagerOnly=*/true);
            while (!stack.empty()) {
                uint32_t id = stack.back();
                stack.pop_back();
                if (seen[id]) continue;   // also terminates a capture cycle (self/mutual recursion)
                seen[id] = 1;
                out.push_back(id);
                pushLinks(id, /*eagerOnly=*/false);
            }
            return out;
        };
        // A class waits for the classes its own eager free variables NAME — its base and the classes a
        // class-variable initializer refers to directly. That is precisely what the body dereferences
        // by name, so it never demands an order the graph cannot give.
        // Deliberately NOT transitive: `eagerFreeVariables` cannot tell `var m = helper` (binds it) from
        // `var v = helper()` (calls it), so ordering a class after everything a captured helper could
        // reach would invent dependencies. Two classes that merely BIND helpers naming each other would
        // then each appear to need the other and neither could ever be built — a cycle error on a
        // perfectly well-founded graph. Reachability still drives bindEagerHelpers below, where
        // over-approximating is free: binding a value nothing reads costs nothing.
        auto eagerDepsReady = [&](uint32_t i) -> bool {
            const Node& nd = nodes[i];
            std::size_t eagerCount = eagerCountOf(nd);
            for (std::size_t k = 0; k < eagerCount; ++k) {
                uint32_t valId = checkId(nd.links[2 * k + 1]);
                if (nodes[valId].tag == Tag::Class && !built[valId]) return false;
            }
            return true;
        };
        // Bind, for real, the free variables of every function the class's eager initializers can reach.
        // Pass 0b could only bind None placeholders (their values may not have existed yet) and pass 5
        // is far too late — the initializers run HERE. Values still unbuilt at this point (an instance,
        // a native stateful) keep their placeholder, exactly as before; pass 5 finishes the job.
        auto bindEagerHelpers = [&](uint32_t i) {
            for (uint32_t id : eagerFrontier(i)) {
                if (nodes[id].tag != Tag::Function || !deserScope[id].slot) continue;
                const Node& d = nodes[id];
                auto& env = static_cast<EnvValue&>(vm.arena().deref(deserScope[id]));
                std::size_t pairs = d.links.size() / 2;
                for (std::size_t k = 0; k < pairs; ++k) {
                    Handle v = objs[checkId(d.links[2 * k + 1])];
                    if (v.slot) env.define(vm.arena(), nameOf(d.links[2 * k]), v);   // barriered
                }
            }
        };
        std::size_t remaining = pending.size();
        // Build one class node: reconnect to a same-named class already in the loading VM, or re-parse.
        auto buildOne = [&](uint32_t i) {
            const Node& nd = nodes[i];
            // Reconnect to a class of this name ALREADY defined in the loading VM rather than
            // rebuilding it — so deserializing an instance never clobbers the caller's live class of
            // the same name (and a same-VM round-trip returns the very same class object). The class
            // still travels in the blob purely so an ABSENT class can be rebuilt here.
            const Handle* existing = vm.findClass(nd.s2);
            if (existing && vm.arena().deref(*existing).kind() == ValueKind::Class) {
                objs[i] = roots.add(*existing);   // no scope -> pass 5 skips it (nothing to wire)
                built[i] = 1;
                --remaining;
                return;
            }
            Handle S = roots.add(vm.newScope(vm.global()));
            declareFreeVars(static_cast<EnvValue&>(vm.arena().deref(S)), nd, /*eagerReal=*/true);
            bindEagerHelpers(i);
            try {
                vm.evalIn(nd.s, S, "<deserialized class>", /*indexTopLevel=*/true);
            } catch (const KiritoError& e) {
                throw KiritoError("cannot deserialize class '" + nd.s2 + "': " + e.what());
            }
            const Handle* c = vm.findClass(nd.s2);
            if (!c)
                throw KiritoError("cannot deserialize class '" + nd.s2 + "': re-parse did not define it");
            objs[i] = roots.add(*c);
            deserScope[i] = S;
            built[i] = 1;
            --remaining;
        };
        // Tier-1 readiness: every CLASS reachable through this class's eager frontier (its own eager
        // links AND the free vars of the helpers they reach) is already built. This sees through a
        // captured helper that instantiates another class (A13-1) — `eagerDepsReady`, which looks only
        // at DIRECT links, cannot. Reachability is a sound over-approximation of the real dependency,
        // so where it is acyclic it gives the exact order.
        auto frontierDepsReady = [&](uint32_t i) -> bool {
            for (uint32_t id : eagerFrontier(i))
                if (id != i && nodes[id].tag == Tag::Class && !built[id]) return false;
            return true;
        };
        while (remaining > 0) {
            bool progressed = false;
            // Tier 1 (precise): build every class whose eager-frontier classes are all built.
            for (uint32_t i : pending)
                if (!built[i] && frontierDepsReady(i)) { buildOne(i); progressed = true; }
            if (progressed) continue;
            // Tier 2 (fallback): reachability reported a possibly-SPURIOUS cycle — two classes that
            // only BIND helpers naming each other look mutually dependent though neither dereferences
            // the other (`eagerFreeVariables` can't tell `var m = helper` from `var v = helper()`).
            // Relax to what the body DIRECTLY names and build ONE, then re-attempt the precise tier.
            for (uint32_t i : pending)
                if (!built[i] && eagerDepsReady(i)) { buildOne(i); progressed = true; break; }
            if (!progressed)
                throw KiritoError("cannot deserialize: cyclic class-definition dependency (a base class "
                                  "or class-variable initializer forms a cycle)");
        }
    }
    // Pass 1: containers (empty) + user instances + native stateful shells, so ids resolve before the
    // wiring passes. Instances look their class up by name — a class serialized alongside them was built
    // in pass 0c, so it is found. Leaves, modules, functions and classes were built above.
    for (uint32_t i = 0; i < n; ++i) {
        const Node& nd = nodes[i];
        if (earlyBuilt[i]) continue;   // leaves/modules/functions + the pass-0b2 containers
        switch (nd.tag) {
            case Tag::List: { objs[i] = roots.add(vm.alloc(std::make_unique<ListVal>())); } break;
            case Tag::Dict: { objs[i] = roots.add(vm.alloc(std::make_unique<DictVal>())); } break;
            case Tag::Set: { objs[i] = roots.add(vm.alloc(std::make_unique<SetVal>())); } break;
            case Tag::Object:
            case Tag::Stateful: {
                const Handle* cls = vm.findClass(nd.s);
                if (cls && vm.arena().deref(*cls).kind() == ValueKind::Class) {
                    // a user class: create a bare instance now; attributes/state are filled below
                    auto inst = std::make_unique<InstanceValue>();
                    inst->cls = *cls;
                    inst->className = nd.s;
                    inst->ownerVM_ = &vm;   // owner VM for _hash_/_eq_/_bool_ (multi-VM safe)
                    // Cache the dunder availability now — same as ClassValue::callFull does — so a
                    // deserialised instance is hashable/equatable to the same extent as a freshly
                    // constructed one.
                    const auto& cv = static_cast<const ClassValue&>(vm.arena().deref(*cls));
                    inst->hasHashDunder = cv.findMethod(vm.arena(), "_hash_") != nullptr;
                    inst->hasEqDunder   = cv.findMethod(vm.arena(), "_eq_")   != nullptr;
                    Handle ih = roots.add(vm.alloc(std::move(inst)));
                    static_cast<InstanceValue&>(vm.arena().deref(ih)).selfHandle = ih;
                    objs[i] = ih;
                } else if (nd.tag == Tag::Stateful) {
                    // a native type that opted in: its registered factory builds an empty object now
                    // (so cycles/shared refs resolve); _setstate_ fills it in pass 3.
                    const auto* factory = vm.findDeserializer(nd.s);
                    if (!factory)
                        throw KiritoError("cannot deserialize '" + nd.s +
                                          "': no class or registered deserializer in this VM");
                    objs[i] = roots.add((*factory)(vm, vm.none()));
                } else {
                    throw KiritoError("cannot deserialize: class '" + nd.s + "' is not defined in this VM");
                }
            } break;
            default: break;  // leaves/modules/function/class already built above
        }
    }
    // A Set element / Dict KEY that is a Stateful node (Bytes/DateTime/Matrix/…) or an Object instance
    // has a CONTENT-based hash whose payload/attributes are only restored later (pass 3 for Stateful,
    // this pass at the member's own higher index for an Object). Hashing it now — while it is still
    // empty — would bucket it wrong (and an Object's _hash_ reading a not-yet-set attribute even
    // throws). So a Set/Dict with any such member is DEFERRED to pass 4 and wired after everything is
    // materialised (A17-1). Scalar/String-keyed containers wire here in pass 2, so a _setstate_ that
    // reads its state Dict still sees it populated.
    auto contentHashedMember = [&](const Node& nd) {
        if (nd.tag == Tag::Set) {
            for (uint32_t id : nd.links) {
                Tag tg = nodes[checkId(id)].tag;
                if (tg == Tag::Stateful || tg == Tag::Object) return true;
            }
        } else if (nd.tag == Tag::Dict) {
            for (std::size_t k = 0; k + 1 < nd.links.size(); k += 2) {
                Tag tg = nodes[checkId(nd.links[k])].tag;
                if (tg == Tag::Stateful || tg == Tag::Object) return true;
            }
        }
        return false;
    };
    // Pass 2: wire containers and instance attributes (handles only — contents finish filling here).
    for (uint32_t i = 0; i < n; ++i) {
        const Node& nd = nodes[i];
        if (earlyBuilt[i]) continue;   // an early container was already wired in pass 0b2
        if (nd.tag == Tag::List) {
            auto& l = static_cast<ListVal&>(vm.arena().deref(objs[i]));
            for (uint32_t id : nd.links) l.append(vm.arena(), objs[checkId(id)]);  // barriered
        } else if (nd.tag == Tag::Set && !contentHashedMember(nd)) {
            auto& s = static_cast<SetVal&>(vm.arena().deref(objs[i]));
            for (uint32_t id : nd.links) s.add(vm.arena(), objs[checkId(id)]);
        } else if (nd.tag == Tag::Dict && !contentHashedMember(nd)) {
            auto& d = static_cast<DictVal&>(vm.arena().deref(objs[i]));
            for (std::size_t k = 0; k + 1 < nd.links.size(); k += 2)
                d.set(vm.arena(), objs[checkId(nd.links[k])], objs[checkId(nd.links[k + 1])]);
        } else if (nd.tag == Tag::Object) {
            auto& inst = static_cast<InstanceValue&>(vm.arena().deref(objs[i]));
            for (std::size_t k = 0; k + 1 < nd.links.size(); k += 2) {
                Object& key = vm.arena().deref(objs[checkId(nd.links[k])]);
                if (key.kind() != ValueKind::String)
                    throw KiritoError("cannot deserialize: instance attribute name is not a String");
                Handle av = objs[checkId(nd.links[k + 1])];
                gcWriteBarrier(vm.arena(), &inst, av);   // a promoted instance gaining a young attr mid-rebuild
                inst.attrs[static_cast<StrVal&>(key).value()] = av;
            }
        }
    }
    // A Stateful node is either a NATIVE type (built by a registered factory: Bytes/Matrix/DateTime/
    // Random/Tensor) or a USER class (its _getstate_/_setstate_). The distinction drives the restore
    // order: a native _setstate_ reconstructs from its own List/scalar state and NEVER reads a shared
    // container, so it can run first (before any deferred container is wired). A user _setstate_ MAY
    // read a state container, so it must run AFTER the container it reads is wired — but a container
    // keyed by a still-empty content-hashed member can't be wired until that member is restored. The
    // resolution: native first, then wire the deferred containers whose content-hashed members are all
    // early-restorable (native-Stateful [now done] or an Object instance [attrs set in pass 2]), then
    // the user _setstate_s (which can now read those containers — A14-1), and finally the containers
    // keyed by a user-Stateful member (whose content only exists after the user pass — keeps A17-1).
    std::vector<char> userStateful(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        if (nodes[i].tag == Tag::Stateful) {
            const Handle* cls = vm.findClass(nodes[i].s);
            userStateful[i] = (cls && vm.arena().deref(*cls).kind() == ValueKind::Class) ? 1 : 0;
        }
    auto hasUserStatefulMember = [&](const Node& nd) {
        if (nd.tag == Tag::Set) {
            for (uint32_t id : nd.links) if (userStateful[checkId(id)]) return true;
        } else if (nd.tag == Tag::Dict) {
            for (std::size_t k = 0; k + 1 < nd.links.size(); k += 2)
                if (userStateful[checkId(nd.links[k])]) return true;
        }
        return false;
    };
    auto restoreStateful = [&](uint32_t i) {
        const Node& nd = nodes[i];
        if (nd.links.empty()) throw KiritoError("cannot deserialize '" + nd.s + "': missing state");
        Handle state = objs[checkId(nd.links[0])];
        auto setm = serdeMethod(vm, objs[i], "_setstate_");
        if (!setm)
            throw KiritoError("cannot deserialize '" + nd.s + "': it defines _getstate_ but no _setstate_");
        std::array<Handle, 1> args{state};
        vm.arena().deref(*setm).call(vm, args);
    };
    auto wireDeferredContainer = [&](uint32_t i) {
        const Node& nd = nodes[i];
        if (nd.tag == Tag::Set) {
            auto& s = static_cast<SetVal&>(vm.arena().deref(objs[i]));
            for (uint32_t id : nd.links) s.add(vm.arena(), objs[checkId(id)]);
        } else if (nd.tag == Tag::Dict) {
            auto& d = static_cast<DictVal&>(vm.arena().deref(objs[i]));
            for (std::size_t k = 0; k + 1 < nd.links.size(); k += 2)
                d.set(vm.arena(), objs[checkId(nd.links[k])], objs[checkId(nd.links[k + 1])]);
        }
    };
    // Pass 3a: restore NATIVE stateful objects (leaf content; they never read a container).
    for (uint32_t i = 0; i < n; ++i)
        if (nodes[i].tag == Tag::Stateful && !userStateful[i]) restoreStateful(i);
    // Pass 3b: wire deferred containers whose content-hashed members are now all restored (native
    // key/Object key), so a user _setstate_ in pass 3c can read through them.
    for (uint32_t i = 0; i < n; ++i)
        if (contentHashedMember(nodes[i]) && !hasUserStatefulMember(nodes[i])) wireDeferredContainer(i);
    // Pass 3c: restore USER stateful objects (may read the containers wired in 3b).
    for (uint32_t i = 0; i < n; ++i)
        if (nodes[i].tag == Tag::Stateful && userStateful[i]) restoreStateful(i);
    // Pass 4: wire the remaining deferred containers — those keyed by a user-Stateful member, whose
    // content only became available in pass 3c (so they bucket under the final hash — A17-1).
    for (uint32_t i = 0; i < n; ++i)
        if (contentHashedMember(nodes[i]) && hasUserStatefulMember(nodes[i])) wireDeferredContainer(i);
    // Pass 5: bind every function/class's captured free variables to their real values, overwriting the
    // pass-0b/0c placeholders. Deferred to here so closures over one another — cycles, self-recursion,
    // forward references — all resolve now that every object in the graph exists.
    for (uint32_t i = 0; i < n; ++i) {
        if (nodes[i].tag != Tag::Function && nodes[i].tag != Tag::Class) continue;
        if (!deserScope[i].slot) continue;  // a class reconnected to an existing one -> no scope to wire
        const Node& nd = nodes[i];
        auto& env = static_cast<EnvValue&>(vm.arena().deref(deserScope[i]));
        std::size_t pairs = nd.links.size() / 2;
        for (std::size_t k = 0; k < pairs; ++k)
            env.define(vm.arena(), nameOf(nd.links[2 * k]), objs[checkId(nd.links[2 * k + 1])]);
    }
    return objs[rootId];
}

}  // namespace serde
}  // namespace kirito

#endif
