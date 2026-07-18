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
    CardTable cards;   // dirty-card set over `elems` for the generational minor GC (card-marked type)

    // Barriered element writes: inserting a (possibly young) handle into a (possibly old) list records
    // the old->young edge AND marks the written slot's card. append/setElem write at a STABLE index, so
    // they mark just that card. insertElem SHIFTS [i,end), moving young children off their cards, so it
    // conservatively markAll()s when old (one full rescan next minor). Removal/reorder ops (pop/erase/
    // reverse/sort) also shift and must call gcReorderShift() — see below and their sites in runtime.hpp.
    void append(ObjectArena& arena, Handle h) { gcWriteBarrier(arena, this, h, elems.size()); elems.push_back(h); }
    void setElem(ObjectArena& arena, std::size_t i, Handle h) { gcWriteBarrier(arena, this, h, i); elems[i] = h; }
    void insertElem(ObjectArena& arena, std::size_t i, Handle h) {
        bool shifts = i < elems.size();
        elems.insert(elems.begin() + static_cast<std::ptrdiff_t>(i), h);
        gcWriteBarrier(arena, this, h, i);
        if (shifts && !gcYoung()) cards.markAll();   // [i,end) moved to new indices -> cards stale
    }
    // Call from any op that REORDERS existing elements (reverse/sort/erase-not-at-end/pop-not-at-end):
    // an old list may hold young children whose card bits now point at the wrong slots, so rescan all.
    // Young lists are traced in full anyway; end-pop/end-append don't shift and skip this.
    void gcReorderShift() { if (!gcYoung()) cards.markAll(); }
    // Single source for emptying a List: drop the elements AND reset the card table (an emptied list has
    // no children, so any dirty card is stale). Both Kirito `list.clear()` and the value.hpp wrapper call
    // this so the invariant matches Dict/Set clear() and isn't duplicated. (Leaving stale cards was
    // memory-safe — forEachDirtyRange clamps to the child count — but defeated the O(N) card fast path.)
    void clearElems() { elems.clear(); cards.clear(); }

    void gcMarkCard(std::size_t entryIndex) override { cards.mark(entryIndex); }
    void gcMarkAllCards() override { cards.markAll(); }
    void gcClearCards() override { cards.clear(); }
    void childrenInDirtyCards(std::vector<Handle>& out) const override {
        cards.forEachDirtyRange(elems.size(), [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i) out.push_back(elems[i]);
        });
    }

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

// The one equality rule for a Dict/Set key lookup, shared by Dict AND Set. Kirito deliberately does
// NOT identity-short-circuit before `==`, so a NaN key is "write-only" — insertable but never findable
// (NaN != NaN), a documented consequence of exact NaN-never-equal equality (r7_types.ki). Cross-type
// equality must stay SYMMETRIC — the same rule kiEquals (runtime.hpp) applies to `==`: a stored native
// Integer doesn't recognize a probe BigInt, but the BigInt recognizes the Integer, so without the retry
// the two hash-equal keys stay unmerged (an order-dependent duplicate key, breaking the no-two-==-keys
// invariant). Gated on a kind mismatch so same-kind dispatch (incl. write-only NaN Floats) is untouched.
inline bool keysEqual(const ObjectArena& arena, const Object& stored, const Object& key) {
    if (stored.equals(arena, key)) return true;
    if (stored.kind() != key.kind() && key.equals(arena, stored)) return true;
    return false;
}

// Reentrancy guard for the hash-bucketed Dict/Set. Probing a bucket runs the key's `_hash_`/`_eq_`,
// which is arbitrary Kirito code that may mutate the SAME container — reallocating the bucket map or
// the bucket vector and dangling a reference held across the probe (a use-after-free that corrupts
// the heap). A ProbeScope marks a probe in progress for the container's lifetime of the call; a
// mutating op refuses to run (a clean, catchable error) while a probe is active on that container, so
// no realloc can invalidate a live bucket reference mid-probe. Reads may nest freely (they don't
// realloc); only nested MUTATION during any probe is rejected.
struct ProbeScope {
    bool& flag;
    bool prev;
    explicit ProbeScope(bool& f) : flag(f), prev(f) { f = true; }
    ~ProbeScope() { flag = prev; }
    ProbeScope(const ProbeScope&) = delete;
    ProbeScope& operator=(const ProbeScope&) = delete;
};

