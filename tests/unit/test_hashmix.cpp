// Bucket-hash mixing (hashmix.hpp) + the per-VM hash seed (ObjectArena::hashSeed()) that hardens
// Dict/Set against algorithmic-complexity ("HashDoS") attacks. Verifies the SipHash-2-4 primitive
// against its published test vectors, that the seeded mixers disperse adversarial key patterns evenly
// across buckets (the property that keeps a container linear under attack), and that each VM draws its
// own unpredictable seed (overridable via KIRITO_HASH_SEED for reproducibility).
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    // --- SipHash-2-4 published reference vectors ---
    {
        unsigned char in[15];
        for (int i = 0; i < 15; ++i) in[i] = static_cast<unsigned char>(i);
        const std::uint64_t k0 = 0x0706050403020100ULL, k1 = 0x0f0e0d0c0b0a0908ULL;
        CHECK(hashmix::siphash24(k0, k1, in, 15) == 0xa129ca6149be45e5ULL);
        CHECK(hashmix::siphash24(k0, k1, in, 0) == 0x726fdb47dd0e0e31ULL);   // empty message vector
    }

    // --- splitmix64 is a bijection with good low-bit avalanche (flipping one input bit changes the
    // output unrecognizably); seededMix is deterministic and key-sensitive ---
    {
        CHECK(hashmix::splitmix64(0) != hashmix::splitmix64(1));
        CHECK(hashmix::seededMix(42, 7) == hashmix::seededMix(42, 7));       // deterministic
        CHECK(hashmix::seededMix(42, 7) != hashmix::seededMix(43, 7));       // seed-sensitive
        CHECK(hashmix::seededBytes(1, "abc", 3) == hashmix::seededBytes(1, "abc", 3));
        CHECK(hashmix::seededBytes(1, "abc", 3) != hashmix::seededBytes(2, "abc", 3));
    }

    // --- the HashDoS property: keys whose INTRINSIC hash collides (integers i<<40 all hash, under the
    // identity int hash, to a multiple of any pow2 table size -> the same bucket) are dispersed evenly
    // once the seed is folded in. Without seeding every key would land in one bucket (load == N). ---
    {
        const std::size_t nbuckets = 4096, mask = nbuckets - 1;
        const std::uint64_t seed = 0xC0FFEE1234567890ULL;
        std::vector<int> load(nbuckets, 0);
        for (std::uint64_t i = 0; i < nbuckets; ++i) {
            std::uint64_t adversarial = i << 40;   // == 0 (mod nbuckets) for every i -> unseeded collision
            load[hashmix::seededMix(seed, static_cast<std::size_t>(adversarial)) & mask]++;
        }
        int worst = *std::max_element(load.begin(), load.end());
        CHECK(worst < 20);   // uniform load is ~1; unseeded this would be 4096. Huge margin either way.
    }
    // --- same for String/Bytes keys via SipHash: adversarial fixed-length byte keys spread out ---
    {
        const std::size_t nbuckets = 4096, mask = nbuckets - 1;
        const std::uint64_t seed = 0x1122334455667788ULL;
        std::vector<int> load(nbuckets, 0);
        for (int i = 0; i < static_cast<int>(nbuckets); ++i) {
            std::string key = "key_" + std::to_string(i);
            load[hashmix::seededBytes(seed, key.data(), key.size()) & mask]++;
        }
        CHECK(*std::max_element(load.begin(), load.end()) < 20);
    }

    // --- each VM/arena draws its own random seed from the OS CSPRNG (unpredictable per process) ---
    {
        ObjectArena a, b;
        CHECK(a.hashSeed() != b.hashSeed());   // P(collision) = 2^-64
        CHECK(a.hashSeed() != 0);
    }

    // --- KIRITO_HASH_SEED pins the seed for reproducible bucket layouts (debugging) ---
    {
#ifdef _WIN32
        _putenv_s("KIRITO_HASH_SEED", "0x1234");
#else
        setenv("KIRITO_HASH_SEED", "0x1234", 1);
#endif
        ObjectArena a;
        CHECK(a.hashSeed() == 0x1234ULL);
#ifdef _WIN32
        _putenv_s("KIRITO_HASH_SEED", "");
#else
        unsetenv("KIRITO_HASH_SEED");
#endif
    }

    return RUN_TESTS();
}
