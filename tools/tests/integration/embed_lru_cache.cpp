// embed_lru_cache.cpp — a bounded cache with a Kirito-pluggable eviction policy. C++ owns the
// storage (an ordered list of entries, each a Dict {"key","value","hits","age"}) and the capacity;
// Kirito owns the eviction POLICY — a Function(entries: List) -> Integer that returns the index of
// the entry to evict when the cache is over capacity. Two policies are supplied as Kirito functions:
// least-recently-used (evict the largest "age") and least-frequently-used (evict the smallest
// "hits"). C++ inserts/gets keys, bumps hits/age, and evicts via the policy when full.
//
// Flow on insert: C++ (store) → if over capacity → Kirito (pick victim index) → C++ (validate index,
// erase entry).

#include <cstdint>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A single cache entry mirrored on the C++ side. It crosses the boundary as a Kirito Dict.
struct Entry {
    std::string key;
    int64_t     value;
    int64_t     hits;   // how many times get() has touched this key
    int64_t     age;    // logical clock: larger == more recently inserted/used
};

class Cache {
public:
    Cache(KiritoVM& vm, std::size_t capacity, Handle policy)
        : vm_(vm), capacity_(capacity), policy_(policy) {}

    // Insert or update. On a fresh insert that overflows capacity, ask the Kirito policy which
    // existing entry to evict.
    void put(const std::string& key, int64_t value) {
        int64_t now = ++clock_;
        for (Entry& e : entries_) {
            if (e.key == key) {                 // update in place, count it as a use
                e.value = value;
                e.hits += 1;
                e.age   = now;
                return;
            }
        }
        entries_.push_back({key, value, 0, now});
        if (entries_.size() > capacity_) evict();
    }

    // Look up a key; a hit bumps hits + age (a "recent use"). Returns whether the key was present.
    bool get(const std::string& key, int64_t& out) {
        int64_t now = ++clock_;
        for (Entry& e : entries_) {
            if (e.key == key) {
                e.hits += 1;
                e.age   = now;
                out     = e.value;
                return true;
            }
        }
        return false;
    }

    bool has(const std::string& key) const {
        for (const Entry& e : entries_)
            if (e.key == key) return true;
        return false;
    }