// Hash-bucketed mapping. Keys must be hashable; lookup hashes then compares with the value
// protocol's equals within the bucket.
class DictVal : public Object {
public:
    // Compact (Python-3.7) layout: a DENSE, INSERTION-ORDERED entry array + an open-addressing index
    // into it. Iteration is insertion order; deletes leave a TOMBSTONE (a blanked entry, key.generation
    // == 0) so surviving entries keep a STABLE index — which is what makes the CardTable over `entries`
    // valid across deletes (a shift would move a young child off its card -> UAF). Compaction (rare)
    // shifts positions and markAll()s.
    struct Entry { Handle key; Handle value; std::size_t hash; };
    static constexpr int32_t kEmpty = -1;
    static constexpr int32_t kDeleted = -2;
    std::vector<Entry> entries;                                    // dense; entries.size() == count + tombstones
    std::vector<int32_t> index = std::vector<int32_t>(8, kEmpty);  // open-addressing -> entry position
    std::size_t count = 0;                                         // live entries
    std::size_t tombstones = 0;                                    // dead (blanked) entries in `entries`
    CardTable cards;                                               // dirty-card set over `entries`
    mutable bool probing_ = false;   // a probe (runs user _hash_/_eq_) is in flight — reject nested mutation

    ValueKind kind() const override { return ValueKind::Dict; }
    std::string typeName() const override { return "Dict"; }
    bool truthy() const override { return count > 0; }

    // Probe the index chain for `key` (hash h). Returns the matching LIVE entry position, or -1; on -1,
    // `outSlot` is the first kEmpty slot (insert target — kDeleted slots are skipped, not reused, so the
    // load-factor invariant below keeps a kEmpty always reachable). May run a user _eq_ via keysEqual.
    std::ptrdiff_t probeIndex(const ObjectArena& arena, const Object& key, std::size_t h, std::size_t& outSlot) const {
        std::size_t mask = index.size() - 1, slot = h & mask;
        for (std::size_t n = 0; n < index.size(); ++n) {
            int32_t e = index[slot];
            if (e == kEmpty) { outSlot = slot; return -1; }
            if (e != kDeleted) {
                const Entry& ent = entries[static_cast<std::size_t>(e)];
                if (ent.hash == h && ent.key.generation != 0 &&
                    keysEqual(arena, arena.deref(ent.key), key)) { outSlot = slot; return e; }
            }
            slot = (slot + 1) & mask;
        }
        outSlot = slot;   // table saturated (shouldn't happen: entries.size() < 3/4 index.size())
        return -1;
    }
    // Rebuild `index` from live entries. When `compact`, first drop tombstoned entries (shifting
    // positions -> markAll). Sizes to a pow2 with a < 3/4 load factor. Amortized O(n).
    void reindex(bool compact) {
        if (compact && tombstones) {
            std::vector<Entry> live; live.reserve(count);
            for (const Entry& e : entries) if (e.key.generation != 0) live.push_back(e);
            entries.swap(live);
            tombstones = 0;
            if (!gcYoung()) cards.markAll();   // entry positions shifted -> old cards no longer describe them
        }
        std::size_t newSize = 8;
        while (newSize * 3 < (entries.size() + 1) * 4) newSize *= 2;
        index.assign(newSize, kEmpty);
        std::size_t mask = newSize - 1;
        for (std::size_t p = 0; p < entries.size(); ++p) {
            if (entries[p].key.generation == 0) continue;   // skip tombstones (non-compacting rebuild)
            std::size_t slot = entries[p].hash & mask;
            while (index[slot] != kEmpty) slot = (slot + 1) & mask;
            index[slot] = static_cast<int32_t>(p);
        }
    }

