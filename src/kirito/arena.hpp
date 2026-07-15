#ifndef KIRITO_ARENA_HPP
#define KIRITO_ARENA_HPP

#include <memory>
#include <utility>
#include <vector>

#include "common.hpp"
#include "handle.hpp"
#include "object.hpp"

namespace kirito {

// The single owner of every shared Kirito value. A slot's unique_ptr is the sole owner of its
// Object; everything else refers to it by Handle. Reclamation (a non-moving GENERATIONAL mark-sweep
// GC) frees slots and bumps their generation but never moves live objects, so handles stay stable.
//
// Generational, non-moving: each Object carries a young/old age tag (object.hpp). A cheap MINOR
// collection scans only the young generation (enumerated by `young_`, so it is O(young), not
// O(capacity)); it treats old objects as live boundaries except those in the `remembered_` set — old
// objects the write barrier flagged as holding an old->young pointer, whose young children it must
// still trace. A full MAJOR collection is the classic scan-everything mark-sweep. Survivors are
// PROMOTED in place by flipping the age tag (no copying, no handle rewriting).
class ObjectArena {
public:
    // Generation 0 is RESERVED: no live handle ever carries it, so a default-constructed `Handle{}`
    // ({slot:0, gen:0}) is a reliable "no value" sentinel — it can never alias the first-allocated
    // object (which would otherwise also be {0,0}). Real generations run [1, UINT32_MAX].
    static constexpr uint32_t kFirstGen = 1;

    Handle alloc(std::unique_ptr<Object> obj) {
        uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
            slots_[slot].obj = std::move(obj);
            slots_[slot].occupied = true;
        } else {
            slot = static_cast<uint32_t>(slots_.size());
            slots_.push_back(Slot{std::move(obj), kFirstGen, true});
        }
        // A fresh object is YOUNG (Object's default age 0); enrol it in the nursery list so a minor
        // GC can enumerate the young generation without walking the whole slot vector.
        young_.push_back(slot);
        return Handle{slot, slots_[slot].generation};
    }

    Object& deref(Handle h) { return *at(h).obj; }
    const Object& deref(Handle h) const { return *at(h).obj; }

    // --- remembered set (old objects holding an old->young edge; populated by the write barrier) ---
    void remember(Object* o) {
        if (o->gcRemembered()) return;   // already enrolled this cycle
        o->gcSetRemembered(true);
        remembered_.push_back(o);
    }
    const std::vector<Object*>& remembered() const { return remembered_; }
    // Clear the remembered set (reset each object's flag). Sound to clear wholesale after a minor
    // because kGcOldAge == 1 promotes every surviving young object, so every old->young edge became
    // old->old this cycle (see object.hpp / the minor collector).
    void resetRemembered() {
        for (Object* o : remembered_) o->gcSetRemembered(false);
        remembered_.clear();
    }

