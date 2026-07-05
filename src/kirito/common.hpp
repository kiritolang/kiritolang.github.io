#ifndef KIRITO_COMMON_HPP
#define KIRITO_COMMON_HPP

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

// AddressSanitizer/ThreadSanitizer detection (GCC defines __SANITIZE_ADDRESS__; Clang exposes
// __has_feature). Sanitizer builds use much larger native frames (redzones + shadow), so recursive
// descents (the parser, the compiler) overflow the stack at a far shallower depth — the depth guards
// below pick a lower bound under a sanitizer so they throw a clean error instead of crashing. Defined
// here, in the first-included header, so every translation unit (parser, vm, ...) sees it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)  // GCC: asan / tsan
#  define KIRITO_SANITIZER_BUILD 1
#elif defined(__has_feature)                                       // Clang
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define KIRITO_SANITIZER_BUILD 1
#  endif
#endif

namespace kirito {

// Position of a token / node in source, for diagnostics. 1-based line/col.
struct SourceSpan {
    uint32_t line = 0;
    uint32_t col = 0;
    uint32_t length = 0;
};

// Shared operator vocabulary used by the AST, the value protocol, and the evaluator alike, so
// none of them needs to know the others' enums.
enum class BinOp { Add, Sub, Mul, Div, FloorDiv, Mod, Pow, Eq, Ne, Lt, Le, Gt, Ge, In, NotIn };
enum class UnOp { Neg, Not };

// One frame of an error's call-stack traceback — the function, source file, and line that were
// executing. Accumulated as an error unwinds (each escaped VM frame appends itself, innermost first),
// so a "Traceback (most recent call last)" can be reconstructed. VM-local: lives only on
// the in-flight exception and the VM's last-traceback snapshot.
struct TraceFrame {
    std::string function;  // the function's name, "<function>" (anonymous), or "<module>" (top level)
    std::string file;      // source chunk the frame was running in
    uint32_t line = 0;     // line being executed (the call site for outer frames, the error site innermost)
};

// Render a traceback as a "most recent call last" block: the accumulated frames are
// innermost-first, so print them in reverse (outermost frame first, the error site last). Empty for
// an empty traceback. Shared by `sys.traceback()` and the CLI's uncaught-error reporter.
inline std::string formatTraceback(const std::vector<TraceFrame>& tb) {
    if (tb.empty()) return "";
    std::string out = "Traceback (most recent call last):\n";
    for (std::size_t i = tb.size(); i-- > 0;) {
        const TraceFrame& f = tb[i];
        out += "  File \"" + (f.file.empty() ? std::string("<main>") : f.file) + "\", line " +
               std::to_string(f.line) + ", in " +
               (f.function.empty() ? std::string("<function>") : f.function) + "\n";
    }
    return out;
}

// KiritoError + KiritoThrow live in exceptions.hpp so their inheritance relationship (both derive
// from a common std::exception base, so an embedder can `catch (const std::exception&)` and get
// either) is defined next to the source of truth for the C++/Kirito exception protocol.
// Re-exported here at the bottom of the file so the ~40 sites that just include common.hpp still
// see the classes. Do NOT move it above — SourceSpan / TraceFrame are used by these classes and
// need to be defined by the time exceptions.hpp reaches its class bodies.

// Parse a double from `s` without std::stod's underflow trap: std::stod throws std::out_of_range
// when a value underflows to a subnormal/zero (errno==ERANGE), which would crash the lexer and the
// serializers on a perfectly representable tiny literal like `5e-324`. std::strtod instead RETURNS
// the (subnormal/zero) value and merely sets errno, so we accept underflow and only reject a
// genuine non-parse (no digits consumed) or a true overflow to ±inf. `consumed`, if non-null,
// receives the number of characters parsed (like std::stod's `pos`), for trailing-garbage checks.
inline double parseDouble(const std::string& s, std::size_t* consumed = nullptr) {
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(begin, &end);
    if (end == begin) throw std::invalid_argument("parseDouble: no conversion");
    if (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL))
        throw std::out_of_range("parseDouble: overflow");   // ±inf — genuine out-of-range
    if (consumed) *consumed = static_cast<std::size_t>(end - begin);
    return v;   // underflow (subnormal/zero) is accepted, not thrown
}

