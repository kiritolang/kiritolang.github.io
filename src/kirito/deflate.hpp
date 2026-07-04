#ifndef KIRITO_DEFLATE_HPP
#define KIRITO_DEFLATE_HPP

// A small, self-contained DEFLATE/INFLATE implementation (RFC 1951) plus zlib wrapping (RFC 1950),
// so the `zlib` module needs no external library and the binaries stay dependency-free and static.
//
// Compression uses fixed-Huffman blocks with greedy LZ77 matching (a hash chain over a 32 KiB
// window). Decompression is a full inflate: stored, fixed, and dynamic Huffman blocks. The output
// round-trips, and real zlib-produced streams decompress correctly.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kirito::deflate {

struct DeflateError : std::runtime_error {
    explicit DeflateError(const std::string& m) : std::runtime_error(m) {}
};

// ---- Adler-32 (for the zlib wrapper) ---------------------------------------------------------
inline uint32_t adler32(const std::string& data) {
    uint32_t a = 1, b = 0;
    for (unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// CRC-32 (IEEE 802.3, as gzip and PNG use). Table-free, so there is no mutable global state.
inline uint32_t crc32(const std::string& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---- bit writer (LSB-first, as DEFLATE requires) ---------------------------------------------
class BitWriter {
public:
    void bits(uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            cur_ = static_cast<uint8_t>(cur_ | (((value >> i) & 1u) << nbits_));
            if (++nbits_ == 8) { out_.push_back(static_cast<char>(cur_)); cur_ = 0; nbits_ = 0; }
        }
    }
    // Huffman codes are emitted MSB-first.
    void huff(uint32_t code, int count) {
        for (int i = count - 1; i >= 0; --i) {
            cur_ = static_cast<uint8_t>(cur_ | (((code >> i) & 1u) << nbits_));
            if (++nbits_ == 8) { out_.push_back(static_cast<char>(cur_)); cur_ = 0; nbits_ = 0; }
        }
    }
    void align() { if (nbits_) { out_.push_back(static_cast<char>(cur_)); cur_ = 0; nbits_ = 0; } }
    std::string take() { align(); return std::move(out_); }

private:
    std::string out_;
    uint8_t cur_ = 0;
    int nbits_ = 0;
};

// ---- length/distance tables (RFC 1951) -------------------------------------------------------
inline const int kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
inline const int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
inline const int kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
inline const int kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

// Fixed-Huffman literal/length code: symbol -> (code, bits). RFC 1951 3.2.6.
inline void fixedLitCode(int sym, uint32_t& code, int& bits) {
    if (sym <= 143) { code = 0x30 + sym; bits = 8; }
    else if (sym <= 255) { code = 0x190 + (sym - 144); bits = 9; }
    else if (sym <= 279) { code = sym - 256; bits = 7; }
    else { code = 0xC0 + (sym - 280); bits = 8; }
}

// ---- DEFLATE (compress) with fixed Huffman + LZ77 --------------------------------------------
inline std::string compress(const std::string& in) {
    BitWriter w;
    w.bits(1, 1);  // BFINAL = 1 (single block)
    w.bits(1, 2);  // BTYPE = 01 (fixed Huffman)

    const int kMinMatch = 3, kMaxMatch = 258, kWindow = 32768;
    const std::size_t n = in.size();
    // hash chains for 3-byte sequences
    std::vector<int> head(1 << 15, -1), prev(n, -1);
    auto hash3 = [&](std::size_t i) -> uint32_t {
        return ((static_cast<uint8_t>(in[i]) << 10) ^ (static_cast<uint8_t>(in[i + 1]) << 5) ^
                static_cast<uint8_t>(in[i + 2])) & 0x7FFF;
    };

    auto emitLiteral = [&](uint8_t c) {
        uint32_t code; int bits;
        fixedLitCode(c, code, bits);
        w.huff(code, bits);
    };

    std::size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + kMinMatch <= n) {
            uint32_t h = hash3(i);
            int j = head[h];
            int chain = 64;  // bound the search for speed
            while (j >= 0 && chain-- > 0) {
                std::size_t dist = i - static_cast<std::size_t>(j);
                if (dist > kWindow) break;
                std::size_t maxLen = std::min<std::size_t>(kMaxMatch, n - i);
                std::size_t len = 0;
                while (len < maxLen && in[j + static_cast<int>(len)] == in[i + len]) ++len;
                if (static_cast<int>(len) > bestLen) {
                    bestLen = static_cast<int>(len);
                    bestDist = static_cast<int>(dist);
                    if (bestLen >= static_cast<int>(maxLen)) break;
                }
                j = prev[j];
            }
        }
        if (bestLen >= kMinMatch) {
            // length code
            int lc = 0;
            while (lc < 28 && bestLen >= kLenBase[lc + 1]) ++lc;
            uint32_t code; int bits;
            fixedLitCode(257 + lc, code, bits);
            w.huff(code, bits);
            w.bits(bestLen - kLenBase[lc], kLenExtra[lc]);
            // distance code (fixed: 5-bit codes)
            int dc = 0;
            while (dc < 29 && bestDist >= kDistBase[dc + 1]) ++dc;
            w.huff(static_cast<uint32_t>(dc), 5);
            w.bits(bestDist - kDistBase[dc], kDistExtra[dc]);
            // insert positions into the hash chains
            std::size_t end = i + static_cast<std::size_t>(bestLen);
            for (; i + kMinMatch <= n && i < end; ++i) {
                uint32_t h = hash3(i);
                prev[i] = head[h];
                head[h] = static_cast<int>(i);
            }
            i = end;
        } else {
            emitLiteral(static_cast<uint8_t>(in[i]));
            if (i + kMinMatch <= n) {
                uint32_t h = hash3(i);
                prev[i] = head[h];
                head[h] = static_cast<int>(i);
            }
            ++i;
        }
    }
    // end-of-block symbol 256
    uint32_t code; int bits;
    fixedLitCode(256, code, bits);
    w.huff(code, bits);
    return w.take();
}

