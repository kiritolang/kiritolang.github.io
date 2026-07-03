// fum/detail/hash_table.hpp
//
// The shared open-addressing engine behind fum::unordered_map and
// fum::unordered_set.  It is parameterised on a small Traits policy so the two
// containers reuse one implementation (DRY):
//
//   Traits::key_type        the key
//   Traits::value_type      what is stored (Key for a set, pair<const Key,T>
//                           for a map)
//   Traits::mapped_type     the mapped type for a map, or void for a set
//   Traits::hasher / key_equal / allocator_type
//   Traits::is_set          true for a set (elements are immutable through
//                           iterators, node handles expose value())
//   Traits::key_of(value)   extracts the key from a stored value
//
// Design (open addressing, Robin Hood, stable arena) is documented in
// fum/unordered_map.hpp.
//
// SPDX-License-Identifier: MIT

#ifndef FUM_DETAIL_HASH_TABLE_HPP
#define FUM_DETAIL_HASH_TABLE_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace fum {
namespace detail {

// splitmix64 finalizer: turns a possibly-weak std::hash result into a value
// whose high and low bits are both well distributed.  The Robin Hood table uses
// the high bits for the home bucket and the low bits for the fingerprint, so
// good avalanche behaviour here is what defends us against adversarial keys.
[[nodiscard]] inline std::uint64_t mix_bits(std::uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

// True when Hash advertises that it already produces avalanche-quality output
// (opt-in, mirroring the ankerl::unordered_dense convention).  Such hashers
// skip the extra mixing step.
template <typename Hash, typename = void>
struct is_avalanching : std::false_type {};
template <typename Hash>
struct is_avalanching<Hash, std::void_t<typename Hash::is_avalanching>>
    : std::true_type {};

template <typename Traits>
class hash_table {
  public:
    using key_type = typename Traits::key_type;
    using value_type = typename Traits::value_type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hasher = typename Traits::hasher;
    using key_equal = typename Traits::key_equal;
    using allocator_type = typename Traits::allocator_type;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = typename std::allocator_traits<allocator_type>::pointer;
    using const_pointer =
        typename std::allocator_traits<allocator_type>::const_pointer;

  protected:
    static constexpr bool is_set = Traits::is_set;

    // Index into the node arena.  32 bits keeps the bucket array small (and so
    // cache friendly); it bounds max_size() to ~4 billion elements.
    using node_index_type = std::uint32_t;
    static constexpr node_index_type kNullIndex =
        std::numeric_limits<node_index_type>::max();

    // Robin Hood bucket.  dist_and_fingerprint packs the probe distance in the
    // high 24 bits and an 8-bit hash fingerprint in the low 8 bits; a value of
    // 0 means the bucket is empty (a real occupant always has distance >= 1).
    struct Bucket {
        std::uint32_t dist_and_fingerprint;
        node_index_type node_index;
    };

    static constexpr std::uint32_t kDistInc = 1u << 8;          // one distance unit
    static constexpr std::uint32_t kFingerprintMask = kDistInc - 1;  // low 8 bits

    // Arena node.  Storage is raw so the element's lifetime is controlled
    // explicitly; `aux` doubles as the element's slot in the iteration vector
    // while live, and as the next-free link while on the free list.
    struct Node {
        alignas(value_type) std::byte storage[sizeof(value_type)];
        node_index_type aux;

        value_type* value_ptr() noexcept {
            return std::launder(reinterpret_cast<value_type*>(&storage));
        }
        const value_type* value_ptr() const noexcept {
            return std::launder(reinterpret_cast<const value_type*>(&storage));
        }
    };

    static constexpr node_index_type kChunkSize = 1024;  // nodes per arena chunk
    static constexpr node_index_type kInitialBucketCount = 8;
    static constexpr float kDefaultMaxLoadFactor = 0.8f;

    using NodeAllocator = typename std::allocator_traits<
        allocator_type>::template rebind_alloc<Node>;
    using BucketAllocator = typename std::allocator_traits<
        allocator_type>::template rebind_alloc<Bucket>;
    using IndexVectorAllocator = typename std::allocator_traits<
        allocator_type>::template rebind_alloc<node_index_type>;
    using ChunkVectorAllocator = typename std::allocator_traits<
        allocator_type>::template rebind_alloc<Node*>;
    using AllocTraits = std::allocator_traits<allocator_type>;
    using NodeAllocTraits = std::allocator_traits<NodeAllocator>;
    using BucketAllocTraits = std::allocator_traits<BucketAllocator>;

  public:
    // ---- iterators ---------------------------------------------------------
    // The iterator stores the owning container plus a *node index* (a stable
    // identity), never a positional index.  This is what lets erase invalidate
    // only the erased element, exactly as the standard requires, even though we
    // physically iterate a dense vector with swap-on-erase.  For a set, every
    // iterator yields a const reference (elements are immutable in place).
    template <bool IsConst>
    class basic_iterator {
        friend class hash_table;
        using table_pointer =
            std::conditional_t<IsConst, const hash_table*, hash_table*>;

        table_pointer table_ = nullptr;
        node_index_type node_index_ = kNullIndex;

        basic_iterator(table_pointer table, node_index_type node_index) noexcept
            : table_(table), node_index_(node_index) {}

      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename hash_table::value_type;
        using difference_type = typename hash_table::difference_type;
        using reference = std::conditional_t<IsConst || is_set,
                                             const value_type&, value_type&>;
        using pointer = std::conditional_t<IsConst || is_set,
                                           const value_type*, value_type*>;

        basic_iterator() noexcept = default;

        // Implicit conversion iterator -> const_iterator.
        template <bool OtherConst,
                  typename = std::enable_if_t<IsConst && !OtherConst>>
        basic_iterator(const basic_iterator<OtherConst>& other) noexcept
            : table_(other.table_), node_index_(other.node_index_) {}

        reference operator*() const noexcept {
            return *table_->node_at(node_index_).value_ptr();
        }
        pointer operator->() const noexcept {
            return table_->node_at(node_index_).value_ptr();
        }

        basic_iterator& operator++() noexcept {
            const node_index_type next_position =
                table_->node_at(node_index_).aux + 1;
            node_index_ = (next_position < table_->iteration_.size())
                              ? table_->iteration_[next_position]
                              : kNullIndex;
            return *this;
        }
        basic_iterator operator++(int) noexcept {
            basic_iterator copy = *this;
            ++(*this);
            return copy;
        }

        template <bool R>
        bool operator==(const basic_iterator<R>& other) const noexcept {
            return node_index_ == other.node_index_;
        }
        template <bool R>
        bool operator!=(const basic_iterator<R>& other) const noexcept {
            return node_index_ != other.node_index_;
        }

        template <bool>
        friend class basic_iterator;
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    // ---- local (per-bucket) iterators --------------------------------------
    // A "bucket" is defined as the set of live elements that share a home slot.
    // This satisfies the standard bucket interface; it is intentionally a slow
    // path (open addressing has no real per-bucket chains).
    template <bool IsConst>
    class basic_local_iterator {
        friend class hash_table;
        using table_pointer =
            std::conditional_t<IsConst, const hash_table*, hash_table*>;

        table_pointer table_ = nullptr;
        size_type home_bucket_ = 0;
        size_type position_ = 0;  // index into iteration_ being scanned

        basic_local_iterator(table_pointer table, size_type home_bucket,
                             size_type position) noexcept
            : table_(table), home_bucket_(home_bucket), position_(position) {
            advance_to_match();
        }

        void advance_to_match() noexcept {
            while (position_ < table_->iteration_.size() &&
                   table_->home_bucket_of(table_->iteration_[position_]) !=
                       home_bucket_) {
                ++position_;
            }
        }

      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename hash_table::value_type;
        using difference_type = typename hash_table::difference_type;
        using reference = std::conditional_t<IsConst || is_set,
                                             const value_type&, value_type&>;
        using pointer = std::conditional_t<IsConst || is_set,
                                           const value_type*, value_type*>;

        basic_local_iterator() noexcept = default;

        template <bool OtherConst,
                  typename = std::enable_if_t<IsConst && !OtherConst>>
        basic_local_iterator(
            const basic_local_iterator<OtherConst>& other) noexcept
            : table_(other.table_),
              home_bucket_(other.home_bucket_),
              position_(other.position_) {}

        reference operator*() const noexcept {
            return *table_->node_at(table_->iteration_[position_]).value_ptr();
        }
        pointer operator->() const noexcept {
            return table_->node_at(table_->iteration_[position_]).value_ptr();
        }

        basic_local_iterator& operator++() noexcept {
            ++position_;
            advance_to_match();
            return *this;
        }
        basic_local_iterator operator++(int) noexcept {
            basic_local_iterator copy = *this;
            ++(*this);
            return copy;
        }

        template <bool R>
        bool operator==(const basic_local_iterator<R>& other) const noexcept {
            return position_ == other.position_;
        }
        template <bool R>
        bool operator!=(const basic_local_iterator<R>& other) const noexcept {
            return position_ != other.position_;
        }

        template <bool>
        friend class basic_local_iterator;
    };

    using local_iterator = basic_local_iterator<false>;
    using const_local_iterator = basic_local_iterator<true>;

  protected:
    // ---- data members ------------------------------------------------------
    Bucket* buckets_ = nullptr;
    size_type bucket_count_ = 0;
    size_type max_load_capacity_ = 0;  // floor(bucket_count_ * max_load_factor_)
    std::uint8_t bucket_shift_ = 64;   // home index = mixed_hash >> bucket_shift_

    std::vector<Node*, ChunkVectorAllocator> chunks_;
    node_index_type arena_high_water_ = 0;  // next never-yet-used node index
    node_index_type free_list_head_ = kNullIndex;
    std::vector<node_index_type, IndexVectorAllocator> iteration_;

    float max_load_factor_ = kDefaultMaxLoadFactor;
    [[no_unique_address]] hasher hash_function_;
    [[no_unique_address]] key_equal key_equal_;
    [[no_unique_address]] allocator_type allocator_;

    // ---- arena addressing --------------------------------------------------
    Node& node_at(node_index_type index) noexcept {
        return chunks_[index / kChunkSize][index % kChunkSize];
    }
    const Node& node_at(node_index_type index) const noexcept {
        return chunks_[index / kChunkSize][index % kChunkSize];
    }

    // ---- hashing helpers ---------------------------------------------------
    [[nodiscard]] std::uint64_t mixed_hash(const key_type& key) const {
        const std::uint64_t raw = static_cast<std::uint64_t>(hash_function_(key));
        if constexpr (is_avalanching<hasher>::value) {
            return raw;
        } else {
            return mix_bits(raw);
        }
    }
    [[nodiscard]] std::uint32_t dist_and_fingerprint_from_hash(
        std::uint64_t hash) const noexcept {
        return kDistInc | static_cast<std::uint32_t>(hash & kFingerprintMask);
    }
    [[nodiscard]] size_type home_from_hash(std::uint64_t hash) const noexcept {
        return static_cast<size_type>(hash >> bucket_shift_);
    }
    [[nodiscard]] size_type home_bucket_of(node_index_type index) const {
        return home_from_hash(mixed_hash(key_at(index)));
    }
    [[nodiscard]] size_type next_bucket(size_type index) const noexcept {
        return (index + 1) & (bucket_count_ - 1);
    }

    const key_type& key_at(node_index_type index) const noexcept {
        return Traits::key_of(*node_at(index).value_ptr());
    }

    // ---- low-level node lifetime ------------------------------------------
    void ensure_arena_slot() {
        if (free_list_head_ != kNullIndex) return;
        if (arena_high_water_ < chunks_.size() * kChunkSize) return;
        // Need a fresh chunk.
        NodeAllocator node_alloc(allocator_);
        Node* chunk = NodeAllocTraits::allocate(node_alloc, kChunkSize);
        chunks_.push_back(chunk);  // strong guarantee: chunk freed below on throw
    }

    // Reserve a node slot (memory only; value not yet constructed).
    node_index_type acquire_node() {
        ensure_arena_slot();
        if (free_list_head_ != kNullIndex) {
            const node_index_type index = free_list_head_;
            free_list_head_ = node_at(index).aux;
            return index;
        }
        return arena_high_water_++;
    }
    void release_node(node_index_type index) noexcept {
        node_at(index).aux = free_list_head_;
        free_list_head_ = index;
    }

    template <typename... Args>
    void construct_value(node_index_type index, Args&&... args) {
        AllocTraits::construct(allocator_, node_at(index).value_ptr(),
                               std::forward<Args>(args)...);
    }
    void destroy_value(node_index_type index) noexcept {
        AllocTraits::destroy(allocator_, node_at(index).value_ptr());
    }

    // Link a freshly constructed node into the dense iteration vector.
    void link_into_iteration(node_index_type index) {
        node_at(index).aux = static_cast<node_index_type>(iteration_.size());
        iteration_.push_back(index);
    }
    // Remove a node from the iteration vector via swap-with-back; the moved
    // element keeps its identity (its node address never changes).
    void unlink_from_iteration(node_index_type index) noexcept {
        const node_index_type position = node_at(index).aux;
        const node_index_type last = iteration_.back();
        iteration_[position] = last;
        node_at(last).aux = position;
        iteration_.pop_back();
    }

    // ---- Robin Hood core ---------------------------------------------------
    struct ProbeState {
        std::uint32_t dist_and_fingerprint;
        size_type bucket_index;
    };
    [[nodiscard]] ProbeState probe_start(std::uint64_t hash) const noexcept {
        ProbeState state{dist_and_fingerprint_from_hash(hash),
                         home_from_hash(hash)};
        while (state.dist_and_fingerprint <
               buckets_[state.bucket_index].dist_and_fingerprint) {
            state.dist_and_fingerprint += kDistInc;
            state.bucket_index = next_bucket(state.bucket_index);
        }
        return state;
    }

    // Insert (dist_and_fingerprint, node_index) at bucket_index, stealing from
    // richer-than-it residents along the way (classic Robin Hood shift-up).
    void place_and_shift_up(std::uint32_t dist_and_fingerprint,
                            size_type bucket_index,
                            node_index_type node_index) noexcept {
        while (buckets_[bucket_index].dist_and_fingerprint != 0) {
            Bucket& resident = buckets_[bucket_index];
            if (dist_and_fingerprint > resident.dist_and_fingerprint) {
                std::swap(dist_and_fingerprint, resident.dist_and_fingerprint);
                std::swap(node_index, resident.node_index);
            }
            dist_and_fingerprint += kDistInc;
            bucket_index = next_bucket(bucket_index);
        }
        buckets_[bucket_index] = {dist_and_fingerprint, node_index};
    }

    // Look up a key.  Returns its bucket index, or bucket_count_ if absent.
    template <typename K>
    [[nodiscard]] size_type find_bucket(const K& key,
                                        std::uint64_t hash) const {
        if (bucket_count_ == 0) return bucket_count_;
        std::uint32_t dist_and_fingerprint = dist_and_fingerprint_from_hash(hash);
        size_type bucket_index = home_from_hash(hash);
        while (true) {
            const Bucket bucket = buckets_[bucket_index];
            if (dist_and_fingerprint == bucket.dist_and_fingerprint) {
                if (key_equal_(key, key_at(bucket.node_index))) {
                    return bucket_index;
                }
            } else if (dist_and_fingerprint > bucket.dist_and_fingerprint) {
                return bucket_count_;  // out-probed the resident: not present
            }
            dist_and_fingerprint += kDistInc;
            bucket_index = next_bucket(bucket_index);
        }
    }

    // Backward-shift deletion: remove the occupant of bucket_index and pull the
    // following displaced occupants one slot closer to their home.
    void erase_bucket(size_type bucket_index) noexcept {
        size_type next_index = next_bucket(bucket_index);
        while (buckets_[next_index].dist_and_fingerprint >= 2 * kDistInc) {
            buckets_[bucket_index] = {
                buckets_[next_index].dist_and_fingerprint - kDistInc,
                buckets_[next_index].node_index};
            bucket_index = next_index;
            next_index = next_bucket(next_index);
        }
        buckets_[bucket_index] = {0, kNullIndex};
    }

    // Full teardown of the index table given a bucket index already located.
    void erase_located(size_type bucket_index) noexcept {
        const node_index_type node_index = buckets_[bucket_index].node_index;
        erase_bucket(bucket_index);
        destroy_value(node_index);
        unlink_from_iteration(node_index);
        release_node(node_index);
    }

    // ---- bucket-array (re)allocation --------------------------------------
    void deallocate_buckets() noexcept {
        if (buckets_ != nullptr) {
            BucketAllocator bucket_alloc(allocator_);
            BucketAllocTraits::deallocate(bucket_alloc, buckets_, bucket_count_);
            buckets_ = nullptr;
        }
    }

    void allocate_buckets(size_type new_bucket_count) {
        BucketAllocator bucket_alloc(allocator_);
        buckets_ = BucketAllocTraits::allocate(bucket_alloc, new_bucket_count);
        bucket_count_ = new_bucket_count;
        bucket_shift_ = static_cast<std::uint8_t>(
            64 - std::countr_zero(new_bucket_count));
        max_load_capacity_ = compute_load_capacity(new_bucket_count);
        std::memset(static_cast<void*>(buckets_), 0,
                    new_bucket_count * sizeof(Bucket));
    }

    [[nodiscard]] size_type compute_load_capacity(
        size_type count) const noexcept {
        if (max_load_factor_ >= 1.0f) {
            return count - 1;  // open addressing always needs one empty slot
        }
        size_type capacity =
            static_cast<size_type>(static_cast<float>(count) * max_load_factor_);
        return capacity < count ? capacity : count - 1;
    }

    // Rebuild the index table at new_bucket_count from the live nodes.  Node
    // identities (and therefore element addresses) are preserved.
    void rehash_to(size_type new_bucket_count) {
        deallocate_buckets();
        allocate_buckets(new_bucket_count);
        for (const node_index_type node_index : iteration_) {
            const std::uint64_t hash = mixed_hash(key_at(node_index));
            place_and_shift_up(dist_and_fingerprint_from_hash(hash),
                               home_from_hash(hash), node_index);
        }
    }

    [[nodiscard]] static size_type round_up_to_power_of_two(
        size_type value) noexcept {
        if (value < kInitialBucketCount) return kInitialBucketCount;
        return std::bit_ceil(value);
    }

    // Ensure there is room for one more element, rehashing (doubling) if needed.
    void reserve_for_one_more() {
        if (bucket_count_ == 0) {
            rehash_to(kInitialBucketCount);
        } else if (iteration_.size() + 1 > max_load_capacity_) {
            rehash_to(bucket_count_ * 2);
        }
    }

    [[nodiscard]] size_type bucket_count_for_elements(
        size_type element_count) const noexcept {
        if (element_count == 0) return 0;
        const float needed =
            static_cast<float>(element_count) / max_load_factor_;
        size_type count = static_cast<size_type>(needed) + 1;
        return round_up_to_power_of_two(count);
    }

    // ---- find-or-insert primitives ----------------------------------------
    void insert_known_absent(node_index_type node_index, std::uint64_t hash) {
        const ProbeState state = probe_start(hash);
        place_and_shift_up(state.dist_and_fingerprint, state.bucket_index,
                           node_index);
    }

    // Core for emplace/insert: construct first, then probe.  Mirrors the
    // standard, which may construct the element even when the key already
    // exists.
    template <typename... Args>
    std::pair<iterator, bool> emplace_impl(Args&&... args) {
        const node_index_type node_index = acquire_node();
        try {
            construct_value(node_index, std::forward<Args>(args)...);
        } catch (...) {
            release_node(node_index);
            throw;
        }
        const key_type& key = key_at(node_index);
        if (const size_type existing = find_bucket(key, mixed_hash(key));
            existing != bucket_count_) {
            const node_index_type existing_node =
                buckets_[existing].node_index;
            destroy_value(node_index);
            release_node(node_index);
            return {iterator(this, existing_node), false};
        }
        try {
            reserve_for_one_more();
            link_into_iteration(node_index);
        } catch (...) {
            destroy_value(node_index);
            release_node(node_index);
            throw;
        }
        insert_known_absent(node_index, mixed_hash(key));
        return {iterator(this, node_index), true};
    }

    // Core for try_emplace / operator[]: probe by key first, construct only if
    // the key is absent (map only; never instantiated for a set).
    template <typename K, typename... Args>
    std::pair<iterator, bool> try_emplace_impl(K&& key, Args&&... args) {
        std::uint64_t hash = mixed_hash(key);
        if (const size_type existing = find_bucket(key, hash);
            existing != bucket_count_) {
            return {iterator(this, buckets_[existing].node_index), false};
        }
        const node_index_type node_index = acquire_node();
        try {
            construct_value(
                node_index, std::piecewise_construct,
                std::forward_as_tuple(std::forward<K>(key)),
                std::forward_as_tuple(std::forward<Args>(args)...));
        } catch (...) {
            release_node(node_index);
            throw;
        }
        try {
            reserve_for_one_more();
            link_into_iteration(node_index);
        } catch (...) {
            destroy_value(node_index);
            release_node(node_index);
            throw;
        }
        insert_known_absent(node_index, mixed_hash(key_at(node_index)));
        return {iterator(this, node_index), true};
    }

    template <typename K, typename M>
    std::pair<iterator, bool> insert_or_assign_impl(K&& key, M&& obj) {
        std::uint64_t hash = mixed_hash(key);
        if (const size_type existing = find_bucket(key, hash);
            existing != bucket_count_) {
            const node_index_type node_index = buckets_[existing].node_index;
            node_at(node_index).value_ptr()->second = std::forward<M>(obj);
            return {iterator(this, node_index), false};
        }
        return try_emplace_impl(std::forward<K>(key), std::forward<M>(obj));
    }

    // ---- teardown ----------------------------------------------------------
    void destroy_all_values() noexcept {
        for (const node_index_type node_index : iteration_) {
            destroy_value(node_index);
        }
    }
    void deallocate_chunks() noexcept {
        NodeAllocator node_alloc(allocator_);
        for (Node* chunk : chunks_) {
            NodeAllocTraits::deallocate(node_alloc, chunk, kChunkSize);
        }
        chunks_.clear();
    }
    void destroy_everything() noexcept {
        destroy_all_values();
        deallocate_buckets();
        deallocate_chunks();
    }
    void reset_to_empty_state() noexcept {
        buckets_ = nullptr;
        bucket_count_ = 0;
        max_load_capacity_ = 0;
        bucket_shift_ = 64;
        arena_high_water_ = 0;
        free_list_head_ = kNullIndex;
        iteration_.clear();
    }

    void copy_contents_from(const hash_table& other) {
        if (other.empty()) return;
        reserve(other.size());
        for (const node_index_type node_index : other.iteration_) {
            emplace_impl(*other.node_at(node_index).value_ptr());
        }
    }

    void steal_storage_from(hash_table& other) noexcept {
        buckets_ = other.buckets_;
        bucket_count_ = other.bucket_count_;
        max_load_capacity_ = other.max_load_capacity_;
        bucket_shift_ = other.bucket_shift_;
        chunks_ = std::move(other.chunks_);
        arena_high_water_ = other.arena_high_water_;
        free_list_head_ = other.free_list_head_;
        iteration_ = std::move(other.iteration_);
        max_load_factor_ = other.max_load_factor_;
        hash_function_ = std::move(other.hash_function_);
        key_equal_ = std::move(other.key_equal_);
        other.buckets_ = nullptr;
        other.reset_to_empty_state();
    }

  public:
    // ======================================================================
    // Construction / destruction / assignment
    // ======================================================================
    hash_table() : hash_table(0) {}

    explicit hash_table(size_type bucket_count, const hasher& hash = hasher(),
                        const key_equal& equal = key_equal(),
                        const allocator_type& alloc = allocator_type())
        : chunks_(ChunkVectorAllocator(alloc)),
          iteration_(IndexVectorAllocator(alloc)),
          hash_function_(hash),
          key_equal_(equal),
          allocator_(alloc) {
        if (bucket_count > 0) {
            rehash_to(round_up_to_power_of_two(bucket_count));
        }
    }

    hash_table(size_type bucket_count, const allocator_type& alloc)
        : hash_table(bucket_count, hasher(), key_equal(), alloc) {}
    hash_table(size_type bucket_count, const hasher& hash,
               const allocator_type& alloc)
        : hash_table(bucket_count, hash, key_equal(), alloc) {}

    explicit hash_table(const allocator_type& alloc)
        : hash_table(0, hasher(), key_equal(), alloc) {}

    template <typename InputIt>
    hash_table(InputIt first, InputIt last, size_type bucket_count = 0,
               const hasher& hash = hasher(),
               const key_equal& equal = key_equal(),
               const allocator_type& alloc = allocator_type())
        : hash_table(bucket_count, hash, equal, alloc) {
        insert(first, last);
    }
    template <typename InputIt>
    hash_table(InputIt first, InputIt last, size_type bucket_count,
               const allocator_type& alloc)
        : hash_table(first, last, bucket_count, hasher(), key_equal(), alloc) {}
    template <typename InputIt>
    hash_table(InputIt first, InputIt last, size_type bucket_count,
               const hasher& hash, const allocator_type& alloc)
        : hash_table(first, last, bucket_count, hash, key_equal(), alloc) {}

    hash_table(std::initializer_list<value_type> init, size_type bucket_count = 0,
               const hasher& hash = hasher(),
               const key_equal& equal = key_equal(),
               const allocator_type& alloc = allocator_type())
        : hash_table(bucket_count, hash, equal, alloc) {
        insert(init);
    }
    hash_table(std::initializer_list<value_type> init, size_type bucket_count,
               const allocator_type& alloc)
        : hash_table(init, bucket_count, hasher(), key_equal(), alloc) {}
    hash_table(std::initializer_list<value_type> init, size_type bucket_count,
               const hasher& hash, const allocator_type& alloc)
        : hash_table(init, bucket_count, hash, key_equal(), alloc) {}

    hash_table(const hash_table& other)
        : hash_table(other, AllocTraits::select_on_container_copy_construction(
                                other.allocator_)) {}

    hash_table(const hash_table& other, const allocator_type& alloc)
        : chunks_(ChunkVectorAllocator(alloc)),
          iteration_(IndexVectorAllocator(alloc)),
          max_load_factor_(other.max_load_factor_),
          hash_function_(other.hash_function_),
          key_equal_(other.key_equal_),
          allocator_(alloc) {
        copy_contents_from(other);
    }

    hash_table(hash_table&& other) noexcept
        : buckets_(other.buckets_),
          bucket_count_(other.bucket_count_),
          max_load_capacity_(other.max_load_capacity_),
          bucket_shift_(other.bucket_shift_),
          chunks_(std::move(other.chunks_)),
          arena_high_water_(other.arena_high_water_),
          free_list_head_(other.free_list_head_),
          iteration_(std::move(other.iteration_)),
          max_load_factor_(other.max_load_factor_),
          hash_function_(std::move(other.hash_function_)),
          key_equal_(std::move(other.key_equal_)),
          allocator_(std::move(other.allocator_)) {
        other.buckets_ = nullptr;
        other.reset_to_empty_state();
    }

    hash_table(hash_table&& other, const allocator_type& alloc)
        : chunks_(ChunkVectorAllocator(alloc)),
          iteration_(IndexVectorAllocator(alloc)),
          max_load_factor_(other.max_load_factor_),
          hash_function_(std::move(other.hash_function_)),
          key_equal_(std::move(other.key_equal_)),
          allocator_(alloc) {
        if (allocator_ == other.allocator_) {
            steal_storage_from(other);
        } else {
            reserve(other.size());
            for (const node_index_type node_index : other.iteration_) {
                emplace_impl(std::move(*other.node_at(node_index).value_ptr()));
            }
            other.clear();
        }
    }

    ~hash_table() { destroy_everything(); }

    hash_table& operator=(const hash_table& other) {
        if (this == &other) return *this;
        if constexpr (AllocTraits::propagate_on_container_copy_assignment::
                          value) {
            if (allocator_ != other.allocator_) {
                destroy_everything();
                reset_to_empty_state();
                allocator_ = other.allocator_;
                chunks_ = std::vector<Node*, ChunkVectorAllocator>(
                    ChunkVectorAllocator(allocator_));
                iteration_ =
                    std::vector<node_index_type, IndexVectorAllocator>(
                        IndexVectorAllocator(allocator_));
                max_load_factor_ = other.max_load_factor_;
                hash_function_ = other.hash_function_;
                key_equal_ = other.key_equal_;
                copy_contents_from(other);
                return *this;
            }
            allocator_ = other.allocator_;
        }
        clear();
        max_load_factor_ = other.max_load_factor_;
        hash_function_ = other.hash_function_;
        key_equal_ = other.key_equal_;
        copy_contents_from(other);
        return *this;
    }

    hash_table& operator=(hash_table&& other) noexcept(
        AllocTraits::propagate_on_container_move_assignment::value&&
            std::is_nothrow_move_assignable_v<hasher>&&
                std::is_nothrow_move_assignable_v<key_equal>) {
        if (this == &other) return *this;
        constexpr bool propagate =
            AllocTraits::propagate_on_container_move_assignment::value;
        if (propagate || allocator_ == other.allocator_) {
            destroy_everything();
            reset_to_empty_state();
            if constexpr (propagate) {
                allocator_ = std::move(other.allocator_);
            }
            steal_storage_from(other);
        } else {
            clear();
            max_load_factor_ = other.max_load_factor_;
            hash_function_ = std::move(other.hash_function_);
            key_equal_ = std::move(other.key_equal_);
            reserve(other.size());
            for (const node_index_type node_index : other.iteration_) {
                emplace_impl(std::move(*other.node_at(node_index).value_ptr()));
            }
            other.clear();
        }
        return *this;
    }

    hash_table& operator=(std::initializer_list<value_type> init) {
        clear();
        insert(init);
        return *this;
    }

    allocator_type get_allocator() const noexcept { return allocator_; }

    // ======================================================================
    // Iterators
    // ======================================================================
    iterator begin() noexcept {
        return iterator(this, iteration_.empty() ? kNullIndex : iteration_[0]);
    }
    const_iterator begin() const noexcept {
        return const_iterator(this,
                              iteration_.empty() ? kNullIndex : iteration_[0]);
    }
    const_iterator cbegin() const noexcept { return begin(); }
    iterator end() noexcept { return iterator(this, kNullIndex); }
    const_iterator end() const noexcept { return const_iterator(this, kNullIndex); }
    const_iterator cend() const noexcept { return end(); }

    // ======================================================================
    // Capacity
    // ======================================================================
    [[nodiscard]] bool empty() const noexcept { return iteration_.empty(); }
    [[nodiscard]] size_type size() const noexcept { return iteration_.size(); }
    [[nodiscard]] size_type max_size() const noexcept {
        return static_cast<size_type>(kNullIndex) - 1;
    }

    // ======================================================================
    // Modifiers shared by both containers
    // ======================================================================
    void clear() noexcept {
        if (empty() && free_list_head_ == kNullIndex && arena_high_water_ == 0) {
            return;
        }
        destroy_all_values();
        iteration_.clear();
        arena_high_water_ = 0;
        free_list_head_ = kNullIndex;
        if (buckets_ != nullptr) {
            std::memset(static_cast<void*>(buckets_), 0,
                        bucket_count_ * sizeof(Bucket));
        }
    }

    std::pair<iterator, bool> insert(const value_type& value) {
        return emplace_impl(value);
    }
    std::pair<iterator, bool> insert(value_type&& value) {
        return emplace_impl(std::move(value));
    }
    template <typename P, typename = std::enable_if_t<
                              std::is_constructible_v<value_type, P&&>>>
    std::pair<iterator, bool> insert(P&& value) {
        return emplace_impl(std::forward<P>(value));
    }
    iterator insert(const_iterator /*hint*/, const value_type& value) {
        return emplace_impl(value).first;
    }
    iterator insert(const_iterator /*hint*/, value_type&& value) {
        return emplace_impl(std::move(value)).first;
    }
    template <typename P, typename = std::enable_if_t<
                              std::is_constructible_v<value_type, P&&>>>
    iterator insert(const_iterator /*hint*/, P&& value) {
        return emplace_impl(std::forward<P>(value)).first;
    }
    template <typename InputIt>
    void insert(InputIt first, InputIt last) {
        for (; first != last; ++first) emplace_impl(*first);
    }
    void insert(std::initializer_list<value_type> init) {
        insert(init.begin(), init.end());
    }

    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        return emplace_impl(std::forward<Args>(args)...);
    }
    template <typename... Args>
    iterator emplace_hint(const_iterator /*hint*/, Args&&... args) {
        return emplace_impl(std::forward<Args>(args)...).first;
    }

    iterator erase(iterator pos) {
        const node_index_type node_index = pos.node_index_;
        const std::uint64_t hash = mixed_hash(key_at(node_index));
        const size_type bucket_index = find_bucket(key_at(node_index), hash);
        const node_index_type position = node_at(node_index).aux;
        erase_located(bucket_index);
        return iterator(this, position < iteration_.size() ? iteration_[position]
                                                           : kNullIndex);
    }
    iterator erase(const_iterator pos) {
        return erase(iterator(this, pos.node_index_));
    }
    iterator erase(const_iterator first, const_iterator last) {
        IndexVectorAllocator index_alloc(allocator_);
        std::vector<node_index_type, IndexVectorAllocator> doomed(index_alloc);
        for (const_iterator it = first; it != last; ++it) {
            doomed.push_back(it.node_index_);
        }
        for (const node_index_type node_index : doomed) {
            const std::uint64_t hash = mixed_hash(key_at(node_index));
            erase_located(find_bucket(key_at(node_index), hash));
        }
        return iterator(this, last.node_index_);
    }
    size_type erase(const key_type& key) {
        const std::uint64_t hash = mixed_hash(key);
        const size_type bucket_index = find_bucket(key, hash);
        if (bucket_index == bucket_count_) return 0;
        erase_located(bucket_index);
        return 1;
    }

    void swap(hash_table& other) noexcept(
        AllocTraits::is_always_equal::value&& std::is_nothrow_swappable_v<hasher>&&
            std::is_nothrow_swappable_v<key_equal>) {
        using std::swap;
        if constexpr (AllocTraits::propagate_on_container_swap::value) {
            swap(allocator_, other.allocator_);
        }
        swap(buckets_, other.buckets_);
        swap(bucket_count_, other.bucket_count_);
        swap(max_load_capacity_, other.max_load_capacity_);
        swap(bucket_shift_, other.bucket_shift_);
        swap(chunks_, other.chunks_);
        swap(arena_high_water_, other.arena_high_water_);
        swap(free_list_head_, other.free_list_head_);
        swap(iteration_, other.iteration_);
        swap(max_load_factor_, other.max_load_factor_);
        swap(hash_function_, other.hash_function_);
        swap(key_equal_, other.key_equal_);
    }

    // ======================================================================
    // Lookup
    // ======================================================================
    size_type count(const key_type& key) const {
        return find_bucket(key, mixed_hash(key)) == bucket_count_ ? 0 : 1;
    }
    iterator find(const key_type& key) {
        const size_type bucket_index = find_bucket(key, mixed_hash(key));
        return bucket_index == bucket_count_
                   ? end()
                   : iterator(this, buckets_[bucket_index].node_index);
    }
    const_iterator find(const key_type& key) const {
        const size_type bucket_index = find_bucket(key, mixed_hash(key));
        return bucket_index == bucket_count_
                   ? end()
                   : const_iterator(this, buckets_[bucket_index].node_index);
    }
    bool contains(const key_type& key) const {
        return find_bucket(key, mixed_hash(key)) != bucket_count_;
    }
    std::pair<iterator, iterator> equal_range(const key_type& key) {
        iterator it = find(key);
        if (it == end()) return {it, it};
        iterator next = it;
        ++next;
        return {it, next};
    }
    std::pair<const_iterator, const_iterator> equal_range(
        const key_type& key) const {
        const_iterator it = find(key);
        if (it == end()) return {it, it};
        const_iterator next = it;
        ++next;
        return {it, next};
    }

    // ======================================================================
    // Bucket interface
    // ======================================================================
    size_type bucket_count() const noexcept { return bucket_count_; }
    size_type max_bucket_count() const noexcept { return size_type(1) << 31; }
    size_type bucket_size(size_type n) const {
        size_type total = 0;
        for (const node_index_type node_index : iteration_) {
            if (home_bucket_of(node_index) == n) ++total;
        }
        return total;
    }
    size_type bucket(const key_type& key) const {
        return bucket_count_ == 0 ? 0 : home_from_hash(mixed_hash(key));
    }
    local_iterator begin(size_type n) { return local_iterator(this, n, 0); }
    const_local_iterator begin(size_type n) const {
        return const_local_iterator(this, n, 0);
    }
    const_local_iterator cbegin(size_type n) const {
        return const_local_iterator(this, n, 0);
    }
    local_iterator end(size_type n) {
        return local_iterator(this, n, iteration_.size());
    }
    const_local_iterator end(size_type n) const {
        return const_local_iterator(this, n, iteration_.size());
    }
    const_local_iterator cend(size_type n) const {
        return const_local_iterator(this, n, iteration_.size());
    }

    // ======================================================================
    // Hash policy
    // ======================================================================
    float load_factor() const noexcept {
        return bucket_count_ == 0 ? 0.0f
                                  : static_cast<float>(size()) /
                                        static_cast<float>(bucket_count_);
    }
    float max_load_factor() const noexcept { return max_load_factor_; }
    void max_load_factor(float new_factor) noexcept {
        max_load_factor_ = new_factor > 0.0f ? new_factor : kDefaultMaxLoadFactor;
        if (bucket_count_ != 0) {
            max_load_capacity_ = compute_load_capacity(bucket_count_);
        }
    }
    void rehash(size_type requested_bucket_count) {
        size_type minimum_for_size = bucket_count_for_elements(size());
        size_type target = requested_bucket_count < minimum_for_size
                               ? minimum_for_size
                               : requested_bucket_count;
        if (target == 0) {
            if (bucket_count_ != 0 && empty()) {
                deallocate_buckets();
                bucket_count_ = 0;
                max_load_capacity_ = 0;
                bucket_shift_ = 64;
            }
            return;
        }
        rehash_to(round_up_to_power_of_two(target));
    }
    void reserve(size_type element_count) {
        rehash(bucket_count_for_elements(element_count));
    }

    // ======================================================================
    // Observers
    // ======================================================================
    hasher hash_function() const { return hash_function_; }
    key_equal key_eq() const { return key_equal_; }

    // ======================================================================
    // Node handles
    // ======================================================================
    class node_type {
        friend class hash_table;
        typename hash_table::value_type* value_ = nullptr;
        [[no_unique_address]] typename hash_table::allocator_type allocator_{};

        node_type(typename hash_table::value_type* value,
                  const typename hash_table::allocator_type& alloc) noexcept
            : value_(value), allocator_(alloc) {}

        void release() noexcept {
            if (value_ != nullptr) {
                AllocTraits::destroy(allocator_, value_);
                AllocTraits::deallocate(allocator_, value_, 1);
                value_ = nullptr;
            }
        }

      public:
        using value_type = typename hash_table::value_type;
        using key_type = typename hash_table::key_type;
        using mapped_type = typename Traits::mapped_type;  // void for a set
        using allocator_type = typename hash_table::allocator_type;

        constexpr node_type() noexcept = default;
        node_type(node_type&& other) noexcept
            : value_(other.value_), allocator_(std::move(other.allocator_)) {
            other.value_ = nullptr;
        }
        node_type& operator=(node_type&& other) noexcept(
            AllocTraits::propagate_on_container_move_assignment::value ||
            AllocTraits::is_always_equal::value) {
            if (this != &other) {
                release();
                value_ = other.value_;
                if constexpr (AllocTraits::
                                  propagate_on_container_move_assignment::value) {
                    allocator_ = std::move(other.allocator_);
                }
                other.value_ = nullptr;
            }
            return *this;
        }
        node_type(const node_type&) = delete;
        node_type& operator=(const node_type&) = delete;
        ~node_type() { release(); }

        [[nodiscard]] bool empty() const noexcept { return value_ == nullptr; }
        explicit operator bool() const noexcept { return value_ != nullptr; }
        allocator_type get_allocator() const { return allocator_; }

        // Set-style accessor.
        value_type& value() const { return *value_; }
        // Map-style accessors (disabled for a set).
        template <bool S = is_set, typename = std::enable_if_t<!S>>
        key_type& key() const {
            return const_cast<key_type&>(Traits::key_of(*value_));
        }
        template <bool S = is_set, typename = std::enable_if_t<!S>>
        auto& mapped() const {
            return value_->second;
        }

        void swap(node_type& other) noexcept(
            AllocTraits::propagate_on_container_swap::value ||
            AllocTraits::is_always_equal::value) {
            using std::swap;
            swap(value_, other.value_);
            if constexpr (AllocTraits::propagate_on_container_swap::value) {
                swap(allocator_, other.allocator_);
            }
        }
        friend void swap(node_type& a, node_type& b) noexcept(noexcept(a.swap(b))) {
            a.swap(b);
        }
    };

    struct insert_return_type {
        iterator position;
        bool inserted;
        node_type node;
    };

  protected:
    node_type extract_located(size_type bucket_index) {
        const node_index_type node_index = buckets_[bucket_index].node_index;
        value_type* standalone = AllocTraits::allocate(allocator_, 1);
        try {
            AllocTraits::construct(allocator_, standalone,
                                   std::move(*node_at(node_index).value_ptr()));
        } catch (...) {
            AllocTraits::deallocate(allocator_, standalone, 1);
            throw;
        }
        erase_located(bucket_index);
        return node_type(standalone, allocator_);
    }

  public:
    node_type extract(const key_type& key) {
        const size_type bucket_index = find_bucket(key, mixed_hash(key));
        if (bucket_index == bucket_count_) return node_type();
        return extract_located(bucket_index);
    }
    node_type extract(const_iterator pos) {
        const std::uint64_t hash = mixed_hash(key_at(pos.node_index_));
        return extract_located(find_bucket(key_at(pos.node_index_), hash));
    }

    insert_return_type insert(node_type&& handle) {
        if (handle.empty()) return {end(), false, node_type()};
        const key_type& key = Traits::key_of(*handle.value_);
        std::uint64_t hash = mixed_hash(key);
        if (const size_type existing = find_bucket(key, hash);
            existing != bucket_count_) {
            return {iterator(this, buckets_[existing].node_index), false,
                    std::move(handle)};
        }
        const node_index_type node_index = acquire_node();
        try {
            construct_value(node_index, std::move(*handle.value_));
        } catch (...) {
            release_node(node_index);
            throw;
        }
        try {
            reserve_for_one_more();
            link_into_iteration(node_index);
        } catch (...) {
            destroy_value(node_index);
            release_node(node_index);
            throw;
        }
        insert_known_absent(node_index, mixed_hash(key_at(node_index)));
        handle.release();
        return {iterator(this, node_index), true, node_type()};
    }
    iterator insert(const_iterator /*hint*/, node_type&& handle) {
        return insert(std::move(handle)).position;
    }

    template <typename OtherTraits>
    void merge(hash_table<OtherTraits>& source) {
        merge_from(source);
    }
    template <typename OtherTraits>
    void merge(hash_table<OtherTraits>&& source) {
        merge_from(source);
    }

  protected:
    template <typename SourceTable>
    void merge_from(SourceTable& source) {
        IndexVectorAllocator index_alloc(allocator_);
        std::vector<node_index_type, IndexVectorAllocator> source_nodes(
            index_alloc);
        source_nodes.reserve(source.size());
        for (const node_index_type source_node : source.iteration_) {
            source_nodes.push_back(source_node);
        }
        for (const node_index_type source_node : source_nodes) {
            const key_type& key = source.key_at(source_node);
            if (contains(key)) continue;
            // Locate the source slot before moving (erase_located works by
            // bucket index, not key, so it stays correct once the key is moved).
            const size_type source_bucket =
                source.find_bucket(key, source.mixed_hash(key));
            value_type& source_value = *source.node_at(source_node).value_ptr();
            if constexpr (is_set) {
                emplace_impl(std::move(source_value));
            } else {
                emplace_impl(
                    std::move(const_cast<key_type&>(source_value.first)),
                    std::move(source_value.second));
            }
            source.erase_located(source_bucket);
        }
    }

    template <typename>
    friend class hash_table;
};

}  // namespace detail
}  // namespace fum

#endif  // FUM_DETAIL_HASH_TABLE_HPP
