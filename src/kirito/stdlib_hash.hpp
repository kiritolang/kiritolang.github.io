#ifndef KIRITO_STDLIB_HASH_HPP
#define KIRITO_STDLIB_HASH_HPP

#include <cstdint>
#include <span>
#include <string>

#include "builtins.hpp"
#include "bytes.hpp"
#include "deflate.hpp"   // adler32 / crc32 checksums
#include "hashing.hpp"
#include "native.hpp"

namespace kirito {

// CRC-64/XZ (ECMA-182 polynomial, reflected) — the crc64 used by the .xz format. Table-free, no
// mutable global state. Returns the raw 64-bit value; as a signed Kirito Integer the top bit makes
// large values negative (Kirito ints are int64), which is still a stable, unique checksum.
inline uint64_t crc64(const std::string& data) {
    uint64_t crc = ~0ULL;
    for (unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? (0xC96C5795D7870F42ULL ^ (crc >> 1)) : (crc >> 1);
    }
    return ~crc;
}

// The `hash` module: digests and checksums of byte data. Cryptographic-style digests MD5 / SHA-1 /
// SHA-256 (lowercase hex) and the fast non-cryptographic checksums Adler-32 / CRC-32 / CRC-64
// (Integers, as gzip/zlib/PNG/xz use). Every function accepts a String OR a Bytes (so binary data
// hashes correctly). Self-contained — no external dependency.
class HashModule : public NativeModule {
public:
    std::string name() const override { return "hash"; }
    void setup(ModuleBuilder& m) override {
        // String-or-Bytes raw view via the shared bytes.hpp helper (argStringOrBytes).
        auto digest = [&](const char* name, std::string (*fn)(const std::string&)) {
            m.fn(name, {{"data"}}, "String", [fn, name](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, name);
                return Value(vm, fn(argStringOrBytes(vm, args[0].handle(), name)));
            });
        };
        digest("md5", hashing::md5);
        digest("sha1", hashing::sha1);
        digest("sha256", hashing::sha256);
        digest("sha384", hashing::sha384);
        digest("sha512", hashing::sha512);
        // hmac(key, msg, algo="sha256") -> lowercase hex HMAC (RFC 2104). key/msg are String-or-Bytes;
        // algo is one of md5/sha1/sha256/sha384/sha512 (an unknown name throws, listing the valid set).
        m.fn("hmac", {{"key"}, {"msg"}, {"algo", "String", m.vm().makeString("sha256")}}, "String",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "hmac");
            const std::string& key = argStringOrBytes(vm, args[0].handle(), "hmac key");
            const std::string& msg = argStringOrBytes(vm, args[1].handle(), "hmac msg");
            std::string algoName = args[2].asStringRef("hmac algo");
            const hashing::HashAlgo* algo = hashing::findAlgo(algoName);
            if (!algo) throw KiritoError("hmac: unknown algorithm '" + algoName + "' (use md5/sha1/sha256/sha384/sha512)");
            return Value(vm, hashing::toHex(hashing::hmacRaw(*algo, key, msg)));
        });
        // pbkdf2(password, salt, iterations, dklen=32, algo="sha256") -> derived key as Bytes
        // (PBKDF2-HMAC, RFC 8018). password/salt are String-or-Bytes.
        m.fn("pbkdf2", {{"password"}, {"salt"}, {"iterations", "Integer"},
                        {"dklen", "Integer", m.vm().makeInt(32)}, {"algo", "String", m.vm().makeString("sha256")}}, "Bytes",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "pbkdf2");
            const std::string& pw = argStringOrBytes(vm, args[0].handle(), "pbkdf2 password");
            const std::string& salt = argStringOrBytes(vm, args[1].handle(), "pbkdf2 salt");
            int64_t iters = args[2].asInt("pbkdf2 iterations");
            int64_t dklen = args[3].asInt("pbkdf2 dklen");
            std::string algoName = args[4].asStringRef("pbkdf2 algo");
            const hashing::HashAlgo* algo = hashing::findAlgo(algoName);
            if (!algo) throw KiritoError("pbkdf2: unknown algorithm '" + algoName + "' (use md5/sha1/sha256/sha384/sha512)");
            if (iters < 1) throw KiritoError("pbkdf2: iterations must be >= 1");
            if (dklen < 1) throw KiritoError("pbkdf2: dklen must be >= 1");
            if (dklen > 1024 * 1024) throw KiritoError("pbkdf2: dklen too large (max 1048576)");
            return Bytes(vm, hashing::pbkdf2Raw(*algo, pw, salt,
                                                static_cast<uint32_t>(iters), static_cast<std::size_t>(dklen)));
        });
        // comparedigest(a, b) -> Bool: constant-time equality for MACs/digests. Both String-or-Bytes.
        // The equal-length path folds every byte with no early exit (so timing can't localise the first
        // differing byte); a length mismatch returns False (Python's hmac.comparedigest leaks length too).
        m.fn("comparedigest", {{"a"}, {"b"}}, "Bool", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "comparedigest");
            const std::string& x = argStringOrBytes(vm, args[0].handle(), "comparedigest a");
            const std::string& y = argStringOrBytes(vm, args[1].handle(), "comparedigest b");
            if (x.size() != y.size()) {
                volatile unsigned char sink = 0;
                for (std::size_t i = 0; i < x.size(); ++i) sink = static_cast<unsigned char>(sink | static_cast<unsigned char>(x[i]));
                (void)sink;
                return vm.makeBool(false);
            }
            volatile unsigned char acc = 0;
            for (std::size_t i = 0; i < x.size(); ++i)
                acc = static_cast<unsigned char>(acc | static_cast<unsigned char>(x[i] ^ y[i]));
            return vm.makeBool(acc == 0);
        });
        // 32-bit checksums (returned as a non-negative Integer).
        auto checksum32 = [&](const char* name, uint32_t (*fn)(const std::string&)) {
            m.fn(name, {{"data"}}, "Integer", [fn, name](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args args(vm, a, name);
                return Value(vm, static_cast<int64_t>(fn(argStringOrBytes(vm, args[0].handle(), name))));
            });
        };
        checksum32("adler32", deflate::adler32);
        checksum32("crc32", deflate::crc32);
        // crc64 — the 64-bit value reinterpreted as a signed Integer (top bit -> negative).
        m.fn("crc64", {{"data"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "crc64");
            return Value(vm, static_cast<int64_t>(crc64(argStringOrBytes(vm, args[0].handle(), "crc64"))));
        });
        // `hash(x)` — the SAME hash Dict/Set key off, exposed to Kirito code. Works uniformly on
        // every hashable value: Integer, Float, Bool, None, String, Bytes, and a user-class
        // instance whose class defines `_hash_`. Unhashable inputs throw the same message the
        // Dict/Set path does.
        m.fn("hash", {{"value"}}, "Integer", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "hash");
            const Object& o = vm.arena().deref(args[0].handle());
            if (!o.hashable())
                throw KiritoError("unhashable type '" + o.typeName() + "'");
            // The Object::hash() virtual returns size_t; reinterpret as int64_t so the Integer
            // preserves every bit (dict-lookup identity is what matters, not the numeric value).
            return Value(vm, static_cast<int64_t>(o.hash()));
        });
    }
};

}  // namespace kirito

#endif
