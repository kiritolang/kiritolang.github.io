#ifndef KIRITO_STDLIB_TIME_HPP
#define KIRITO_STDLIB_TIME_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <span>
#include <string>
#include <thread>

#include "builtins.hpp"
#include "collections.hpp"
#include "native.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// Portable UTC broken-down-time -> epoch seconds (POSIX timegm / Windows _mkgmtime aren't standard).
// Pure civil-date arithmetic (Howard Hinnant's days-from-civil algorithm) so it needs no globals or
// timezone state and matches gmtime_r round-trips.
inline int64_t timegmCompat(const std::tm& tm) {
    int64_t y = tm.tm_year + 1900;
    int64_t m = tm.tm_mon + 1;
    int64_t d = tm.tm_mday;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;  // days since 1970-01-01
    // hour/min/sec widened to int64 before scaling — an int `tm_hour * 3600` would overflow `int`.
    return days * 86400 + static_cast<int64_t>(tm.tm_hour) * 3600
                        + static_cast<int64_t>(tm.tm_min) * 60 + tm.tm_sec;
}

// Inverse of timegmCompat: epoch seconds -> broken-down UTC fields, via the same pure civil arithmetic
// (Hinnant's civil-from-days). Unlike the platform gmtime_r/gmtime_s — whose valid range differs across
// platforms (Windows rejects negative time_t and years beyond ~3000) — this handles the full int64
// range identically everywhere, so DateTime behaves the same on Linux and Windows.
inline void gmtimeCompat(int64_t secs, std::tm& tm) {
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }                  // floor toward -inf: rem in [0, 86399]
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                          // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;     // [0, 399]
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                   // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                                        // [0, 11]
    int64_t d = doy - (153 * mp + 2) / 5 + 1;                                // [1, 31]
    int64_t m = mp < 10 ? mp + 3 : mp - 9;                                   // [1, 12]
    y += (m <= 2);
    // Validate the TRUE (int64) year before narrowing to int. A pathological epoch yields a |year| far
    // beyond int range; `static_cast<int>(y - 1900)` would wrap and a later range check on the narrowed
    // tm_year could then wrongly ACCEPT a corrupt DateTime. DateTime's contract is year in [-9999, 9999].
    if (y < -9999 || y > 9999)
        throw KiritoError("DateTime: epoch " + std::to_string(secs) +
                          " is out of representable range (year " + std::to_string(y) + ")");
    tm = std::tm{};
    tm.tm_year = static_cast<int>(y - 1900);
    tm.tm_mon = static_cast<int>(m - 1);
    tm.tm_mday = static_cast<int>(d);
    tm.tm_hour = static_cast<int>(rem / 3600);
    tm.tm_min = static_cast<int>((rem % 3600) / 60);
    tm.tm_sec = static_cast<int>(rem % 60);
    int64_t wd = (days % 7 + 4) % 7;                         // 1970-01-01 was a Thursday (tm_wday 4)
    tm.tm_wday = static_cast<int>(wd < 0 ? wd + 7 : wd);     // 0=Sunday … 6=Saturday
    std::tm jan{};
    jan.tm_year = tm.tm_year; jan.tm_mon = 0; jan.tm_mday = 1;
    tm.tm_yday = static_cast<int>(days - timegmCompat(jan) / 86400);
}

// Days in a (1-based) month of a proleptic-Gregorian year, for field-range validation.
inline int daysInMonthUTC(int64_t year, int month) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return dim[month - 1];
}

// Minimal strptime covering the common UTC fields (%Y %m %d %H %M %S and literal separators). Avoids
// the platform strptime (absent on Windows). Returns false if the text doesn't match the format —
// which includes out-of-range fields (month 99, day 32, hour 25) and any unconverted trailing input,
// so malformed text fails to parse instead of silently normalizing to a
// garbage-but-plausible date. (Construction via `time.make` deliberately keeps C-mktime rollover.)
inline bool strptimeCompat(const char* s, const char* fmt, std::tm& tm) {
    auto num = [&](int width, int& out) -> bool {
        int v = 0, n = 0;
        while (*s >= '0' && *s <= '9' && n < width) { v = v * 10 + (*s - '0'); ++s; ++n; }
        if (n == 0) return false;
        out = v;
        return true;
    };
    tm.tm_mday = 1;  // sensible defaults so a partial format still yields a valid date
    while (*fmt) {
        if (*fmt == '%') {
            ++fmt;
            int v = 0;
            switch (*fmt) {
                case 'Y': { if (!num(4, v)) return false; tm.tm_year = v - 1900; } break;
                case 'm': { if (!num(2, v)) return false; tm.tm_mon = v - 1; } break;
                case 'd': { if (!num(2, v)) return false; tm.tm_mday = v; } break;
                case 'H': { if (!num(2, v)) return false; tm.tm_hour = v; } break;
                case 'M': { if (!num(2, v)) return false; tm.tm_min = v; } break;
                case 'S': { if (!num(2, v)) return false; tm.tm_sec = v; } break;
                case '%': { if (*s != '%') return false; ++s; } break;
                default: { return false; } break;  // unsupported directive
            }
            ++fmt;
        } else {
            if (*s != *fmt) return false;  // literal must match
            ++s; ++fmt;
        }
    }
    if (*s != '\0') return false;  // unconverted trailing input ("2024-01-01XYZ") -> no match
    int year = tm.tm_year + 1900, month = tm.tm_mon + 1;
    if (month < 1 || month > 12) return false;
    if (tm.tm_mday < 1 || tm.tm_mday > daysInMonthUTC(year, month)) return false;
    if (tm.tm_hour < 0 || tm.tm_hour > 23) return false;
    if (tm.tm_min < 0 || tm.tm_min > 59) return false;
    if (tm.tm_sec < 0 || tm.tm_sec > 59) return false;
    return true;
}