    void set(ObjectArena& arena, Handle key, Handle value) {
        const Object& k = arena.deref(key);
        requireHashable(k);
        std::size_t h = k.hash();  // may run _hash_; done before any entry reference is cached
        if (probing_) throw KiritoError("Dict changed size during a key comparison");
        ProbeScope guard(probing_);
        std::size_t slot = 0;
        std::ptrdiff_t pos = probeIndex(arena, k, h, slot);   // may run _eq_ (nested mutation rejected)
        if (pos >= 0) {   // key present: update the value in place (stable index -> mark its card)
            gcWriteBarrier(arena, this, value, static_cast<std::size_t>(pos));
            entries[static_cast<std::size_t>(pos)].value = value;
            return;
        }
        // insert: append the entry only AFTER the probe, so a GC fired by _hash_/_eq_ saw a consistent
        // dict (the append below is a C++ vector op, never a Kirito allocation, so no GC lands mid-write).
        std::size_t np = entries.size();
        entries.push_back(Entry{key, value, h});
        index[slot] = static_cast<int32_t>(np);
        ++count;
        gcWriteBarrier(arena, this, key, np);
        gcWriteBarrier(arena, this, value, np);
        if (entries.size() * 4 >= index.size() * 3) reindex(tombstones * 2 >= entries.size());
    }
    const Handle* find(const ObjectArena& arena, Handle key) const {
        const Object& k = arena.deref(key);
        requireHashable(k);
        std::size_t h = k.hash();
        ProbeScope guard(probing_);  // read: block nested mutation from a reentrant _eq_
        std::size_t slot = 0;
        std::ptrdiff_t pos = probeIndex(arena, k, h, slot);
        return pos >= 0 ? &entries[static_cast<std::size_t>(pos)].value : nullptr;
    }
    std::vector<Handle> keys() const {
        std::vector<Handle> out; out.reserve(count);
        for (const Entry& e : entries) if (e.key.generation != 0) out.push_back(e.key);
        return out;
    }
    // Key/value pairs, for C++ consumers that need both (e.g. urlencode). Insertion order.
    std::vector<std::pair<Handle, Handle>> pairs() const {
        std::vector<std::pair<Handle, Handle>> out; out.reserve(count);
        for (const Entry& e : entries) if (e.key.generation != 0) out.emplace_back(e.key, e.value);
        return out;
    }

