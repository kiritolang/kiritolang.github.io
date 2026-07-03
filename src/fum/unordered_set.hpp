// fum/unordered_set.hpp
//
// fum::unordered_set<Key, Hash, KeyEqual, Allocator>
//
// A header-only, C++20, 100%-API-compatible drop-in replacement for
// std::unordered_set, built on the very same open-addressing engine as
// fum::unordered_map (see fum/detail/hash_table.hpp).  It is faster and more
// cache friendly than std::unordered_set while preserving the standard's
// reference/pointer-stability guarantee (elements live in a never-moved arena).
//
// As required by the standard, set elements are immutable in place: both
// iterator and const_iterator yield `const Key&`, and the node handle exposes
// value() rather than key()/mapped().
//
// SPDX-License-Identifier: MIT

#ifndef FUM_UNORDERED_SET_HPP
#define FUM_UNORDERED_SET_HPP

#include <functional>
#include <iterator>
#include <memory>
#include <type_traits>

#include "fum/detail/hash_table.hpp"

namespace fum {
namespace detail {

template <typename Key, typename Hash, typename KeyEqual, typename Allocator>
struct set_traits {
    using key_type = Key;
    using mapped_type = void;
    using value_type = Key;
    using hasher = Hash;
    using key_equal = KeyEqual;
    using allocator_type = Allocator;
    static constexpr bool is_set = true;
    static const key_type& key_of(const value_type& value) noexcept {
        return value;
    }
};

}  // namespace detail

template <typename Key, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = std::allocator<Key>>
class unordered_set
    : public detail::hash_table<
          detail::set_traits<Key, Hash, KeyEqual, Allocator>> {
    using base =
        detail::hash_table<detail::set_traits<Key, Hash, KeyEqual, Allocator>>;

  public:
    using typename base::const_iterator;
    using typename base::iterator;
    using typename base::key_type;
    using typename base::size_type;
    using typename base::value_type;

    using base::base;       // inherit every constructor
    using base::operator=;  // inherit initializer-list assignment

    unordered_set() : base() {}
    unordered_set(const unordered_set&) = default;
    unordered_set(unordered_set&&) = default;
    unordered_set& operator=(const unordered_set&) = default;
    unordered_set& operator=(unordered_set&&) = default;
    ~unordered_set() = default;

    // The set adds no members beyond the shared engine: insert/emplace/find/
    // erase/extract/merge/bucket/hash-policy are all inherited unchanged.
};

// ==========================================================================
// Non-member functions
// ==========================================================================
template <typename Key, typename Hash, typename KeyEqual, typename Allocator>
bool operator==(const unordered_set<Key, Hash, KeyEqual, Allocator>& lhs,
                const unordered_set<Key, Hash, KeyEqual, Allocator>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (const auto& key : lhs) {
        if (!rhs.contains(key)) return false;
    }
    return true;
}
template <typename Key, typename Hash, typename KeyEqual, typename Allocator>
bool operator!=(const unordered_set<Key, Hash, KeyEqual, Allocator>& lhs,
                const unordered_set<Key, Hash, KeyEqual, Allocator>& rhs) {
    return !(lhs == rhs);
}

template <typename Key, typename Hash, typename KeyEqual, typename Allocator>
void swap(unordered_set<Key, Hash, KeyEqual, Allocator>& lhs,
          unordered_set<Key, Hash, KeyEqual, Allocator>& rhs) noexcept(
    noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
}

template <typename Key, typename Hash, typename KeyEqual, typename Allocator,
          typename Predicate>
typename unordered_set<Key, Hash, KeyEqual, Allocator>::size_type erase_if(
    unordered_set<Key, Hash, KeyEqual, Allocator>& set, Predicate predicate) {
    typename unordered_set<Key, Hash, KeyEqual, Allocator>::size_type erased = 0;
    for (auto it = set.begin(); it != set.end();) {
        if (predicate(*it)) {
            it = set.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }
    return erased;
}

// ==========================================================================
// Deduction guides
// ==========================================================================
template <typename InputIt,
          typename Hash = std::hash<
              typename std::iterator_traits<InputIt>::value_type>,
          typename KeyEqual = std::equal_to<
              typename std::iterator_traits<InputIt>::value_type>,
          typename Allocator = std::allocator<
              typename std::iterator_traits<InputIt>::value_type>>
unordered_set(InputIt, InputIt,
              typename std::allocator_traits<Allocator>::size_type = {},
              Hash = Hash(), KeyEqual = KeyEqual(), Allocator = Allocator())
    -> unordered_set<typename std::iterator_traits<InputIt>::value_type, Hash,
                     KeyEqual, Allocator>;

template <typename Key, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = std::allocator<Key>>
unordered_set(std::initializer_list<Key>,
              typename std::allocator_traits<Allocator>::size_type = {},
              Hash = Hash(), KeyEqual = KeyEqual(), Allocator = Allocator())
    -> unordered_set<Key, Hash, KeyEqual, Allocator>;

template <typename InputIt, typename Allocator>
unordered_set(InputIt, InputIt,
              typename std::allocator_traits<Allocator>::size_type, Allocator)
    -> unordered_set<typename std::iterator_traits<InputIt>::value_type,
                     std::hash<typename std::iterator_traits<InputIt>::value_type>,
                     std::equal_to<
                         typename std::iterator_traits<InputIt>::value_type>,
                     Allocator>;

template <typename InputIt, typename Hash, typename Allocator>
unordered_set(InputIt, InputIt,
              typename std::allocator_traits<Allocator>::size_type, Hash,
              Allocator)
    -> unordered_set<typename std::iterator_traits<InputIt>::value_type, Hash,
                     std::equal_to<
                         typename std::iterator_traits<InputIt>::value_type>,
                     Allocator>;

template <typename Key, typename Allocator>
unordered_set(std::initializer_list<Key>,
              typename std::allocator_traits<Allocator>::size_type, Allocator)
    -> unordered_set<Key, std::hash<Key>, std::equal_to<Key>, Allocator>;

template <typename Key, typename Hash, typename Allocator>
unordered_set(std::initializer_list<Key>,
              typename std::allocator_traits<Allocator>::size_type, Hash,
              Allocator)
    -> unordered_set<Key, Hash, std::equal_to<Key>, Allocator>;

}  // namespace fum

#endif  // FUM_UNORDERED_SET_HPP