    std::size_t size() const { return entries_.size(); }

private:
    // Hand the whole entry list to Kirito as a List of Dicts; get back the victim index.
    void evict() {
        RootScope rs(vm_);
        List es(vm_);
        for (const Entry& e : entries_) {
            Dict d(vm_);
            d.set("key",   Value(vm_, e.key));
            d.set("value", Value(vm_, e.value));
            d.set("hits",  Value(vm_, e.hits));
            d.set("age",   Value(vm_, e.age));
            es.push(Value(vm_, d.handle()));
        }
        std::array<Handle, 1> args{rs.add(es.handle())};
        Value idxV(vm_, rs.add(vm_.arena().deref(policy_).call(vm_, args)));

        // The policy MUST return an Integer index in range. Reject anything else loudly.
        if (!idxV.isInt())
            throw KiritoError("cache: eviction policy must return an Integer, got '" +
                              idxV.typeName() + "'");
        int64_t idx = idxV.asInt("evict index");
        if (idx < 0 || static_cast<std::size_t>(idx) >= entries_.size())
            throw KiritoError("cache: eviction index " + std::to_string(idx) + " out of range");
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(idx));
    }

    KiritoVM&          vm_;
    std::size_t        capacity_;
    Handle             policy_;
    std::vector<Entry> entries_;
    int64_t            clock_ = 0;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // ---- Policy: LRU — the least-recently-used entry has the SMALLEST "age" (age is a logical
    //      clock bumped on every insert/use). Return the index of the smallest "age".
    Handle lru = compile(R"KI(
Function(entries):
    var victim = 0
    var best = entries[0]["age"]
    var i = 0
    for e in entries:
        if e["age"] < best:
            best = e["age"]
            victim = i
        i = i + 1
    return victim
)KI");

    // ---- Policy: LFU — evict the entry with the SMALLEST "hits" (least frequently used). Ties are
    //      broken toward the earlier index (first seen), matching a first-wins scan.
    Handle lfu = compile(R"KI(
Function(entries):
    var victim = 0
    var best = entries[0]["hits"]
    var i = 0
    for e in entries:
        if e["hits"] < best:
            best = e["hits"]
            victim = i
        i = i + 1
    return victim
)KI");

    // ================= Scenario 1: LRU =================
    // Capacity 3. Access pattern designed so one key becomes stale.
    {
        Cache c(vm, 3, lru);
        c.put("a", 1);      // age 1
        c.put("b", 2);      // age 2
        c.put("c", 3);      // age 3
        int64_t out = 0;
        CHECK(c.get("a", out) && out == 1);   // a refreshed -> age 4
        CHECK(c.get("b", out) && out == 2);   // b refreshed -> age 5
        // Now insert d -> over capacity. Least-recently-used is "c" (age 3).
        c.put("d", 4);                        // evicts c
        CHECK(c.size() == 3);
        CHECK(c.has("a"));
        CHECK(c.has("b"));
        CHECK(c.has("d"));
        CHECK(!c.has("c"));                   // c was evicted
        // Insert e -> evict the now-stale "a" (its last use age 4 < b's 5 < d's insert < e).
        CHECK(c.get("d", out) && out == 4);   // refresh d
        c.put("e", 5);                        // evicts a (smallest age)
        CHECK(!c.has("a"));
        CHECK(c.has("b"));
        CHECK(c.has("d"));
        CHECK(c.has("e"));
    }

    // ================= Scenario 2: LFU =================
    // Capacity 3. Hammer some keys, starve one, then overflow.
    {
        Cache c(vm, 3, lfu);
        c.put("x", 10);
        c.put("y", 20);
        c.put("z", 30);
        int64_t out = 0;
        // x used a lot, y used once, z never used after insert.
        CHECK(c.get("x", out));
        CHECK(c.get("x", out));
        CHECK(c.get("x", out));
        CHECK(c.get("y", out));
        // z has the fewest hits (0) -> it's the victim on overflow.
        c.put("w", 40);                       // evicts z
        CHECK(c.size() == 3);
        CHECK(c.has("x"));
        CHECK(c.has("y"));
        CHECK(c.has("w"));
        CHECK(!c.has("z"));                    // starved key evicted
        // w has 0 hits now, y has 1, x has 3. Insert v -> evict w (fewest hits).
        c.put("v", 50);                       // evicts w
        CHECK(c.has("x"));
        CHECK(c.has("y"));
        CHECK(c.has("v"));
        CHECK(!c.has("w"));
    }

    // ================= Adversarial: out-of-range index rejected by C++ =================
    {
        Handle badIdx = compile(R"KI(
Function(entries):
    return 999
)KI");
        Cache c(vm, 2, badIdx);
        c.put("a", 1);
        c.put("b", 2);
        CHECK_THROWS(c.put("c", 3));          // overflow triggers eviction -> bad index throws
    }

    // ================= Adversarial: negative index rejected =================
    {
        Handle negIdx = compile(R"KI(
Function(entries):
    return -1
)KI");
        Cache c(vm, 2, negIdx);
        c.put("a", 1);
        c.put("b", 2);
        CHECK_THROWS(c.put("c", 3));
    }

    // ================= Adversarial: non-Integer return rejected =================
    {
        Handle notInt = compile(R"KI(
Function(entries):
    return "evict-me-please"
)KI");
        Cache c(vm, 2, notInt);
        c.put("a", 1);
        c.put("b", 2);
        CHECK_THROWS(c.put("c", 3));          // String, not Integer -> throws
    }

    // ================= Adversarial: Float return rejected (must be Integer) =================
    {
        Handle floatIdx = compile(R"KI(
Function(entries):
    return 0.0
)KI");
        Cache c(vm, 2, floatIdx);
        c.put("a", 1);
        c.put("b", 2);
        CHECK_THROWS(c.put("c", 3));          // Float is not an Integer index
    }

    return RUN_TESTS();
}