    std::string str(StringifyCtx& ctx) const override {
        return stringifyGuarded(this, ctx, "{", "}", [&] {
            ProbeScope pguard(probing_);  // stringifyChild runs a contained value's _str_, which can
                                          // mutate THIS dict and realloc `entries` — reject it (catchable)
                                          // instead of a heap-use-after-free.
            std::string s;
            bool first = true;
            for (const Entry& e : entries) {
                if (e.key.generation == 0) continue;
                if (!first) s += ", ";
                first = false;
                s += stringifyChild(ctx, e.key);
                s += ": ";
                s += stringifyChild(ctx, e.value);
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
        ProbeScope pguard(probing_);  // we iterate THIS's live entries; a reentrant _eq_ (run by o.find)
                                      // that mutates/clears this dict mid-compare must be rejected, not UAF
        for (const Entry& e : entries) {
            if (e.key.generation == 0) continue;
            const Handle* ov = o.find(arena, e.key);
            if (!ov || !arena.deref(e.value).equals(arena, arena.deref(*ov))) return false;
        }
        return true;
    }
    void children(std::vector<Handle>& out) const override {
        for (const Entry& e : entries) if (e.key.generation != 0) { out.push_back(e.key); out.push_back(e.value); }
    }
    // Card virtuals — dirty-range rescan over `entries`, sharing children's per-entry emit (key then value).
    void gcMarkCard(std::size_t entryIndex) override { cards.mark(entryIndex); }
    void gcMarkAllCards() override { cards.markAll(); }
    void gcClearCards() override { cards.clear(); }
    void childrenInDirtyCards(std::vector<Handle>& out) const override {
        cards.forEachDirtyRange(entries.size(), [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i)
                if (entries[i].key.generation != 0) { out.push_back(entries[i].key); out.push_back(entries[i].value); }
        });
    }
    std::optional<std::vector<Handle>> iterate(KiritoVM&) override { return keys(); }
    std::optional<int64_t> length(KiritoVM&) override { return static_cast<int64_t>(count); }

    // The index slot pointing at entry `pos` — probe by that entry's hash for the slot whose value IS
    // `pos` (matched by POSITION, not key equality, so it also finds a write-only NaN key's slot).
    std::size_t slotOfPos(std::size_t pos) const {
        std::size_t mask = index.size() - 1, slot = entries[pos].hash & mask;
        while (index[slot] != static_cast<int32_t>(pos)) slot = (slot + 1) & mask;   // must exist (live)
        return slot;
    }
    // Tombstone the live entry at `pos`: kDelete its index slot, blank the entry (dropping its old->young
    // edges), shrink count. No card mark — removing an edge never needs remembering.
    void tombstone(std::size_t pos) {
        index[slotOfPos(pos)] = kDeleted;
        entries[pos].key = Handle{};
        entries[pos].value = Handle{};
        ++tombstones;
        --count;
    }

    bool remove(ObjectArena& arena, Handle key) {
        const Object& k = arena.deref(key);
        requireHashable(k);   // reject an unhashable key with the SAME message as set/find (A06-1),
                              // not a silent false that surfaces downstream as a misleading "key not found"
        std::size_t h = k.hash();
        if (probing_) throw KiritoError("Dict changed size during a key comparison");
        ProbeScope guard(probing_);
        std::size_t slot = 0;
        std::ptrdiff_t pos = probeIndex(arena, k, h, slot);
        if (pos < 0) return false;
        tombstone(static_cast<std::size_t>(pos));
        if (tombstones * 2 >= entries.size() && tombstones > 8) reindex(/*compact=*/true);
        return true;
    }

    // Remove and return the LAST (key, value) — Python `popitem` order. Takes the entry by POSITION,
    // not by looking a key back up, because a NaN key is write-only (`NaN != NaN`); the drain loop
    // `while len(d) > 0: d.popitem()` must terminate even for NaN keys. SetVal::popArbitrary is the same.
    std::pair<Handle, Handle> popArbitrary() {
        if (probing_) throw KiritoError("Dict changed size during a key comparison");
        std::size_t i = entries.size();
        while (i > 0 && entries[i - 1].key.generation == 0) --i;   // skip trailing tombstones
        if (i == 0) throw KiritoError("popitem: dictionary is empty");
        --i;                                                        // the last LIVE entry
        std::pair<Handle, Handle> kv{entries[i].key, entries[i].value};
        index[slotOfPos(i)] = kDeleted;
        std::size_t trailingDead = entries.size() - i - 1;          // dead entries after the live one
        entries.resize(i);                                          // physically drop them all -> tail stays tight
        --count;
        tombstones -= trailingDead;
        if (count == 0) index.assign(8, kEmpty);                    // fully drained -> reset the index (drop kDeleted bloat)
        return kv;
    }

    // Empty the dict. Guarded like every mutator: a reentrant _hash_/_eq_ clearing mid-probe would free
    // `entries`/`index` a live C++ probe loop holds by reference.
    void clear() {
        if (probing_) throw KiritoError("Dict changed size during a key comparison");
        entries.clear();
        index.assign(8, kEmpty);
        count = 0;
        tombstones = 0;
        cards.clear();
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
    // Same dense insertion-ordered layout as DictVal, without the value column. See DictVal for the
    // rationale (tombstoned deletes -> stable indices -> a valid CardTable over `entries`).
    struct Entry { Handle value; std::size_t hash; };
    static constexpr int32_t kEmpty = -1;
    static constexpr int32_t kDeleted = -2;
    std::vector<Entry> entries;
    std::vector<int32_t> index = std::vector<int32_t>(8, kEmpty);
    std::size_t count = 0;
    std::size_t tombstones = 0;
    CardTable cards;
    mutable bool probing_ = false;

    ValueKind kind() const override { return ValueKind::Set; }
    std::string typeName() const override { return "Set"; }
    bool truthy() const override { return count > 0; }

    std::ptrdiff_t probeIndex(const ObjectArena& arena, const Object& value, std::size_t h, std::size_t& outSlot) const {
        std::size_t mask = index.size() - 1, slot = h & mask;
        for (std::size_t n = 0; n < index.size(); ++n) {
            int32_t e = index[slot];
            if (e == kEmpty) { outSlot = slot; return -1; }
            if (e != kDeleted) {
                const Entry& ent = entries[static_cast<std::size_t>(e)];
                if (ent.hash == h && ent.value.generation != 0 &&
                    keysEqual(arena, arena.deref(ent.value), value)) { outSlot = slot; return e; }
            }
            slot = (slot + 1) & mask;
        }
        outSlot = slot;
        return -1;
    }
    void reindex(bool compact) {
        if (compact && tombstones) {
            std::vector<Entry> live; live.reserve(count);
            for (const Entry& e : entries) if (e.value.generation != 0) live.push_back(e);
            entries.swap(live);
            tombstones = 0;
            if (!gcYoung()) cards.markAll();
        }
        std::size_t newSize = 8;
        while (newSize * 3 < (entries.size() + 1) * 4) newSize *= 2;
        index.assign(newSize, kEmpty);
        std::size_t mask = newSize - 1;
        for (std::size_t p = 0; p < entries.size(); ++p) {
            if (entries[p].value.generation == 0) continue;
            std::size_t slot = entries[p].hash & mask;
            while (index[slot] != kEmpty) slot = (slot + 1) & mask;
            index[slot] = static_cast<int32_t>(p);
        }
    }
    std::size_t slotOfPos(std::size_t pos) const {
        std::size_t mask = index.size() - 1, slot = entries[pos].hash & mask;
        while (index[slot] != static_cast<int32_t>(pos)) slot = (slot + 1) & mask;
        return slot;
    }

    bool add(ObjectArena& arena, Handle value) {
        const Object& v = arena.deref(value);
        requireHashable(v);
        std::size_t h = v.hash();
        if (probing_) throw KiritoError("Set changed size during a value comparison");
        ProbeScope guard(probing_);
        std::size_t slot = 0;
        if (probeIndex(arena, v, h, slot) >= 0) return false;
        std::size_t np = entries.size();
        entries.push_back(Entry{value, h});
        index[slot] = static_cast<int32_t>(np);
        ++count;
        gcWriteBarrier(arena, this, value, np);
        if (entries.size() * 4 >= index.size() * 3) reindex(tombstones * 2 >= entries.size());
        return true;
    }
    bool remove(ObjectArena& arena, Handle value) {
        const Object& v = arena.deref(value);
        if (!v.hashable()) return false;
        std::size_t h = v.hash();
        if (probing_) throw KiritoError("Set changed size during a value comparison");
        ProbeScope guard(probing_);
        std::size_t slot = 0;
        std::ptrdiff_t pos = probeIndex(arena, v, h, slot);
        if (pos < 0) return false;
        index[slotOfPos(static_cast<std::size_t>(pos))] = kDeleted;
        entries[static_cast<std::size_t>(pos)].value = Handle{};
        ++tombstones; --count;
        if (tombstones * 2 >= entries.size() && tombstones > 8) reindex(true);
        return true;
    }
    bool contains(const ObjectArena& arena, Handle value) const {
        const Object& v = arena.deref(value);
        if (!v.hashable()) return false;
        std::size_t h = v.hash();
        ProbeScope guard(probing_);  // read: block nested mutation from a reentrant _eq_
        std::size_t slot = 0;
        return probeIndex(arena, v, h, slot) >= 0;
    }
    std::vector<Handle> items() const {
        std::vector<Handle> out; out.reserve(count);
        for (const Entry& e : entries) if (e.value.generation != 0) out.push_back(e.value);
        return out;
    }

    void clear() {
        if (probing_) throw KiritoError("Set changed size during a value comparison");
        entries.clear();
        index.assign(8, kEmpty);
        count = 0;
        tombstones = 0;
        cards.clear();
    }
    // Remove and return the LAST element (throws on empty). By POSITION, so a write-only NaN member is
    // still drainable (Set.pop() on a NaN has always worked). Trims trailing tombstones to stay O(N).
    Handle popArbitrary() {
        if (probing_) throw KiritoError("Set changed size during a value comparison");
        std::size_t i = entries.size();
        while (i > 0 && entries[i - 1].value.generation == 0) --i;
        if (i == 0) throw KiritoError("pop from an empty Set");
        --i;
        Handle v = entries[i].value;
        index[slotOfPos(i)] = kDeleted;
        std::size_t trailingDead = entries.size() - i - 1;
        entries.resize(i);
        --count;
        tombstones -= trailingDead;
        if (count == 0) index.assign(8, kEmpty);
        return v;
    }

    std::string str(StringifyCtx& ctx) const override {
        return stringifyGuarded(this, ctx, "{", "}", [&] {
            ProbeScope pguard(probing_);  // a contained value's _str_ can mutate THIS set and realloc
                                          // `entries` — reject it, don't UAF.
            std::string s;
            bool first = true;
            for (const Entry& e : entries) {
                if (e.value.generation == 0) continue;
                if (!first) s += ", ";
                first = false;
                s += stringifyChild(ctx, e.value);
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
        ProbeScope pguard(probing_);  // we iterate THIS's live entries; a reentrant _eq_ (run by
                                      // o.contains) that clears this set mid-compare must be rejected
        for (const Entry& e : entries)
            if (e.value.generation != 0 && !o.contains(arena, e.value)) return false;
        return true;
    }
    void children(std::vector<Handle>& out) const override {
        for (const Entry& e : entries) if (e.value.generation != 0) out.push_back(e.value);
    }
    void gcMarkCard(std::size_t entryIndex) override { cards.mark(entryIndex); }
    void gcMarkAllCards() override { cards.markAll(); }
    void gcClearCards() override { cards.clear(); }
    void childrenInDirtyCards(std::vector<Handle>& out) const override {
        cards.forEachDirtyRange(entries.size(), [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i)
                if (entries[i].value.generation != 0) out.push_back(entries[i].value);
        });
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
