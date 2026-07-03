#ifndef KIRITO_CLASS_VALUE_HPP
#define KIRITO_CLASS_VALUE_HPP

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fum/unordered_map.hpp"
#include "arena.hpp"
#include "object.hpp"

namespace kirito {

// Kirito's special (operator) methods use dunder names with single underscores:
// _init_, _str_, _add_, _eq_, _getitem_, _call_, ... These map an operator/protocol slot to the
// method name a class may define.
inline const char* binOpMethod(BinOp op) {
    switch (op) {
        case BinOp::Add: { return "_add_"; } break;
        case BinOp::Sub: { return "_sub_"; } break;
        case BinOp::Mul: { return "_mul_"; } break;
        case BinOp::Div: { return "_div_"; } break;
        case BinOp::FloorDiv: { return "_floordiv_"; } break;
        case BinOp::Mod: { return "_mod_"; } break;
        case BinOp::Pow: { return "_pow_"; } break;
        case BinOp::Eq: { return "_eq_"; } break;
        case BinOp::Ne: { return "_ne_"; } break;
        case BinOp::Lt: { return "_lt_"; } break;
        case BinOp::Le: { return "_le_"; } break;
        case BinOp::Gt: { return "_gt_"; } break;
        case BinOp::Ge: { return "_ge_"; } break;
        case BinOp::In: case BinOp::NotIn: { } break;  // handled via _contains_, not here
    }
    return "";
}
inline const char* unOpMethod(UnOp op) { return op == UnOp::Neg ? "_neg_" : "_not_"; }

// A private member has a single leading underscore and no trailing underscore (e.g. _count).
// Special methods are _name_ (underscores on both sides) and are NOT private.
inline bool isPrivateName(std::string_view n) {
    return n.size() >= 2 && n.front() == '_' && n.back() != '_';
}

// A user-defined class: a bag of methods (Functions whose first param is the receiver) plus an
// optional base class. Calling it constructs an instance and runs its `_init_` method. A class is
// a first-class value living in the same model as built-ins — the unified-object goal.
class ClassValue : public Object {
public:
    std::string name;
    fum::unordered_map<std::string, Handle> methods;
    Handle base{};
    bool hasBase = false;
    Handle selfHandle{};  // the class's own arena handle (set by the evaluator after allocation)

    ValueKind kind() const override { return ValueKind::Class; }
    std::string typeName() const override { return name; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override { return "<class " + name + ">"; }
    bool equals(const ObjectArena&, const Object& other) const override { return this == &other; }
    void children(std::vector<Handle>& out) const override {
        for (const auto& [k, v] : methods) out.push_back(v);
        if (hasBase) out.push_back(base);
    }

    // Method resolution walks the base chain.
    const Handle* findMethod(const ObjectArena& arena, const std::string& n) const {
        auto it = methods.find(n);
        if (it != methods.end()) return &it->second;
        if (hasBase) return static_cast<const ClassValue&>(arena.deref(base)).findMethod(arena, n);
        return nullptr;
    }

    Handle call(KiritoVM&, std::span<const Handle> args) override;  // instantiate (runtime.hpp)
    // Instantiate, forwarding keyword arguments to `_init_` (so `C(x, y = 1)` works). `call` above
    // delegates here with no named args. Defined in runtime.hpp.
    Handle callFull(KiritoVM&, std::span<const Handle> positional, std::span<const NamedArg> named);
};

// An instance of a user class: its own attribute table plus a handle to its class for method
// lookup. Operator slots dispatch to the class's _op_ methods when defined.
class InstanceValue : public Object {
public:
    Handle cls{};
    Handle selfHandle{};     // this instance's own arena handle (for invoking its methods)
    std::string className;   // copied from the class so typeName()/str() need no arena
    // Opt-in dunder cache — set once at instantiation time by walking the class chain, so the
    // Dict/Set hash/equals hot path is a plain bool test with no method lookup.
    bool hasHashDunder = false;   // class (or a base) defines `_hash_` → InstanceValue is hashable
    bool hasEqDunder   = false;   // class (or a base) defines `_eq_` → equals() uses it
    bool hasBoolDunder = false;   // class (or a base) defines `_bool_` → truthy() calls it

