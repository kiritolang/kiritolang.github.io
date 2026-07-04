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
// Object; everything else refers to it by Handle. Reclamation (precise mark-sweep GC) frees
// slots and bumps their generation but never moves live objects, so handles stay stable.
class ObjectArena {
public:
    // Generation 0 is RESERVED: no live handle ever carries it, so a default-constructed `Handle{}`
    // ({slot:0, gen:0}) is a reliable "no value" sentinel — it can never alias the first-allocated
    // object (which would otherwise also be {0,0}). Real generations run [1, UINT32_MAX].
    static constexpr uint32_t kFirstGen = 1;

    Handle alloc(std::unique_ptr<Object> obj) {
        if (!free_.empty()) {
            uint32_t slot = free_.back();
            free_.pop_back();
            slots_[slot].obj = std::move(obj);
            slots_[slot].occupied = true;
            return Handle{slot, slots_[slot].generation};
        }
        uint32_t slot = static_cast<uint32_t>(slots_.size());
        slots_.push_back(Slot{std::move(obj), kFirstGen, true, false});
        return Handle{slot, kFirstGen};
    }

    Object& deref(Handle h) { return *at(h).obj; }
    const Object& deref(Handle h) const { return *at(h).obj; }

    // --- mark-sweep GC primitives (driven by KiritoVM, which knows the roots) ---
    void clearMarks() {
        for (Slot& s : slots_) s.marked = false;
    }
    // Mark a slot if live and not yet marked; returns true exactly once per object per cycle so
    // the caller knows to enqueue its children.
    bool markIfUnmarked(Handle h) {
        if (h.slot >= slots_.size()) return false;
        Slot& s = slots_[h.slot];
        if (!s.occupied || s.generation != h.generation || s.marked) return false;
        s.marked = true;
        return true;
    }
    // Free every occupied-but-unmarked slot; returns how many were reclaimed.
    std::size_t sweep() {
        std::size_t freed = 0;
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.occupied && !s.marked) {
                s.obj.reset();
                s.occupied = false;
                ++freed;
                if (s.generation == UINT32_MAX) {
                    // The generation would wrap to 0 (the reserved null value) and could re-validate a
                    // long-lived stale handle (ABA). Retire the slot permanently — leave it occupied=
                    // false and OFF the free-list so it is never reused. Costs one leaked slot only
                    // after 2^32 reuses of that single slot, which no real program reaches.
                    continue;
                }
                ++s.generation;
                free_.push_back(i);
            }
        }
        return freed;
    }
    std::size_t liveCount() const {
        std::size_t n = 0;
        for (const Slot& s : slots_) if (s.occupied) ++n;
        return n;
    }
    std::size_t capacity() const { return slots_.size(); }

private:
    struct Slot {
        std::unique_ptr<Object> obj;
        uint32_t generation = kFirstGen;  // never 0 while live: gen 0 is the reserved Handle{} sentinel
        bool occupied = false;
        bool marked = false;
    };

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
};

}  // namespace kirito

#endif
