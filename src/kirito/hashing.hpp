#ifndef KIRITO_HASHING_HPP
#define KIRITO_HASHING_HPP

// Self-contained MD5, SHA-1, SHA-256, SHA-384 and SHA-512 implementations (no external dependency).
// Each `*Raw` returns the raw binary digest; each hex wrapper returns the lowercase hex digest. On
// top of the primitives, generic HMAC (RFC 2104) and PBKDF2 (RFC 8018) drive any of the algorithms
// via the `HashAlgo` descriptor table. Standard test vectors verified.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kirito::hashing {

inline std::string toHex(const unsigned char* d, std::size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) { out.push_back(hx[d[i] >> 4]); out.push_back(hx[d[i] & 0xF]); }
    return out;
}
inline std::string toHex(const std::string& s) {
    return toHex(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

// ---- MD5 (RFC 1321) --------------------------------------------------------------------------
inline std::string md5Raw(const std::string& msg) {
    auto rotl = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    std::string data = msg;
    uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 0; i < 8; ++i) data.push_back(static_cast<char>((bitlen >> (8 * i)) & 0xFF));

    for (std::size_t off = 0; off < data.size(); off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = static_cast<uint8_t>(data[off + i * 4]) |
                   (static_cast<uint8_t>(data[off + i * 4 + 1]) << 8) |
                   (static_cast<uint8_t>(data[off + i * 4 + 2]) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(data[off + i * 4 + 3])) << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t F; int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rotl(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    unsigned char out[16];
    uint32_t parts[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (parts[i] >> (8 * j)) & 0xFF;
    return std::string(reinterpret_cast<char*>(out), 16);
}
inline std::string md5(const std::string& msg) { return toHex(md5Raw(msg)); }

// ---- SHA-256 (FIPS 180-4) --------------------------------------------------------------------
inline std::string sha256Raw(const std::string& msg) {
    auto rotr = [](uint32_t x, int c) { return (x >> c) | (x << (32 - c)); };
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};

    std::string data = msg;
    uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bitlen >> (8 * i)) & 0xFF));

    for (std::size_t off = 0; off < data.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint8_t>(data[off + i * 4]) << 24) |
                   (static_cast<uint8_t>(data[off + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(data[off + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(data[off + i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    unsigned char out[32];
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (h[i] >> (24 - 8 * j)) & 0xFF;
    return std::string(reinterpret_cast<char*>(out), 32);
}
inline std::string sha256(const std::string& msg) { return toHex(sha256Raw(msg)); }

// ---- SHA-1 (FIPS 180-4) — useful and cheap to include ----------------------------------------
inline std::string sha1Raw(const std::string& msg) {
    auto rotl = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
    std::string data = msg;
    uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bitlen >> (8 * i)) & 0xFF));
    for (std::size_t off = 0; off < data.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint8_t>(data[off + i * 4]) << 24) |
                   (static_cast<uint8_t>(data[off + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(data[off + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(data[off + i * 4 + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rotl(a,5) + f + e + k + w[i];
            e=d; d=c; c=rotl(b,30); b=a; a=t;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    unsigned char out[20];
    uint32_t parts[5] = {h0,h1,h2,h3,h4};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (parts[i] >> (24 - 8 * j)) & 0xFF;
    return std::string(reinterpret_cast<char*>(out), 20);
}
inline std::string sha1(const std::string& msg) { return toHex(sha1Raw(msg)); }

// ---- SHA-512 / SHA-384 (FIPS 180-4) ----------------------------------------------------------
// One 64-bit-word core, parameterised by the eight IV words and the output length: SHA-512 keeps
// all 64 bytes, SHA-384 uses different IVs and truncates to the first 48. The block is 128 bytes and
// the length field is 128-bit big-endian (the top 8 bytes are always zero for a byte-length message).
inline std::string sha512Core(const std::string& msg, const uint64_t iv[8], std::size_t outLen) {
    auto rotr = [](uint64_t x, int c) { return (x >> c) | (x << (64 - c)); };
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
        0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
        0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
        0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
    uint64_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = iv[i];

    std::string data = msg;
    uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 128 != 112) data.push_back('\0');
    for (int i = 0; i < 8; ++i) data.push_back('\0');          // high 64 bits of the 128-bit length
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bitlen >> (8 * i)) & 0xFF));

    for (std::size_t off = 0; off < data.size(); off += 128) {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            uint64_t v = 0;
            for (int j = 0; j < 8; ++j) v = (v << 8) | static_cast<uint8_t>(data[off + i * 8 + j]);
            w[i] = v;
        }
        for (int i = 16; i < 80; ++i) {
            uint64_t s0 = rotr(w[i-15], 1) ^ rotr(w[i-15], 8) ^ (w[i-15] >> 7);
            uint64_t s1 = rotr(w[i-2], 19) ^ rotr(w[i-2], 61) ^ (w[i-2] >> 6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint64_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 80; ++i) {
            uint64_t S1 = rotr(e,14) ^ rotr(e,18) ^ rotr(e,41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = hh + S1 + ch + K[i] + w[i];
            uint64_t S0 = rotr(a,28) ^ rotr(a,34) ^ rotr(a,39);
            uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    unsigned char out[64];
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) out[i * 8 + j] = (h[i] >> (56 - 8 * j)) & 0xFF;
    return std::string(reinterpret_cast<char*>(out), outLen);
}
inline std::string sha512Raw(const std::string& msg) {
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    return sha512Core(msg, iv, 64);
}
inline std::string sha512(const std::string& msg) { return toHex(sha512Raw(msg)); }
inline std::string sha384Raw(const std::string& msg) {
    static const uint64_t iv[8] = {
        0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL};
    return sha512Core(msg, iv, 48);
}
inline std::string sha384(const std::string& msg) { return toHex(sha384Raw(msg)); }

// ---- Algorithm descriptor + HMAC (RFC 2104) + PBKDF2 (RFC 8018) ------------------------------
// A named handle onto a raw-digest function plus its digest and internal block sizes, so HMAC and
// PBKDF2 stay algorithm-generic. blockLen is the hash's compression-block size (64 for MD5/SHA-1/
// SHA-256, 128 for SHA-384/SHA-512) — the width HMAC pads its key to.
struct HashAlgo {
    const char* name;
    std::string (*raw)(const std::string&);
    std::size_t digestLen;
    std::size_t blockLen;
};
inline const HashAlgo* findAlgo(const std::string& name) {
    static const HashAlgo table[] = {
        {"md5", md5Raw, 16, 64},
        {"sha1", sha1Raw, 20, 64},
        {"sha256", sha256Raw, 32, 64},
        {"sha384", sha384Raw, 48, 128},
        {"sha512", sha512Raw, 64, 128},
    };
    for (const HashAlgo& a : table)
        if (name == a.name) return &a;
    return nullptr;
}

inline std::string hmacRaw(const HashAlgo& algo, const std::string& key, const std::string& msg) {
    std::string k = key.size() > algo.blockLen ? algo.raw(key) : key;
    k.resize(algo.blockLen, '\0');                       // zero-pad (or the hashed key) to a block
    std::string ipad(algo.blockLen, 0), opad(algo.blockLen, 0);
    for (std::size_t i = 0; i < algo.blockLen; ++i) {
        ipad[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x36);
        opad[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x5c);
    }
    return algo.raw(opad + algo.raw(ipad + msg));
}

inline std::string pbkdf2Raw(const HashAlgo& algo, const std::string& password, const std::string& salt,
                             uint32_t iterations, std::size_t dklen) {
    std::string dk;
    dk.reserve(dklen);
    uint32_t block = 1;
    while (dk.size() < dklen) {
        // U1 = HMAC(pw, salt || INT32BE(block)); Ui = HMAC(pw, U(i-1)); T = U1 ^ U2 ^ ... ^ Uc.
        std::string idx(4, 0);
        idx[0] = static_cast<char>((block >> 24) & 0xFF);
        idx[1] = static_cast<char>((block >> 16) & 0xFF);
        idx[2] = static_cast<char>((block >> 8) & 0xFF);
        idx[3] = static_cast<char>(block & 0xFF);
        std::string u = hmacRaw(algo, password, salt + idx);
        std::string t = u;
        for (uint32_t i = 1; i < iterations; ++i) {
            u = hmacRaw(algo, password, u);
            for (std::size_t j = 0; j < t.size(); ++j) t[j] = static_cast<char>(t[j] ^ u[j]);
        }
        dk += t;
        ++block;
    }
    dk.resize(dklen);
    return dk;
}

}  // namespace kirito::hashing

#endif