// ---- bit reader (LSB-first) ------------------------------------------------------------------
class BitReader {
public:
    explicit BitReader(std::string_view s) : s_(s) {}
    int bit() {
        if (bitpos_ == 0) {
            if (pos_ >= s_.size()) throw DeflateError("unexpected end of deflate stream");
            byte_ = static_cast<uint8_t>(s_[pos_++]);
        }
        int b = (byte_ >> bitpos_) & 1;
        bitpos_ = (bitpos_ + 1) & 7;
        return b;
    }
    uint32_t bits(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) v |= static_cast<uint32_t>(bit()) << i;
        return v;
    }
    void alignByte() { bitpos_ = 0; }
    std::string_view raw() const { return s_; }
    std::size_t bytePos() const { return pos_; }
    void setBytePos(std::size_t p) { pos_ = p; bitpos_ = 0; }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
    uint8_t byte_ = 0;
    int bitpos_ = 0;
};

// ---- canonical Huffman decoder ---------------------------------------------------------------
struct Huffman {
    std::vector<int> counts;   // number of codes of each length
    std::vector<int> symbols;  // symbols sorted by (length, value)

    void build(const std::vector<int>& lengths, int maxBits) {
        counts.assign(maxBits + 1, 0);
        for (int l : lengths) counts[l]++;
        counts[0] = 0;
        std::vector<int> offsets(maxBits + 2, 0);
        for (int i = 1; i <= maxBits; ++i) offsets[i + 1] = offsets[i] + counts[i];
        symbols.assign(lengths.size(), 0);
        for (std::size_t s = 0; s < lengths.size(); ++s)
            if (lengths[s]) symbols[offsets[lengths[s]]++] = static_cast<int>(s);
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < static_cast<int>(counts.size()); ++len) {
            code |= br.bit();
            int count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw DeflateError("invalid Huffman code");
    }
};

// Cap on inflate output (matches the 256 MiB ceiling used by the other resource guards). Without it
// a tiny crafted stream — a "zip bomb" reachable via net.get().content -> gzip/zlib.decompress on
// untrusted data — expands without bound and OOMs the process. Throw instead, like the other guards.
inline constexpr std::size_t kMaxInflateOut = 256ull * 1024 * 1024;

