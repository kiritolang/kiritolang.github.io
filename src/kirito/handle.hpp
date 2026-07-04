#ifndef KIRITO_HANDLE_HPP
#define KIRITO_HANDLE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace kirito {

// A reference to a value living in a KiritoVM's ObjectArena. Trivially copyable; the
// generation distinguishes a live slot from a reused one, so a stale handle is detectable.
// Handles are how reference-assignment semantics work: two bindings holding the same Handle
// alias the same value.
//
// A default-constructed `Handle{}` ({slot:0, gen:0}) is the canonical "no value" sentinel:
// the arena never issues generation 0 to a live object (see ObjectArena::kFirstGen), so this
// can never accidentally alias a real value — in particular the first-allocated object.
struct Handle {
    uint32_t slot = 0;
    uint32_t generation = 0;
    bool operator==(const Handle&) const = default;
};

// A keyword (named) call argument: `f(name = value)`. Lives here, in a low-level header, so both
// the class/instance call paths and the function machinery can name it.
struct NamedArg { std::string name; Handle value; };

}  // namespace kirito

template <>
struct std::hash<kirito::Handle> {
    std::size_t operator()(const kirito::Handle& h) const noexcept {
        return (static_cast<std::size_t>(h.generation) << 32) ^ h.slot;
    }
};

#endif
