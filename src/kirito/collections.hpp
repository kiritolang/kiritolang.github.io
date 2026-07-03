#ifndef KIRITO_COLLECTIONS_HPP
#define KIRITO_COLLECTIONS_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fum/unordered_map.hpp"
#include "arena.hpp"
#include "object.hpp"

namespace kirito {

// A Dict key / Set element must be hashable; throw the standard error otherwise. One source of truth
// for the guard repeated by DictVal::set/find and SetVal::add.
inline void requireHashable(const Object& o) {
    if (!o.hashable()) throw KiritoError("unhashable type '" + o.typeName() + "'");
}

// Containers store element handles, so aliasing (and, later, cycles) work naturally. Methods that
// need the VM (getItem/setItem/getAttr) are declared here and defined in runtime.hpp once
// KiritoVM is complete; everything else is inline.

// Render a sequence/mapping with cycle guarding via the StringifyCtx active set (keyed on `this`),
// plus a depth bound so a very deeply nested (acyclic) structure throws instead of overflowing the
// native stack.
template <typename F>
inline std::string stringifyGuarded(const Object* self, StringifyCtx& ctx,
                                    const char* open, const char* close, F emit) {
    if (ctx.active.count(self)) return std::string(open) + "..." + close;
    if (++ctx.depth > 1000) { --ctx.depth; throw KiritoError("structure too deeply nested to stringify"); }
    ctx.active.insert(self);
    std::string out = open;
    out += emit();
    out += close;
    ctx.active.erase(self);
    --ctx.depth;
    return out;
}

class ListVal : public Object {
public:
    std::vector<Handle> elems;

    ValueKind kind() const override { return ValueKind::List; }
    std::string typeName() const override { return "List"; }
    bool truthy() const override { return !elems.empty(); }

    std::string str(StringifyCtx& ctx) const override {
        return stringifyGuarded(this, ctx, "[", "]", [&] {
            std::string s;
            for (std::size_t i = 0; i < elems.size(); ++i) {
                if (i) s += ", ";
                s += stringifyChild(ctx, elems[i]);
            }
            return s;
        });
    }
    bool equals(const ObjectArena& arena, const Object& other) const override {
        if (this == &other) return true;  // identity (handles self-reference)
        if (other.kind() != ValueKind::List) return false;
        const auto& o = static_cast<const ListVal&>(other);
        if (o.elems.size() != elems.size()) return false;
        EqualsGuard guard;
        for (std::size_t i = 0; i < elems.size(); ++i)
            if (!arena.deref(elems[i]).equals(arena, arena.deref(o.elems[i]))) return false;
        return true;
    }
    void children(std::vector<Handle>& out) const override {
        out.insert(out.end(), elems.begin(), elems.end());
    }
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override { return elems; }
    std::optional<int64_t> length(KiritoVM&) override { return static_cast<int64_t>(elems.size()); }

    Handle getItem(KiritoVM&, std::span<const Handle> keys) override;
    void setItem(KiritoVM&, std::span<const Handle> keys, Handle value) override;
    Handle slice(KiritoVM&, Handle start, Handle stop, Handle step) override;
    Handle binary(KiritoVM&, BinOp, Handle self, Handle rhs) override;  // ordering + concatenation (runtime.hpp)
    bool contains(KiritoVM&, Handle value) override;
    std::vector<std::string> inspectMembers() const override {
        // Parameter names mirror the actual makeMethod bindings (`value` for the
        // search/equality methods so `xs.count(value = 2)` etc. work as inspect advertises).
        return {"append(item)", "pop(index) -> item", "insert(index, item)", "remove(value)",
                "index(value, start, end) -> Integer", "count(value) -> Integer", "extend(iterable)",
                "reverse()", "sort(key = None, reverse = False)", "apply(fn) -> List",
                "copy() -> List", "clear()"};
    }
    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;
};

// The one implementation of the "find a key within its hash bucket" probe, shared by Dict (a bucket
// of {key, value}) and Set (a bucket of value): `keyOf` extracts the comparable key Handle from a
// bucket element, and equality goes through the value protocol. Returns the index or -1.
template <class Bucket, class KeyOf>
inline std::ptrdiff_t probeBucket(const ObjectArena& arena, const Bucket& bucket, const Object& key,
                                  KeyOf keyOf) {
    for (std::size_t i = 0; i < bucket.size(); ++i)
        if (arena.deref(keyOf(bucket[i])).equals(arena, key)) return static_cast<std::ptrdiff_t>(i);
    return -1;
}
inline Handle dictKeyOf(const std::pair<Handle, Handle>& e) { return e.first; }
inline Handle setKeyOf(Handle e) { return e; }

// Hash-bucketed mapping. Keys must be hashable; lookup hashes then compares with the value
// protocol's equals within the bucket.
class DictVal : public Object {
public:
    fum::unordered_map<std::size_t, std::vector<std::pair<Handle, Handle>>> buckets;
    std::size_t count = 0;