    // --- MAJOR (full) collection primitives — driven by KiritoVM, which knows the roots ---
    void clearMarks() {
        for (Slot& s : slots_) if (s.occupied) s.obj->gcSetMarked(false);
    }
    // Mark a slot if live and not yet marked; returns true exactly once per object per cycle so
    // the caller knows to enqueue its children.
    bool markIfUnmarked(Handle h) {
        if (h.slot >= slots_.size()) return false;
        Slot& s = slots_[h.slot];
        if (!s.occupied || s.generation != h.generation || s.obj->gcMarked()) return false;
        s.obj->gcSetMarked(true);
        return true;
    }
    // Free every occupied-but-unmarked slot; promote every survivor to old and reset its cycle flags.
    // A full trace proves reachability, so the nursery is empty afterward (every survivor is old).
    std::size_t sweep() {
        std::size_t freed = 0;
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (!s.occupied) continue;
            if (!s.obj->gcMarked()) {
                freeSlot(i);
                ++freed;
            } else {
                s.obj->gcSetMarked(false);
                s.obj->gcSetAge(Object::kGcOldAge);   // survived a full trace => old
                s.obj->gcSetRemembered(false);
            }
        }
        young_.clear();
        remembered_.clear();   // flags already cleared on survivors above; freed ones are gone
        return freed;
    }

    // --- MINOR (nursery) collection primitives — scan only the young generation ---
    void clearYoungMarks() {
        for (uint32_t slot : young_) {
            Slot& s = slots_[slot];
            if (s.occupied) s.obj->gcSetMarked(false);
        }
    }
    // Mark a slot only if it is live, YOUNG, and unmarked. Old objects are live boundaries in a minor
    // (never marked, never swept), so returning false for them stops the trace from crossing into the
    // old generation — the remembered set separately seeds any old->young edges.
    bool markIfYoungUnmarked(Handle h) {
        if (h.slot >= slots_.size()) return false;
        Slot& s = slots_[h.slot];
        if (!s.occupied || s.generation != h.generation) return false;
        if (!s.obj->gcYoung() || s.obj->gcMarked()) return false;
        s.obj->gcSetMarked(true);
        return true;
    }
    // Sweep the nursery: free unmarked young objects (dead); promote marked young objects to old.
    // Compacts `young_` IN PLACE (retaining its capacity, so the next allocation burst doesn't
    // reallocate) — with kGcOldAge == 1 every survivor promotes, so it ends empty. Writing young_[keep]
    // while range-iterating is safe: keep never runs ahead of the read cursor, and size is unchanged
    // until the final resize.
    std::size_t sweepYoung() {
        std::size_t freed = 0, keep = 0;
        for (uint32_t slot : young_) {
            Slot& s = slots_[slot];
            if (!s.occupied) continue;   // defensive: a young slot is only freed here
            if (s.obj->gcMarked()) {
                s.obj->gcSetMarked(false);
                s.obj->gcPromote();
                if (s.obj->gcYoung()) young_[keep++] = slot;  // still young (only when kGcOldAge > 1)
            } else {
                freeSlot(slot);
                ++freed;
            }
        }
        young_.resize(keep);
        return freed;
    }

    std::size_t liveCount() const {
        std::size_t n = 0;
        for (const Slot& s : slots_) if (s.occupied) ++n;
        return n;
    }
    std::size_t youngCount() const { return young_.size(); }
    std::size_t capacity() const { return slots_.size(); }

private:
    struct Slot {
        std::unique_ptr<Object> obj;
        uint32_t generation = kFirstGen;  // never 0 while live: gen 0 is the reserved Handle{} sentinel
        bool occupied = false;
    };

    // Reclaim one slot: destroy its object, bump the generation (UAF guard), return it to the free list.
    void freeSlot(uint32_t i) {
        Slot& s = slots_[i];
        s.obj.reset();
        s.occupied = false;
        if (s.generation == UINT32_MAX) {
            // The generation would wrap to 0 (the reserved null value) and could re-validate a
            // long-lived stale handle (ABA). Retire the slot permanently — leave it occupied=false
            // and OFF the free-list so it is never reused. Costs one leaked slot only after 2^32
            // reuses of that single slot, which no real program reaches.
            return;
        }
        ++s.generation;
        free_.push_back(i);
    }

    // Keep the (rare) error reporting out of line so at()'s hot path stays a tiny, inlinable check.
    [[noreturn]] static void dangling(const char* why) { throw KiritoError(std::string("dangling handle (") + why + ")"); }

    const Slot& at(Handle h) const {
        if (h.slot >= slots_.size()) dangling("slot out of range");
        const Slot& slot = slots_[h.slot];
        if (!slot.occupied || slot.generation != h.generation) dangling("stale generation");
        return slot;
    }
    Slot& at(Handle h) { return const_cast<Slot&>(std::as_const(*this).at(h)); }

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::vector<uint32_t> young_;       // slot indices of the young generation (the nursery)
    std::vector<Object*> remembered_;   // old objects with an old->young edge (write-barrier set)
};

}  // namespace kirito

#endif
