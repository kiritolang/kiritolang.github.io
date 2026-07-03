// fum/unordered_map.hpp
//
// fum::unordered_map<Key, T, Hash, KeyEqual, Allocator>
//
// A header-only, C++20, 100%-API-compatible drop-in replacement for
// std::unordered_map that is faster and far more cache friendly.
//
// Design summary
// --------------
//   * Open-addressing index table using Robin Hood hashing with backward-shift
//     deletion (no tombstones).  Each bucket is only 8 bytes and packs a probe
//     distance together with a hash fingerprint, so the hot lookup path touches
//     a dense, contiguous array and only dereferences a stored element when the
//     fingerprint matches.  This is dramatically more cache friendly than the
//     node-per-bucket separate chaining mandated for std::unordered_map.
//
//   * Elements themselves live in a *stable* segmented node arena (a vector of
//     fixed-size chunks that are never reallocated).  This preserves the
//     std::unordered_map guarantee that references and pointers to elements stay
//     valid across insert/rehash, which a plain "flat" map cannot offer.  A
//     dense iteration vector with swap-on-erase provides O(size) cache-friendly
//     iteration without ever moving an element.
//
//   * std::hash (or any user hasher) is reused verbatim, then run through a
//     strong bit-mixing finalizer so that adversarial / poorly distributed key
//     patterns do not collapse performance.
//
// The shared engine lives in fum/detail/hash_table.hpp and is reused verbatim by
// fum::unordered_set.  This class is a thin adapter that adds the map-only
// surface (mapped_type, at, operator[], try_emplace, insert_or_assign).
//
// The container is fully allocator aware (POCCA / POCMA / POCS honoured),
// exception safe, and supports the complete C++20 std::unordered_map interface
// including node handles (extract/insert), merge, the bucket interface, the
// hash-policy interface, deduction guides and erase_if.
//
// SPDX-License-Identifier: MIT

#ifndef FUM_UNORDERED_MAP_HPP
#define FUM_UNORDERED_MAP_HPP

#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "fum/detail/hash_table.hpp"