    ValueKind kind() const override { return ValueKind::Dict; }
    std::string typeName() const override { return "Dict"; }
    bool truthy() const override { return count > 0; }

    void set(ObjectArena& arena, Handle key, Handle value) {
        const Object& k = arena.deref(key);
        requireHashable(k);
        auto& bucket = buckets[k.hash()];
        auto i = probeBucket(arena, bucket, k, dictKeyOf);
        if (i >= 0) { bucket[static_cast<std::size_t>(i)].second = value; return; }
        bucket.emplace_back(key, value);
        ++count;
    }
    const Handle* find(const ObjectArena& arena, Handle key) const {
        const Object& k = arena.deref(key);
        requireHashable(k);
        auto it = buckets.find(k.hash());
        if (it == buckets.end()) return nullptr;
        auto i = probeBucket(arena, it->second, k, dictKeyOf);
        return i >= 0 ? &it->second[static_cast<std::size_t>(i)].second : nullptr;
    }
    std::vector<Handle> keys() const {
        std::vector<Handle> out;
        out.reserve(count);
        for (const auto& [h, bucket] : buckets)
            for (const auto& [k, v] : bucket) out.push_back(k);
        return out;
    }
    // Key/value pairs, for C++ consumers that need both (e.g. urlencode).
    std::vector<std::pair<Handle, Handle>> pairs() const {
        std::vector<std::pair<Handle, Handle>> out;
        out.reserve(count);
        for (const auto& [h, bucket] : buckets)
            for (const auto& [k, v] : bucket) out.emplace_back(k, v);
        return out;
    }

    std::string str(StringifyCtx& ctx) const override {
        return stringifyGuarded(this, ctx, "{", "}", [&] {
            std::string s;
            bool first = true;
            for (const auto& [h, bucket] : buckets)
                for (const auto& [k, v] : bucket) {
                    if (!first) s += ", ";
                    first = false;
                    s += stringifyChild(ctx, k);
                    s += ": ";
                    s += stringifyChild(ctx, v);
                }
            return s;
        });
    }
    bool equals(const ObjectArena& arena, const Object& other) const override {
        if (this == &other) return true;
        if (other.kind() != ValueKind::Dict) return false;
        const auto& o = static_cast<const DictVal&>(other);
        if (o.count != count) return false;
        EqualsGuard guard;
        for (const auto& [h, bucket] : buckets)
            for (const auto& [k, v] : bucket) {
                const Handle* ov = o.find(arena, k);
                if (!ov || !arena.deref(v).equals(arena, arena.deref(*ov))) return false;
            }
        return true;
    }
    void children(std::vector<Handle>& out) const override {
        for (const auto& [h, bucket] : buckets)
            for (const auto& [k, v] : bucket) { out.push_back(k); out.push_back(v); }
    }
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override { return keys(); }
    std::optional<int64_t> length(KiritoVM&) override { return static_cast<int64_t>(count); }

    bool remove(ObjectArena& arena, Handle key) {
        const Object& k = arena.deref(key);
        if (!k.hashable()) return false;
        auto it = buckets.find(k.hash());
        if (it == buckets.end()) return false;
        auto& bucket = it->second;
        auto i = probeBucket(arena, bucket, k, dictKeyOf);
        if (i < 0) return false;
        bucket.erase(bucket.begin() + i);
        --count;
        return true;
    }

