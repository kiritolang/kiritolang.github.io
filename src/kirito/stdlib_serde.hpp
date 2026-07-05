#ifndef KIRITO_STDLIB_SERDE_HPP
#define KIRITO_STDLIB_SERDE_HPP

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
enum class Tag : uint8_t {
    None = 0, Bool = 1, Integer = 2, Float = 3, String = 4, List = 5, Dict = 6, Set = 7,
    Object = 8, Stateful = 9
};

struct Node {
    Tag tag = Tag::None;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    std::vector<uint32_t> links;
};

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

// Walk the graph rooted at `root`, giving every reachable object an id by identity (a value reachable
// by two paths is recorded once; an id is reserved before recursing so cycles terminate) and flatten
// it into a Node table. `verb` ("serialize" / "dump") names the operation in error messages. Returns
// the table and the root's id. Supported kinds: None/Bool/Integer/Float/String/Bytes/List/Dict/Set,
// plus user `class` instances (by attributes, or via the _getstate_/_setstate_ protocol) and
// serializable native value types (Matrix/Complex/Tensor/DateTime/Random, which opt in the same way).
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
        // Guard against overflowing the native stack on a pathologically deep graph. ASan enlarges
        // every frame, so the recursive walk overflows far sooner there — use a smaller ceiling.
#if defined(KIRITO_SANITIZER_BUILD)
        constexpr int kMaxFlattenDepth = 1500;
#else
        constexpr int kMaxFlattenDepth = 10000;
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
                // A `_getstate_` override (user class OR a native type that opts in) wins: serialize
                // whatever it returns, tagged with the type name so `_setstate_` can restore it.
                if (auto gs = serdeMethod(vm, h, "_getstate_")) {
                    Handle state = roots.add(vm.arena().deref(*gs).call(vm, {}));
                    n.tag = Tag::Stateful;
                    n.s = o.typeName();
                    n.links.push_back(visit(state));
                    break;
                }
                // Otherwise a plain user-class instance auto-serializes its attributes.
                if (auto* inst = dynamic_cast<InstanceValue*>(&o)) {
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

// Rebuild the graph from a Node table in two passes: create every object (scalars valued, containers
// empty) so ids resolve, then wire the container links — restoring shared references and cycles. The
// root id and every link id are bounds-checked against the table.
inline Handle rebuild(KiritoVM& vm, const std::vector<Node>& nodes, uint32_t rootId) {
    uint32_t n = static_cast<uint32_t>(nodes.size());
    if (rootId >= n) throw KiritoError("serialized root id out of range");
    RootScope roots(vm);
    std::vector<Handle> objs(n);
    auto checkId = [&](uint32_t id) -> uint32_t {
        if (id >= n) throw KiritoError("serialized child id out of range");
        return id;
    };
    // Pass 1: create every object so ids resolve (scalars valued; containers empty; instances created
    // from their class, attributes/state filled later — this lets shared references and cycles work).
    for (uint32_t i = 0; i < n; ++i) {
        const Node& nd = nodes[i];
        switch (nd.tag) {
            case Tag::None: { objs[i] = vm.none(); } break;
            case Tag::Bool: { objs[i] = vm.makeBool(nd.b); } break;
            case Tag::Integer: { objs[i] = roots.add(vm.makeInt(nd.i)); } break;
            case Tag::Float: { objs[i] = roots.add(vm.makeFloat(nd.f)); } break;
            case Tag::String: { objs[i] = roots.add(vm.makeString(nd.s)); } break;
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
        if (nd.tag == Tag::List) {
            auto& l = static_cast<ListVal&>(vm.arena().deref(objs[i]));
            for (uint32_t id : nd.links) l.elems.push_back(objs[checkId(id)]);
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
                inst.attrs[static_cast<StrVal&>(key).value()] = objs[checkId(nd.links[k + 1])];
            }
        }
    }
    // Pass 3: now that every state value is fully wired, restore _setstate_ objects.
    for (uint32_t i = 0; i < n; ++i) {
        const Node& nd = nodes[i];
        if (nd.tag != Tag::Stateful) continue;
        if (nd.links.empty()) throw KiritoError("cannot deserialize '" + nd.s + "': missing state");
        Handle state = objs[checkId(nd.links[0])];
        auto setm = serdeMethod(vm, objs[i], "_setstate_");
        if (!setm)
            throw KiritoError("cannot deserialize '" + nd.s + "': it defines _getstate_ but no _setstate_");
        std::array<Handle, 1> args{state};
        vm.arena().deref(*setm).call(vm, args);
    }
    // Pass 4: wire the Sets/Dicts deferred from pass 2 (those with a content-hashed member), now that
    // every element/key is fully materialised so it buckets under its final hash (A17-1).
    for (uint32_t i = 0; i < n; ++i) {
        const Node& nd = nodes[i];
        if (!contentHashedMember(nd)) continue;
        if (nd.tag == Tag::Set) {
            auto& s = static_cast<SetVal&>(vm.arena().deref(objs[i]));
            for (uint32_t id : nd.links) s.add(vm.arena(), objs[checkId(id)]);
        } else if (nd.tag == Tag::Dict) {
            auto& d = static_cast<DictVal&>(vm.arena().deref(objs[i]));
            for (std::size_t k = 0; k + 1 < nd.links.size(); k += 2)
                d.set(vm.arena(), objs[checkId(nd.links[k])], objs[checkId(nd.links[k + 1])]);
        }
    }
    return objs[rootId];
}

}  // namespace serde
}  // namespace kirito

#endif