inline void inflateBlock(BitReader& br, const Huffman& lit, const Huffman& dist, std::string& out,
                         std::size_t maxOut) {
    while (true) {
        if (out.size() > maxOut) throw DeflateError("decompressed data exceeds the size limit");
        int sym = lit.decode(br);
        if (sym == 256) break;
        if (sym < 256) {
            out.push_back(static_cast<char>(sym));
        } else {
            sym -= 257;
            if (sym >= 29) throw DeflateError("invalid length symbol");
            int len = kLenBase[sym] + static_cast<int>(br.bits(kLenExtra[sym]));
            int dsym = dist.decode(br);
            if (dsym >= 30) throw DeflateError("invalid distance symbol");
            int d = kDistBase[dsym] + static_cast<int>(br.bits(kDistExtra[dsym]));
            if (static_cast<std::size_t>(d) > out.size()) throw DeflateError("distance too far back");
            std::size_t start = out.size() - static_cast<std::size_t>(d);
            for (int k = 0; k < len; ++k) out.push_back(out[start + static_cast<std::size_t>(k)]);
        }
    }
}

inline Huffman fixedLitHuffman() {
    std::vector<int> lengths(288);
    for (int i = 0; i <= 143; ++i) lengths[i] = 8;
    for (int i = 144; i <= 255; ++i) lengths[i] = 9;
    for (int i = 256; i <= 279; ++i) lengths[i] = 7;
    for (int i = 280; i <= 287; ++i) lengths[i] = 8;
    Huffman h; h.build(lengths, 9); return h;
}
inline Huffman fixedDistHuffman() {
    std::vector<int> lengths(30, 5);
    Huffman h; h.build(lengths, 5); return h;
}

// `consumed`, if non-null, receives the number of input bytes the stream occupied (rounded up to the
// next byte boundary past the final block) — gzip needs it to find the trailer / a following member.
// `inflate(in)` is the simple public entry point; gzip uses this directly for the consumed count.
// `in` is a view: gzip passes a suffix of the whole stream (a following member starts mid-buffer)
// WITHOUT copying it — inflating N concatenated members is O(total), not O(N * total).
inline std::string inflateImpl(std::string_view in, std::size_t maxOut, std::size_t* consumed) {
    BitReader br(in);
    std::string out;
    bool final = false;
    while (!final) {
        if (out.size() > maxOut) throw DeflateError("decompressed data exceeds the size limit");
        final = br.bit() != 0;
        int type = static_cast<int>(br.bits(2));
        switch (type) {
        case 0: {  // stored block
            br.alignByte();
            std::size_t p = br.bytePos();
            if (p + 4 > in.size()) throw DeflateError("truncated stored block header");
            uint16_t len = static_cast<uint16_t>(static_cast<uint8_t>(in[p]) | (static_cast<uint8_t>(in[p + 1]) << 8));
            uint16_t nlen = static_cast<uint16_t>(static_cast<uint8_t>(in[p + 2]) | (static_cast<uint8_t>(in[p + 3]) << 8));
            // RFC 1951 §3.2.4: NLEN is the one's-complement of LEN. Raw inflate has no checksum, so a
            // corrupt stored-block length would otherwise be silently accepted (A16-2).
            if (static_cast<uint16_t>(~len) != nlen) throw DeflateError("invalid stored block lengths");
            p += 4;  // consumed LEN + NLEN
            if (p + len > in.size()) throw DeflateError("truncated stored block");
            if (out.size() + len > maxOut) throw DeflateError("decompressed data exceeds the size limit");
            out.append(in, p, len);
            br.setBytePos(p + len);
        } break;
        case 1: {  // fixed Huffman
            static const Huffman lit = fixedLitHuffman();
            static const Huffman dist = fixedDistHuffman();
            inflateBlock(br, lit, dist, out, maxOut);
        } break;
        case 2: {  // dynamic Huffman
            int hlit = static_cast<int>(br.bits(5)) + 257;
            int hdist = static_cast<int>(br.bits(5)) + 1;
            int hclen = static_cast<int>(br.bits(4)) + 4;
            static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<int> clLengths(19, 0);
            for (int i = 0; i < hclen; ++i) clLengths[order[i]] = static_cast<int>(br.bits(3));
            Huffman clHuff; clHuff.build(clLengths, 7);
            std::vector<int> lengths;
            lengths.reserve(hlit + hdist);
            while (static_cast<int>(lengths.size()) < hlit + hdist) {
                int sym = clHuff.decode(br);
                if (sym < 16) lengths.push_back(sym);
                else if (sym == 16) {
                    if (lengths.empty()) throw DeflateError("invalid repeat");
                    int rep = 3 + static_cast<int>(br.bits(2));
                    int prev = lengths.back();
                    for (int k = 0; k < rep; ++k) lengths.push_back(prev);
                } else if (sym == 17) {
                    int rep = 3 + static_cast<int>(br.bits(3));
                    for (int k = 0; k < rep; ++k) lengths.push_back(0);
                } else {
                    int rep = 11 + static_cast<int>(br.bits(7));
                    for (int k = 0; k < rep; ++k) lengths.push_back(0);
                }
            }
            if (static_cast<int>(lengths.size()) != hlit + hdist) throw DeflateError("bad dynamic lengths");
            std::vector<int> litLengths(lengths.begin(), lengths.begin() + hlit);
            std::vector<int> distLengths(lengths.begin() + hlit, lengths.end());
            Huffman lit; lit.build(litLengths, 15);
            Huffman dist; dist.build(distLengths, 15);
            inflateBlock(br, lit, dist, out, maxOut);
        } break;
        default: {
            throw DeflateError("invalid block type");
        } break;
        }
    }
    if (consumed) { br.alignByte(); *consumed = br.bytePos(); }
    return out;
}

