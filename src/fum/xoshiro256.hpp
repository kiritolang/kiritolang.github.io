// fum/xoshiro256.hpp
//
// fum::xoshiro256 — a header-only, C++20 implementation of the xoshiro256++
// pseudo-random number generator by David Blackman and Sebastiano Vigna
// (https://prng.di.unimi.it/).
//
// Drop-in for std::mt19937_64
// ---------------------------
//   fum::xoshiro256 exposes exactly the public surface required of a standard
//   RandomNumberEngine (see [rand.eng.mers] / [rand.req.eng]):
//     * result_type, word_size, state_size, default_seed, min(), max()
//     * default constructor, seed-value constructor, SeedSeq constructor
//     * seed(value = default_seed), seed(Sseq&)
//     * operator(), discard(unsigned long long)
//     * operator==, operator!=, operator<< / operator>>
//   Any code written against std::mt19937_64 can therefore replace the type
//   with fum::xoshiro256 without other changes.  The type also satisfies
//   std::uniform_random_bit_generator, so every std::*_distribution and every
//   <random>-consuming algorithm (std::shuffle, std::sample, …) accepts it.
//
// Properties
// ----------
//   * 256-bit state, period 2^256 − 1.
//   * Passes BigCrush and PractRand out to at least 32 TB.
//   * jump()      advances the state by 2^128 calls to operator() in O(1).
//   * long_jump() advances the state by 2^192 calls to operator() in O(1).
//     Allows partitioning the sequence into 2^128 disjoint streams of length
//     2^128 (jump), or 2^64 streams of length 2^192 (long_jump) — a facility
//     std::mt19937_64 lacks.
//
// Seeding
// -------
//   A single 64-bit seed is expanded to the four state words with splitmix64,
//   which is guaranteed to produce a non-zero state for any 64-bit input.  A
//   user-supplied state that happens to be all zeros — xoshiro's one degenerate
//   configuration — is transparently repaired to the default state.
//
// SPDX-License-Identifier: MIT

#ifndef FUM_XOSHIRO256_HPP
#define FUM_XOSHIRO256_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <istream>
#include <limits>
#include <ostream>
#include <type_traits>

namespace fum {

class xoshiro256 {
  public:
    using result_type = std::uint64_t;
    using state_type = std::array<std::uint64_t, 4>;

    // Engine characteristics required of any [rand.req.eng] engine.  The names
    // and semantics match std::mt19937_64 so generic code that reads them keeps
    // working after a drop-in swap.
    static constexpr std::size_t word_size = 64;
    static constexpr std::size_t state_size = 4;
    static constexpr result_type default_seed = 0x9E3779B97F4A7C15ULL;

    // ---- construction ------------------------------------------------------
    xoshiro256() noexcept : xoshiro256(default_seed) {}

    explicit xoshiro256(result_type seed_value) noexcept
        : state_(make_state_from_seed(seed_value)) {}

    explicit xoshiro256(const state_type& state) noexcept
        : state_(is_all_zero(state) ? make_state_from_seed(default_seed)
                                    : state) {}

    // The SeedSeq constructor is required by the standard PRNG interface.  It
    // is only enabled for types that are not convertible to result_type, so
    // that xoshiro256(0) unambiguously calls the seed-value overload.
    template <typename SeedSeq,
              typename = std::enable_if_t<
                  !std::is_convertible_v<SeedSeq, result_type> &&
                  !std::is_same_v<std::remove_cv_t<std::remove_reference_t<SeedSeq>>,
                                  xoshiro256>>>
    explicit xoshiro256(SeedSeq& seq) {
        state_ = make_state_from_seed_seq(seq);
    }

    // ---- required constants -----------------------------------------------
    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept {
        return std::numeric_limits<result_type>::max();
    }

    // ---- (re)seed ----------------------------------------------------------
    // Single signature `seed(value = default_seed)` mirrors std::mt19937_64.
    void seed(result_type value = default_seed) noexcept {
        state_ = make_state_from_seed(value);
    }
    void seed(const state_type& state) noexcept {
        state_ = is_all_zero(state) ? make_state_from_seed(default_seed) : state;
    }
    template <typename SeedSeq,
              typename = std::enable_if_t<
                  !std::is_convertible_v<SeedSeq, result_type>>>
    void seed(SeedSeq& seq) {
        state_ = make_state_from_seed_seq(seq);
    }

    // ---- generation --------------------------------------------------------
    result_type operator()() noexcept {
        // xoshiro256++ output function: rotl(s0+s3, 23) + s0.
        const result_type output =
            std::rotl(state_[0] + state_[3], 23) + state_[0];

        // Standard xoshiro256 state update.
        const result_type t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = std::rotl(state_[3], 45);
        return output;
    }

    void discard(unsigned long long z) noexcept {
        for (unsigned long long i = 0; i < z; ++i) {
            (void)(*this)();
        }
    }