// A point in calendar time, broken into fields. Constructed from a Unix
// timestamp (seconds since the epoch, UTC). Exposes year/month/day/hour/minute/second plus
// formatting; immutable once built.
class DateTime : public NativeClass<DateTime> {
public:
    static constexpr const char* kTypeName = "DateTime";
    int64_t epoch = 0;  // whole seconds since 1970-01-01 UTC
    std::tm tm{};       // broken-down UTC fields
    bool initialized_ = false;  // true once a value is fully built; guards _setstate_ (see below)

    DateTime() { setEpoch(0); }                       // empty value for serialize/dump reconstruction
    explicit DateTime(int64_t secs) { setEpoch(secs); initialized_ = true; }

    // Set the epoch and recompute the broken-down UTC fields, keeping `epoch` and `tm` consistent.
    // Uses the portable gmtimeCompat (not the platform gmtime, whose range differs across OSes), so the
    // accepted epoch range is the same on every platform; reject epochs outside a sane year range
    // rather than emit a nonsense date.
    void setEpoch(int64_t secs) {
        epoch = secs;
        gmtimeCompat(secs, tm);
        int64_t year = static_cast<int64_t>(tm.tm_year) + 1900;
        if (year < -9999 || year > 9999)
            throw KiritoError("DateTime: epoch " + std::to_string(secs) + " is out of representable range");
    }