    fum::unordered_map<std::string, Handle> attrs;

    ValueKind kind() const override { return ValueKind::Instance; }
    std::string typeName() const override { return className; }
    // Custom truthiness via `_bool_(self) -> Bool` (opt-in). A class without `_bool_` keeps the
    // historical behaviour: an instance is always truthy. See runtime.hpp for the dispatch (uses
    // KiritoVM::activeVM(), same pattern as `_hash_`).
    bool truthy() const override;
    std::string str(StringifyCtx&) const override;  // invokes _str_ if defined (runtime.hpp)
    // Structural equality falls through to `_eq_` when the class defines it (needed so a `_hash_`
    // opt-in doesn't break the "equal keys collide" invariant Dict/Set assume). If `_eq_` is not
    // defined, or the interpreter context is unavailable, fall back to identity — the historical
    // behaviour.
    bool equals(const ObjectArena&, const Object& other) const override;
    // Hashability opt-in: only a class that defines `_hash_` gets a hashable instance. `hash()`
    // invokes the Kirito method via KiritoVM::activeVM() and folds the returned Integer into a
    // `size_t` (Dict/Set only care that equal objects yield equal buckets).
    bool hashable() const override { return hasHashDunder; }
    std::size_t hash() const override;
    void children(std::vector<Handle>& out) const override {
        out.push_back(cls);
        for (const auto& [k, v] : attrs) out.push_back(v);
    }
    void setAttr(KiritoVM&, std::string_view name, Handle value) override {
        attrs[std::string(name)] = value;
    }
    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;  // runtime.hpp

    // Operator protocol -> _op_ method dispatch (defined in runtime.hpp).
    Handle binary(KiritoVM&, BinOp, Handle self, Handle rhs) override;
    Handle unary(KiritoVM&, UnOp, Handle self) override;
    Handle call(KiritoVM&, std::span<const Handle> args) override;
    Handle callKw(KiritoVM&, std::span<const Handle> args, std::span<const NamedArg> named);  // _call_ + kwargs
    Handle getItem(KiritoVM&, std::span<const Handle> keys) override;
    void setItem(KiritoVM&, std::span<const Handle> keys, Handle value) override;
    std::optional<int64_t> length(KiritoVM&) override;
    bool contains(KiritoVM&, Handle value) override;
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override;  // via _iter_ (runtime.hpp)

    const Handle* findMethod(const ObjectArena& arena, const std::string& n) const {
        return static_cast<const ClassValue&>(arena.deref(cls)).findMethod(arena, n);
    }
};

// The value returned by `self._super_()`: a proxy onto the same instance whose attribute/method
// lookup begins at the BASE of the class whose method is currently running. Accessing `parent.foo`
// finds `foo` from the base chain and binds it to the original instance, so an overriding method can
// reuse the inherited behaviour. It is only meaningful for a class that inherits; constructing one
// for a baseless class is an error (thrown where `_super_()` is evaluated).
class SuperValue : public Object {
public:
    Handle instance{};   // the real receiver (self)
    Handle startClass{};  // the class to begin method resolution from (the base of the current class)

    ValueKind kind() const override { return ValueKind::Instance; }  // behaves like an instance proxy
    std::string typeName() const override { return "Super"; }
    bool truthy() const override { return true; }
    std::string str(StringifyCtx&) const override { return "<super>"; }
    bool equals(const ObjectArena&, const Object& other) const override { return this == &other; }
    void children(std::vector<Handle>& out) const override { out.push_back(instance); out.push_back(startClass); }

    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;  // runtime.hpp
};

}  // namespace kirito

#endif