    // ---- jumps -------------------------------------------------------------
    // Advance by 2^128 calls to operator() in O(1) time.  Useful for producing
    // 2^128 non-overlapping streams of length 2^128 from one generator.
    void jump() noexcept {
        static constexpr result_type kJumpPolynomial[4] = {
            0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
            0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL};
        apply_jump_polynomial(kJumpPolynomial);
    }

    // Advance by 2^192 calls to operator() in O(1) time.
    void long_jump() noexcept {
        static constexpr result_type kLongJumpPolynomial[4] = {
            0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL,
            0x77710069854ee241ULL, 0x39109bb02acbe635ULL};
        apply_jump_polynomial(kLongJumpPolynomial);
    }

    // ---- observers ---------------------------------------------------------
    [[nodiscard]] state_type state() const noexcept { return state_; }

    friend bool operator==(const xoshiro256& a, const xoshiro256& b) noexcept {
        return a.state_ == b.state_;
    }
    friend bool operator!=(const xoshiro256& a, const xoshiro256& b) noexcept {
        return !(a == b);
    }

    // ---- stream serialisation ---------------------------------------------
    // Standard PRNG convention: whitespace-separated decimal state words.
    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(
        std::basic_ostream<CharT, Traits>& os, const xoshiro256& rng) {
        const auto saved_flags = os.flags();
        const auto saved_fill = os.fill();
        os.flags(std::ios_base::dec | std::ios_base::left);
        os.fill(os.widen(' '));
        os << rng.state_[0] << ' ' << rng.state_[1] << ' ' << rng.state_[2]
           << ' ' << rng.state_[3];
        os.flags(saved_flags);
        os.fill(saved_fill);
        return os;
    }

    template <typename CharT, typename Traits>
    friend std::basic_istream<CharT, Traits>& operator>>(
        std::basic_istream<CharT, Traits>& is, xoshiro256& rng) {
        const auto saved_flags = is.flags();
        is.flags(std::ios_base::dec | std::ios_base::skipws);
        state_type parsed{};
        is >> parsed[0] >> parsed[1] >> parsed[2] >> parsed[3];
        if (is) {
            if (is_all_zero(parsed)) {
                is.setstate(std::ios_base::failbit);
            } else {
                rng.state_ = parsed;
            }
        }
        is.flags(saved_flags);
        return is;
    }

  private:
    state_type state_;

    [[nodiscard]] static constexpr bool is_all_zero(
        const state_type& s) noexcept {
        return s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 0;
    }

    // splitmix64: expand a single 64-bit seed to four state words that are
    // guaranteed non-zero and well distributed even for adversarial seeds
    // (including 0).  This is the seeding scheme recommended by the xoshiro
    // authors.
    [[nodiscard]] static state_type make_state_from_seed(
        result_type seed_value) noexcept {
        state_type words{};
        result_type z = seed_value;
        for (std::size_t i = 0; i < state_size; ++i) {
            z += 0x9E3779B97F4A7C15ULL;
            result_type v = z;
            v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
            v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
            v = v ^ (v >> 31);
            words[i] = v;
        }
        return words;
    }

    // Populate a state from a SeedSeq (standard PRNG requirement, per
    // [rand.req.eng]).  Requests eight uint_least32_t words and packs each pair
    // into one 64-bit state word — the same word-count formula
    // n * ceil(w/32) = 4 * 2 = 8 that std::mt19937_64 uses.
    template <typename SeedSeq>
    [[nodiscard]] static state_type make_state_from_seed_seq(SeedSeq& seq) {
        std::array<std::uint_least32_t, 2 * state_size> raw{};
        seq.generate(raw.begin(), raw.end());
        state_type words{};
        for (std::size_t i = 0; i < state_size; ++i) {
            // Mask to 32 bits in case uint_least32_t happens to be wider.
            const std::uint64_t high =
                static_cast<std::uint64_t>(raw[2 * i] & 0xFFFFFFFFu);
            const std::uint64_t low =
                static_cast<std::uint64_t>(raw[2 * i + 1] & 0xFFFFFFFFu);
            words[i] = (high << 32) | low;
        }
        if (is_all_zero(words)) {
            words = make_state_from_seed(default_seed);
        }
        return words;
    }

    // Apply a jump polynomial (per Blackman & Vigna): XOR-accumulate the state
    // at every bit set in the polynomial while stepping the generator.
    void apply_jump_polynomial(const result_type polynomial[4]) noexcept {
        state_type accumulator{0, 0, 0, 0};
        for (std::size_t word_index = 0; word_index < state_size; ++word_index) {
            for (int bit = 0; bit < 64; ++bit) {
                if ((polynomial[word_index] >> bit) & 1ULL) {
                    accumulator[0] ^= state_[0];
                    accumulator[1] ^= state_[1];
                    accumulator[2] ^= state_[2];
                    accumulator[3] ^= state_[3];
                }
                (void)(*this)();
            }
        }
        state_ = accumulator;
    }
};

}  // namespace fum

#endif  // FUM_XOSHIRO256_HPP