// Public single-argument entry point (the type `&inflate` callers — zlib/net — expect). Output is
// bounded by the zip-bomb guard; use inflateImpl when the consumed-byte count is needed (gzip).
inline std::string inflate(const std::string& in) { return inflateImpl(in, kMaxInflateOut, nullptr); }

// ---- zlib wrapper (RFC 1950): 2-byte header + deflate + 4-byte Adler-32 ----------------------
inline std::string zlibCompress(const std::string& in) {
    std::string out;
    out.push_back(static_cast<char>(0x78));  // CMF: deflate, 32K window
    out.push_back(static_cast<char>(0x9C));  // FLG: default level, check bits
    out += compress(in);
    uint32_t a = adler32(in);
    out.push_back(static_cast<char>((a >> 24) & 0xFF));
    out.push_back(static_cast<char>((a >> 16) & 0xFF));
    out.push_back(static_cast<char>((a >> 8) & 0xFF));
    out.push_back(static_cast<char>(a & 0xFF));
    return out;
}

inline std::string zlibDecompress(const std::string& in) {
    if (in.size() < 6) throw DeflateError("zlib data too short");
    uint8_t cmf = static_cast<uint8_t>(in[0]);
    if ((cmf & 0x0F) != 8) throw DeflateError("unsupported zlib compression method");
    // RFC 1950 §2.2: CINFO (high nibble of CMF) is the base-2 log of the LZ77 window minus 8, so a
    // window > 32K (CINFO > 7) is invalid for deflate.
    if ((cmf >> 4) > 7) throw DeflateError("invalid zlib window size");
    // FDICT (FLG bit 5) means a 4-byte preset-dictionary id follows the header; feeding those bytes to
    // inflate as DEFLATE data would produce a bogus "invalid block"/checksum error on an otherwise
    // valid RFC-1950 stream. Reject it clearly (preset dictionaries are unsupported).
    uint8_t flg = static_cast<uint8_t>(in[1]);
    if (flg & 0x20) throw DeflateError("zlib preset dictionary is not supported");
    // FCHECK: the 16-bit CMF*256+FLG must be a multiple of 31 (RFC 1950 §2.2).
    if (((static_cast<uint32_t>(cmf) << 8) | flg) % 31 != 0) throw DeflateError("invalid zlib header check");
    std::string body = in.substr(2, in.size() - 6);  // strip 2-byte header + 4-byte trailer
    std::string out = inflate(body);
    // verify Adler-32 trailer (big-endian)
    std::size_t t = in.size() - 4;
    uint32_t expect = (static_cast<uint8_t>(in[t]) << 24) | (static_cast<uint8_t>(in[t + 1]) << 16) |
                      (static_cast<uint8_t>(in[t + 2]) << 8) | static_cast<uint8_t>(in[t + 3]);
    if (adler32(out) != expect) throw DeflateError("zlib checksum mismatch (corrupt data)");
    return out;
}

}  // namespace kirito::deflate

#endif