// The numeric value of a hex digit (0–15), or -1 if `d` is not a hex digit. The single source of
// truth for hex parsing — reused by the lexer's `\xHH` escape, the parser's f-string escapes, and the
// JSON `\uXXXX` decoder, which each used to hand-roll the same three-range check.
inline int hexDigitValue(char d) {
    if (d >= '0' && d <= '9') return d - '0';
    if (d >= 'a' && d <= 'f') return d - 'a' + 10;
    if (d >= 'A' && d <= 'F') return d - 'A' + 10;
    return -1;
}

// Decode ONE cooked (non-raw) backslash escape from `src[i]`, where `src[i]` must be the backslash.
// Appends the decoded bytes to `out` and returns the number of source characters consumed (backslash
// + body). On a bad/incomplete escape it consumes nothing, writes a human message to `err`, and
// returns 0 — the CALLER then throws with its own SourceSpan (the lexer's line/col, the parser's f-
// string span), so diagnostics keep their location. THE single source of truth for cooked escapes:
// the plain-string lexer and the f-string literal-part decoder both call this, so `\xHH` (a code
// point emitted as UTF-8, NOT a raw byte) and the bad-escape error can never drift between the two
// spellings again (A01-1/A01-2/A01-3). Supported: \n \t \r \0 \\ \" \' and \xHH.
inline std::size_t decodeCookedEscape(std::string_view src, std::size_t i, std::string& out,
                                      std::string& err) {
    if (i + 1 >= src.size()) { err = "dangling backslash in string"; return 0; }
    char e = src[i + 1];
    if (e == 'x') {
        if (i + 3 >= src.size()) { err = "invalid \\x escape (expected two hex digits)"; return 0; }
        int hi = hexDigitValue(src[i + 2]), lo = hexDigitValue(src[i + 3]);
        if (hi < 0 || lo < 0) { err = "invalid \\x escape (expected hex digit)"; return 0; }
        // A String is a sequence of Unicode CODE POINTS (stored UTF-8), so `\xHH` is U+00HH — emit its
        // UTF-8 encoding (1 byte for HH<0x80, else 2), never a raw high byte (which would merge with
        // following bytes under the code-point layer and mangle the string). Use Bytes for raw bytes.
        int cp = hi * 16 + lo;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        return 4;
    }
    char d;
    switch (e) {
        case 'n': d = '\n'; break;
        case 't': d = '\t'; break;
        case 'r': d = '\r'; break;
        case '0': d = '\0'; break;
        case '\\': d = '\\'; break;
        case '"': d = '"'; break;
        case '\'': d = '\''; break;
        default: { err = std::string("invalid escape '\\") + e + "'"; return 0; }
    }
    out.push_back(d);
    return 2;
}

// Approximate float comparison: |a-b| <= max(relTol*max(|a|,|b|), absTol). NaN is never close; equal
// infinities are close. The single source of truth for the numeric `.compare()` tolerance, shared by
// the runtime's Integer/Float compare and the matrix/complex/tensor modules (defined here, in the
// first-included header, so the stdlib modules — compiled before runtime.hpp — can all reach it).
inline bool floatClose(double a, double b, double relTol, double absTol) {
    if (a == b) return true;                              // exact (covers inf == inf)
    if (std::isnan(a) || std::isnan(b) || std::isinf(a) || std::isinf(b)) return false;
    double diff = std::fabs(a - b);
    return diff <= std::max(relTol * std::max(std::fabs(a), std::fabs(b)), absTol);
}

// Upper bound on the size (in elements/bytes) of a single value built by repetition or padding, so a
// hostile or careless count (`"x" * 10**12`, `"".ljust(10**9)`, `b"x" * 10**12`) throws cleanly
// instead of OOMing the host. ~256 MB is far beyond any legitimate scripting use. Single source of
// truth for String/List/Bytes repetition, padding, and `range` — defined here so bytes.hpp (compiled
// before runtime.hpp) shares the exact same cap.
inline constexpr uint64_t kMaxRepeat = 256ull * 1024 * 1024;

}  // namespace kirito

// KiritoError and KiritoThrow — see the note above their placeholder-comment. Included at the
// end so SourceSpan / TraceFrame are already defined, and so the 40+ files that only include
// common.hpp still get both classes through the chain.
#include "exceptions.hpp"    // NOLINT(build/include_order): must follow SourceSpan / TraceFrame

#endif