    std::string iso() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return buf;
    }
    std::string str(StringifyCtx&) const override { return "DateTime(" + iso() + ")"; }

    // Value semantics: two DateTimes are equal when they denote the same instant (epoch). The epoch
    // is an exact int64, so equality and hash agree — a DateTime is hashable (usable as a Dict/Set
    // key).
    bool equals(const ObjectArena&, const Object& other) const override {
        const auto* d = dynamic_cast<const DateTime*>(&other);
        return d && d->epoch == epoch;
    }
    bool hashable() const override { return true; }
    std::size_t hash() const override { return std::hash<int64_t>{}(epoch); }

    std::vector<std::string> inspectMembers() const override {
        return {"year: Integer", "month: Integer", "day: Integer", "hour: Integer",
                "minute: Integer", "second: Integer", "weekday: Integer", "yearday: Integer",
                "timestamp: Integer", "add(seconds) -> DateTime", "sub(seconds) -> DateTime",
                "diff(other) -> Integer", "iso() -> String", "isoformat() -> String", "format(fmt) -> String"};
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        if (name == "year") return vm.makeInt(tm.tm_year + 1900);
        if (name == "month") return vm.makeInt(tm.tm_mon + 1);
        if (name == "day") return vm.makeInt(tm.tm_mday);
        if (name == "hour") return vm.makeInt(tm.tm_hour);
        if (name == "minute") return vm.makeInt(tm.tm_min);
        if (name == "second") return vm.makeInt(tm.tm_sec);
        if (name == "weekday") return vm.makeInt(tm.tm_wday);   // 0 = Sunday
        if (name == "yearday") return vm.makeInt(tm.tm_yday + 1);
        if (name == "timestamp") return vm.makeInt(epoch);   // an attribute, like the other UTC fields
        // Arithmetic: add/sub a number of seconds -> a new DateTime; diff -> seconds between two.
        if (name == "add" || name == "sub")
            return makeMethod(vm,
                std::string(name), {"seconds"},
                [self, sub = (name == "sub")](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                    if (a.size() != 1) throw KiritoError("add/sub expects 1 argument (seconds)");
                    const Object& o = vm.arena().deref(a[0]);
                    int64_t delta;
                    if (o.kind() == ValueKind::Integer) delta = static_cast<const IntVal&>(o).value();
                    else if (o.kind() == ValueKind::Float) delta = toInt64Checked(static_cast<const FloatVal&>(o).value(), "add/sub");  // NaN/inf/range-safe
                    else throw KiritoError("add/sub expects a number of seconds");
                    int64_t base = static_cast<DateTime&>(vm.arena().deref(self)).epoch;
                    // Overflow-safe (no UB even for delta == INT64_MIN); a wrapped epoch would be nonsense anyway.
                    int64_t result;
                    bool overflow = sub ? __builtin_sub_overflow(base, delta, &result)
                                        : __builtin_add_overflow(base, delta, &result);
                    if (overflow) throw KiritoError("DateTime arithmetic overflow");
                    return vm.alloc(std::make_unique<DateTime>(result));
                }, std::vector<Handle>{self});
        if (name == "diff")
            return makeMethod(vm,
                "diff", {"other"},
                [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                    if (a.size() != 1) throw KiritoError("diff expects 1 argument (a DateTime)");
                    const auto* other = dynamic_cast<const DateTime*>(&vm.arena().deref(a[0]));
                    if (!other) throw KiritoError("diff expects a DateTime");
                    return vm.makeInt(static_cast<DateTime&>(vm.arena().deref(self)).epoch - other->epoch);
                }, std::vector<Handle>{self});
        if (name == "iso" || name == "isoformat")
            return makeMethod(vm,
                "iso", {},
                [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
                    return vm.makeString(static_cast<DateTime&>(vm.arena().deref(self)).iso());
                }, std::vector<Handle>{self});
        if (name == "format")
            return makeMethod(vm,
                "format", {"fmt"},
                [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                    Args(vm, a, "format").require(1);                       // guard the positional fast path (a[0] OOB otherwise)
                    auto& dt = static_cast<DateTime&>(vm.arena().deref(self));
                    const Object& o = vm.arena().deref(a[0]);
                    if (o.kind() != ValueKind::String) throw KiritoError("format expects a String");
                    const std::string& fmt = static_cast<const StrVal&>(o).value();
                    // The format string is intentionally user-supplied (that's the feature), so the
                    // non-literal-format warning is expected here; silence it locally. Grow the buffer
                    // when strftime returns 0 (didn't fit) so a long result isn't silently truncated to "".
                    std::string out;
                    if (!fmt.empty()) {
                        std::vector<char> buf(256);
                        for (int tries = 0; tries < 6; ++tries) {
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
                            std::size_t n = std::strftime(buf.data(), buf.size(), fmt.c_str(), &dt.tm);
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
                            if (n > 0) { out.assign(buf.data(), n); break; }
                            buf.resize(buf.size() * 4);
                        }
                    }
                    return vm.makeString(out);
                }, std::vector<Handle>{self});
        // --- serialization (serialize / dump): a DateTime is fully determined by its epoch. ---
        if (name == "_getstate_")
            return makeMethod(vm, "_getstate_", {},
                [self](KiritoVM& vm, std::span<const Handle>) -> Handle {
                    return vm.makeInt(static_cast<DateTime&>(vm.arena().deref(self)).epoch);
                }, std::vector<Handle>{self});
        if (name == "_setstate_")
            return makeMethod(vm, "_setstate_", {"state"},
                [self](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                    Args(vm, a, "_setstate_").require(1);                  // guard the positional fast path (a[0] OOB otherwise)
                    auto& d = static_cast<DateTime&>(vm.arena().deref(self));
                    // DateTime is immutable + hashable (a Dict/Set key). _setstate_ exists ONLY for the
                    // deserializer's alloc(empty) -> _setstate_(epoch) path; re-homing an established value
                    // in place would corrupt any container keyed on it. So it is one-shot: refuse once built.
                    if (d.initialized_)
                        throw KiritoError("DateTime _setstate_: cannot re-initialize an established DateTime "
                                          "(it is immutable once built)");
                    const Object& o = vm.arena().deref(a[0]);
                    if (o.kind() != ValueKind::Integer)
                        throw KiritoError("DateTime _setstate_: expected an Integer epoch");
                    d.setEpoch(static_cast<const IntVal&>(o).value());
                    d.initialized_ = true;
                    return vm.none();
                }, std::vector<Handle>{self});
        return Object::getAttr(vm, self, name);
    }
};

// The `time` module: high-precision clocks and a small datetime facility.
class TimeModule : public NativeModule {
public:
    std::string name() const override { return "time"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
        using namespace std::chrono;

        // Let serialize/dump reconstruct a DateTime: build an empty one; _setstate_ fills it in.
        vm.registerDeserializer("DateTime", [](KiritoVM& v, Handle) -> Handle {
            return v.alloc(std::make_unique<DateTime>());
        });

        // time() -> Float seconds since the Unix epoch (wall clock).
        m.fn("time", {}, "Float", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return Value(vm, duration<double>(system_clock::now().time_since_epoch()).count());
        });