    Handle getItem(KiritoVM&, std::span<const Handle> keys) override;
    void setItem(KiritoVM&, std::span<const Handle> keys, Handle value) override;
    bool contains(KiritoVM&, Handle key) override;  // key membership
    std::vector<std::string> inspectMembers() const override {
        return {"keys() -> List", "values() -> List", "items() -> List", "get(key, default = None)",
                "pop(key, default = None)", "remove(key)", "setdefault(key, default = None)",
                "update(other)", "popitem() -> List", "apply(fn) -> Dict", "copy() -> Dict", "clear()"};
    }
    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;
};

// Hash-bucketed set of unique values.
class SetVal : public Object {
public:
    fum::unordered_map<std::size_t, std::vector<Handle>> buckets;
    std::size_t count = 0;

    ValueKind kind() const override { return ValueKind::Set; }
    std::string typeName() const override { return "Set"; }
    bool truthy() const override { return count > 0; }

    bool add(ObjectArena& arena, Handle value) {
        const Object& v = arena.deref(value);
        requireHashable(v);
        auto& bucket = buckets[v.hash()];
        if (probeBucket(arena, bucket, v, setKeyOf) >= 0) return false;
        bucket.push_back(value);
        ++count;
        return true;
    }
    bool contains(const ObjectArena& arena, Handle value) const {
        const Object& v = arena.deref(value);
        if (!v.hashable()) return false;
        auto it = buckets.find(v.hash());
        if (it == buckets.end()) return false;
        return probeBucket(arena, it->second, v, setKeyOf) >= 0;
    }
    std::vector<Handle> items() const {
        std::vector<Handle> out;
        out.reserve(count);
        for (const auto& [h, bucket] : buckets)
            for (Handle e : bucket) out.push_back(e);
        return out;
    }

    std::string str(StringifyCtx& ctx) const override {
        return stringifyGuarded(this, ctx, "{", "}", [&] {
            std::string s;
            bool first = true;
            for (const auto& [h, bucket] : buckets)
                for (Handle e : bucket) {
                    if (!first) s += ", ";
                    first = false;
                    s += stringifyChild(ctx, e);
                }
            return s;
        });
    }
    bool equals(const ObjectArena& arena, const Object& other) const override {
        if (this == &other) return true;
        if (other.kind() != ValueKind::Set) return false;
        const auto& o = static_cast<const SetVal&>(other);
        if (o.count != count) return false;
        EqualsGuard guard;
        for (const auto& [h, bucket] : buckets)
            for (Handle e : bucket)
                if (!o.contains(arena, e)) return false;
        return true;
    }
    void children(std::vector<Handle>& out) const override {
        for (const auto& [h, bucket] : buckets)
            out.insert(out.end(), bucket.begin(), bucket.end());
    }
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override { return items(); }
    std::optional<int64_t> length(KiritoVM&) override { return static_cast<int64_t>(count); }
    bool contains(KiritoVM&, Handle value) override;

    std::vector<std::string> inspectMembers() const override {
        return {"add(value)", "discard(value)", "remove(value)", "contains(value) -> Bool",
                "pop() -> value", "union(other) -> Set", "intersection(other) -> Set",
                "difference(other) -> Set", "symmetricdifference(other) -> Set",
                "issubset(other) -> Bool", "issuperset(other) -> Bool", "isdisjoint(other) -> Bool",
                "apply(fn) -> Set", "copy() -> Set", "clear()"};
    }
    Handle getAttr(KiritoVM&, Handle self, std::string_view name) override;
    // Set algebra via the operators Kirito has: `-` (difference) and `<`/`<=`/`>`/`>=` (proper-/
    // subset, proper-/superset). (Kirito has no |/&/^ tokens; union/intersection/symmetricdifference
    // remain methods. `==`/`!=` go through equals().)
    Handle binary(KiritoVM&, BinOp op, Handle self, Handle rhs) override;
};

}  // namespace kirito

#endif