namespace fum {
namespace detail {

template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator>
struct map_traits {
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<const Key, T>;
    using hasher = Hash;
    using key_equal = KeyEqual;
    using allocator_type = Allocator;
    static constexpr bool is_set = false;
    static const key_type& key_of(const value_type& value) noexcept {
        return value.first;
    }
};

}  // namespace detail

template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = std::allocator<std::pair<const Key, T>>>
class unordered_map
    : public detail::hash_table<
          detail::map_traits<Key, T, Hash, KeyEqual, Allocator>> {
    using base = detail::hash_table<
        detail::map_traits<Key, T, Hash, KeyEqual, Allocator>>;

  public:
    using mapped_type = T;
    using typename base::const_iterator;
    using typename base::iterator;
    using typename base::key_type;
    using typename base::size_type;
    using typename base::value_type;

    using base::base;          // inherit every constructor
    using base::operator=;     // inherit initializer-list assignment

    unordered_map() : base() {}
    unordered_map(const unordered_map&) = default;
    unordered_map(unordered_map&&) = default;
    unordered_map& operator=(const unordered_map&) = default;
    unordered_map& operator=(unordered_map&&) = default;
    ~unordered_map() = default;

    // ---- map-only element access ------------------------------------------
    mapped_type& at(const key_type& key) {
        const iterator it = this->find(key);
        if (it == this->end()) {
            throw std::out_of_range("fum::unordered_map::at: key not found");
        }
        return it->second;
    }
    const mapped_type& at(const key_type& key) const {
        const const_iterator it = this->find(key);
        if (it == this->end()) {
            throw std::out_of_range("fum::unordered_map::at: key not found");
        }
        return it->second;
    }

    mapped_type& operator[](const key_type& key) {
        return this->try_emplace_impl(key).first->second;
    }
    mapped_type& operator[](key_type&& key) {
        return this->try_emplace_impl(std::move(key)).first->second;
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const key_type& key, Args&&... args) {
        return this->try_emplace_impl(key, std::forward<Args>(args)...);
    }
    template <typename... Args>
    std::pair<iterator, bool> try_emplace(key_type&& key, Args&&... args) {
        return this->try_emplace_impl(std::move(key),
                                      std::forward<Args>(args)...);
    }
    template <typename... Args>
    iterator try_emplace(const_iterator /*hint*/, const key_type& key,
                         Args&&... args) {
        return this->try_emplace_impl(key, std::forward<Args>(args)...).first;
    }
    template <typename... Args>
    iterator try_emplace(const_iterator /*hint*/, key_type&& key,
                         Args&&... args) {
        return this->try_emplace_impl(std::move(key),
                                      std::forward<Args>(args)...)
            .first;
    }

    template <typename M>
    std::pair<iterator, bool> insert_or_assign(const key_type& key, M&& obj) {
        return this->insert_or_assign_impl(key, std::forward<M>(obj));
    }
    template <typename M>
    std::pair<iterator, bool> insert_or_assign(key_type&& key, M&& obj) {
        return this->insert_or_assign_impl(std::move(key),
                                           std::forward<M>(obj));
    }
    template <typename M>
    iterator insert_or_assign(const_iterator /*hint*/, const key_type& key,
                              M&& obj) {
        return this->insert_or_assign_impl(key, std::forward<M>(obj)).first;
    }
    template <typename M>
    iterator insert_or_assign(const_iterator /*hint*/, key_type&& key,
                              M&& obj) {
        return this->insert_or_assign_impl(std::move(key), std::forward<M>(obj))
            .first;
    }
};

// ==========================================================================
// Non-member functions
// ==========================================================================
template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator>
bool operator==(const unordered_map<Key, T, Hash, KeyEqual, Allocator>& lhs,
                const unordered_map<Key, T, Hash, KeyEqual, Allocator>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (const auto& [key, mapped] : lhs) {
        const auto it = rhs.find(key);
        if (it == rhs.end() || !(it->second == mapped)) return false;
    }
    return true;
}
template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator>
bool operator!=(const unordered_map<Key, T, Hash, KeyEqual, Allocator>& lhs,
                const unordered_map<Key, T, Hash, KeyEqual, Allocator>& rhs) {
    return !(lhs == rhs);
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator>
void swap(unordered_map<Key, T, Hash, KeyEqual, Allocator>& lhs,
          unordered_map<Key, T, Hash, KeyEqual, Allocator>& rhs) noexcept(
    noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator, typename Predicate>
typename unordered_map<Key, T, Hash, KeyEqual, Allocator>::size_type erase_if(
    unordered_map<Key, T, Hash, KeyEqual, Allocator>& map, Predicate predicate) {
    typename unordered_map<Key, T, Hash, KeyEqual, Allocator>::size_type erased =
        0;
    for (auto it = map.begin(); it != map.end();) {
        if (predicate(*it)) {
            it = map.erase(it);
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
          typename Hash = std::hash<std::remove_const_t<
              typename std::iterator_traits<InputIt>::value_type::first_type>>,
          typename KeyEqual = std::equal_to<std::remove_const_t<
              typename std::iterator_traits<InputIt>::value_type::first_type>>,
          typename Allocator = std::allocator<
              typename std::iterator_traits<InputIt>::value_type>>
unordered_map(InputIt, InputIt,
              typename std::allocator_traits<Allocator>::size_type = {},
              Hash = Hash(), KeyEqual = KeyEqual(), Allocator = Allocator())
    -> unordered_map<
        std::remove_const_t<
            typename std::iterator_traits<InputIt>::value_type::first_type>,
        typename std::iterator_traits<InputIt>::value_type::second_type, Hash,
        KeyEqual, Allocator>;

template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = std::allocator<std::pair<const Key, T>>>
unordered_map(std::initializer_list<std::pair<Key, T>>,
              typename std::allocator_traits<Allocator>::size_type = {},
              Hash = Hash(), KeyEqual = KeyEqual(), Allocator = Allocator())
    -> unordered_map<Key, T, Hash, KeyEqual, Allocator>;

template <typename InputIt, typename Allocator>
unordered_map(InputIt, InputIt,
              typename std::allocator_traits<Allocator>::size_type, Allocator)
    -> unordered_map<
        std::remove_const_t<
            typename std::iterator_traits<InputIt>::value_type::first_type>,
        typename std::iterator_traits<InputIt>::value_type::second_type,
        std::hash<std::remove_const_t<
            typename std::iterator_traits<InputIt>::value_type::first_type>>,
        std::equal_to<std::remove_const_t<
            typename std::iterator_traits<InputIt>::value_type::first_type>>,
        Allocator>;

template <typename InputIt, typename Hash, typename Allocator>
unordered_map(InputIt, InputIt,
              typename std::allocator_traits<Allocator>::size_type, Hash,
              Allocator)
    -> unordered_map<
        std::remove_const_t<
            typename std::iterator_traits<InputIt>::value_type::first_type>,
        typename std::iterator_traits<InputIt>::value_type::second_type, Hash,
        std::equal_to<std::remove_const_t<
            typename std::iterator_traits<InputIt>::value_type::first_type>>,
        Allocator>;

template <typename Key, typename T, typename Allocator>
unordered_map(std::initializer_list<std::pair<Key, T>>,
              typename std::allocator_traits<Allocator>::size_type, Allocator)
    -> unordered_map<Key, T, std::hash<Key>, std::equal_to<Key>, Allocator>;

template <typename Key, typename T, typename Hash, typename Allocator>
unordered_map(std::initializer_list<std::pair<Key, T>>,
              typename std::allocator_traits<Allocator>::size_type, Hash,
              Allocator)
    -> unordered_map<Key, T, Hash, std::equal_to<Key>, Allocator>;

}  // namespace fum

#endif  // FUM_UNORDERED_MAP_HPP