        // timens() -> Integer nanoseconds since the epoch (wall clock).
        m.fn("timens", {}, "Integer", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return Value(vm, static_cast<int64_t>(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count()));
        });

        // monotonic() -> Float seconds from a steady clock (for measuring intervals).
        m.fn("monotonic", {}, "Float", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return Value(vm, duration<double>(steady_clock::now().time_since_epoch()).count());
        });

        // perfcounterns() -> Integer nanoseconds from the highest-resolution steady clock.
        m.fn("perfcounterns", {}, "Integer", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return Value(vm, static_cast<int64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()));
        });

        // sleep(seconds) — Float or Integer seconds.
        m.fn("sleep", {{"seconds", "Number"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            double secs = Args(vm, a, "sleep")[0].asFloat("sleep");
            // Guard the one float entry point that reaches an int64 duration_cast: a non-finite or
            // absurd value would overflow it (UB — `sleep(inf)` returned immediately) or request a
            // multi-billion-year sleep (`sleep(1e18)` hung). Reject both, like the rest of the
            // numeric stack rejects out-of-range float->int conversions.
            if (!std::isfinite(secs)) throw KiritoError("sleep: seconds must be a finite number");
            if (secs > 1e9) throw KiritoError("sleep: seconds too large (maximum 1e9)");
            if (secs > 0) std::this_thread::sleep_for(duration<double>(secs));
            return Value::None(vm);
        });

        // datetime(timestamp) -> DateTime; with no arg (or None), the current UTC time.
        m.fn("datetime", {{"timestamp", "", vm.none()}}, "DateTime", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "datetime");
            int64_t secs;
            if (args.empty() || args[0].isNone())
                secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
            else if (args[0].isInt())
                secs = args[0].asInt("datetime timestamp");   // exact: an Integer epoch must not lose precision via double
            else {
                // Casting a non-finite or out-of-range double to int64 is UB (and GCC's -fsanitize=undefined
                // does not flag float-cast-overflow), so guard before the cast — like math.floor/ceil.
                double d = args[0].asFloat("datetime timestamp");
                if (std::isnan(d)) throw KiritoError("datetime: cannot convert NaN to a timestamp");
                if (std::isinf(d)) throw KiritoError("datetime: cannot convert infinity to a timestamp");
                if (d >= 9223372036854775808.0 || d < -9223372036854775808.0)
                    throw KiritoError("datetime: timestamp out of representable range");
                secs = static_cast<int64_t>(d);
            }
            return vm.alloc(std::make_unique<DateTime>(secs));
        });

        // now() -> DateTime for the current UTC time (convenience).
        m.fn("now", {}, "DateTime", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            int64_t secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
            return vm.alloc(std::make_unique<DateTime>(secs));
        });

        // make(year, month, day[, hour, minute, second]) -> DateTime built from UTC components.
        m.fn("make", {{"year", "Integer"}, {"month", "Integer"}, {"day", "Integer"}, {"hour", "Integer", vm.makeInt(0)}, {"minute", "Integer", vm.makeInt(0)}, {"second", "Integer", vm.makeInt(0)}}, "DateTime", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "make");
            // Bound each component well within `int` (margin for the `-1900`/`-1` below and the int
            // tm fields) — else the component arithmetic is signed-overflow UB. A huge in-band value
            // still produces an out-of-epoch-range DateTime, which the constructor rejects cleanly.
            auto iv = [&](std::size_t i, int dflt) -> int {
                if (i >= args.size()) return dflt;
                int64_t v = args[i].asInt("make component");
                if (v < -2000000000 || v > 2000000000) throw KiritoError("make: date component out of range");
                return static_cast<int>(v);
            };
            std::tm tm{};
            tm.tm_year = iv(0, 1970) - 1900;
            tm.tm_mon = iv(1, 1) - 1;
            tm.tm_mday = iv(2, 1);
            tm.tm_hour = iv(3, 0);
            tm.tm_min = iv(4, 0);
            tm.tm_sec = iv(5, 0);
            return vm.alloc(std::make_unique<DateTime>(timegmCompat(tm)));
        });

        // strptime(text, format) -> DateTime, parsing UTC fields with the given strftime format.
        m.fn("strptime", {{"text", "String"}, {"format", "String"}}, "DateTime", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "strptime");
            std::tm tm{};
            if (!strptimeCompat(args[0].asStringRef("strptime text").c_str(),
                                args[1].asStringRef("strptime format").c_str(), tm))
                throw KiritoError("strptime: text does not match format");
            return vm.alloc(std::make_unique<DateTime>(timegmCompat(tm)));
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
