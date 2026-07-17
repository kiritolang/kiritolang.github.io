#ifndef KIRITO_EXCEPTIONS_HPP
#define KIRITO_EXCEPTIONS_HPP

// The two exception types the interpreter uses share a single std::exception base so any C++
// embedder can catch either uniformly:
//
//     KiritoThrow                          (derives from std::exception)
//         │
//         └── KiritoError                  (adds a std::string message)
//
// KiritoThrow is the raw Kirito `throw <value>` — it carries a Handle for the value the user
// threw, so Kirito's typed catch (`catch String as e:`, `catch MyClass as e:`) can route on the
// value's actual type. KiritoError is what the C++ side of the interpreter emits for its own
// diagnostics (parse errors, type mismatches, div-by-zero, etc.) — it carries a std::string
// message and no live value handle; the Kirito `try/catch` machinery promotes it to a Kirito
// String at the catch site (see bytecode_vm.hpp).
//
// Because KiritoError IS-A KiritoThrow, catch order matters where the two are distinguished:
// put the more-derived `catch (KiritoError&)` first, then `catch (KiritoThrow&)`.

#include "common.hpp"    // SourceSpan, TraceFrame
#include "handle.hpp"    // Handle

#include <exception>
#include <string>
#include <vector>

namespace kirito {

class KiritoThrow : public std::exception {
public:
    // Kirito `throw <value>` ctor: carries the raw value handle across the C++ stack unwind.
    KiritoThrow(Handle v, SourceSpan s = {}) : value(v), span(s) {}

    Handle value{};                       // Handle{} on a KiritoError-derived (no live value)
    SourceSpan span{};                    // throw / assert site (file:line:col)
    std::string file;                     // defining chunk of the function that threw ("" = entry)
    std::vector<TraceFrame> traceback;    // call chain — filled as the throw unwinds VM frames
    int depth = -1;                       // VM call depth at the throw site (set by Op::Throw/Reraise);
                                          // -1 for a C++ KiritoError. Used for strict PEP-479 attribution
                                          // of a generator's own StopIteration vs one leaking from deeper.

    // No VM here, so we cannot stringify `value`. Return a stable generic label; a runSource-style
    // wrapper that catches an escaping KiritoThrow typically re-throws it as a KiritoError with
    // a "uncaught exception: <str(value)>" message so end-user-facing messages are readable.
    const char* what() const noexcept override { return "Kirito value thrown"; }

protected:
    // For derived classes that carry no live value handle.
    KiritoThrow() = default;
};

// KiritoError — a C++/interpreter-side diagnostic. Not tied to a live Kirito value, but Kirito's
// try/catch still catches it (the runtime promotes .what() into a Kirito String value at the
// catch site). Ordering: this derives from KiritoThrow, so `catch (KiritoError&)` MUST come
// before `catch (KiritoThrow&)` at any site that treats them differently.
class KiritoError : public KiritoThrow {
public:
    explicit KiritoError(std::string message, SourceSpan sp = {})
        : KiritoThrow(), message_(std::move(message)) { span = sp; }

    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

}  // namespace kirito

#endif
