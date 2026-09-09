#ifndef KIRITO_HASHMIX_HPP
#define KIRITO_HASHMIX_HPP

// Bucket-hash mixing for the Dict/Set index — NOT a message digest (that is hashing.hpp).
//
// Dict/Set place a key at `bucketHash(seed) & mask`, where `seed` is a per-VM random value drawn from
// the OS CSPRNG (ObjectArena::hashSeed()). Folding an unpredictable seed into the placement is the
// standard defence against algorithmic-complexity ("HashDoS") attacks: without it, an attacker who
// controls keys (e.g. JSON object keys, HTTP parameters) can craft many that land in one bucket and
// drive every insert/lookup to O(n) — turning a container into a quadratic time-bomb.
//
// This is DELIBERATELY DISTINCT from Object::hash() (the logical, documented value surfaced by
// `hash.hash()` — e.g. an Integer hashes to itself). bucketHash is never observable: Dict/Set iterate
// in INSERTION order, and serialization writes values not hashes, so the seed changes no program
// output — only the internal bucket layout. Do not "unify" the two: hash() is a stable contract,
// bucketHash is a security-hardened implementation detail.
//
//   - numeric / user keys: seededMix(seed, hash()) — their hash() is not forgeable byte-by-byte, so
//     dispersing it through a seeded bijection (splitmix64) is enough to defeat crafted collisions.
//   - String / Bytes keys: seededBytes(seed, ...) — their hash() IS byte-forgeable (a fixed std::hash),
//     so the seed must enter at the byte level via a keyed PRF (SipHash-2-4, as used by Rust/Python
//     hash maps) that an attacker cannot invert without the (unseen) seed.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kirito::hashmix {

// splitmix64 finalizer: a fast bijection with strong avalanche (used to disperse hashes and to derive
// SipHash subkeys from a single 64-bit seed).
inline std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Disperse an already-computed intrinsic hash with the seed (bijective, so equal hashes stay equal —
// the Dict/Set equality invariant is preserved).
inline std::size_t seededMix(std::uint64_t seed, std::size_t h) {
    return static_cast<std::size_t>(splitmix64(seed ^ static_cast<std::uint64_t>(h)));
}

// SipHash-2-4 (Aumasson & Bernstein): a keyed pseudo-random function. Reference implementation;
// verified against the published test vector (see test_hashmix.cpp). c = 2 compression rounds, d = 4
// finalization rounds.
inline std::uint64_t siphash24(std::uint64_t k0, std::uint64_t k1, const void* data, std::size_t len) {
    auto rotl = [](std::uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    std::uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    std::uint64_t v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    const auto* in = static_cast<const unsigned char*>(data);
    const std::size_t nblocks = len / 8;
    for (std::size_t i = 0; i < nblocks; ++i) {
        std::uint64_t m;
        std::memcpy(&m, in + i * 8, 8);  // little-endian assumed (SipHash spec); portable on our targets
        v3 ^= m; round(); round(); v0 ^= m;
    }
    std::uint64_t b = static_cast<std::uint64_t>(len) << 56;
    const unsigned char* tail = in + nblocks * 8;
    switch (len & 7) {
        case 7: b |= static_cast<std::uint64_t>(tail[6]) << 48; [[fallthrough]];
        case 6: b |= static_cast<std::uint64_t>(tail[5]) << 40; [[fallthrough]];
        case 5: b |= static_cast<std::uint64_t>(tail[4]) << 32; [[fallthrough]];
        case 4: b |= static_cast<std::uint64_t>(tail[3]) << 24; [[fallthrough]];
        case 3: b |= static_cast<std::uint64_t>(tail[2]) << 16; [[fallthrough]];
        case 2: b |= static_cast<std::uint64_t>(tail[1]) << 8;  [[fallthrough]];
        case 1: b |= static_cast<std::uint64_t>(tail[0]);       break;
        case 0: break;
    }
    v3 ^= b; round(); round(); v0 ^= b;
    v2 ^= 0xff; round(); round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// Keyed byte hash for String/Bytes bucketing: derive the 128-bit SipHash key from the 64-bit VM seed.
inline std::size_t seededBytes(std::uint64_t seed, const void* p, std::size_t n) {
    std::uint64_t k0 = splitmix64(seed);
    std::uint64_t k1 = splitmix64(seed ^ 0xD1B54A32D192ED03ULL);
    return static_cast<std::size_t>(siphash24(k0, k1, p, n));
}

}  // namespace kirito::hashmix

#endif
