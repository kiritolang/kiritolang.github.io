#ifndef KIRITO_STDLIB_REGEX_HPP
#define KIRITO_STDLIB_REGEX_HPP

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "native.hpp"
#include "regex_engine.hpp"

namespace kirito {

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// The `regex` module: a full-featured regular-expression library backed by reng (regex_engine.hpp),
// whose Thompson-NFA / Pike-VM core guarantees LINEAR-TIME matching — no catastrophic backtracking.
// The API: compile() yields a reusable Regex; module-level match/search/...
// compile on the fly. Positions and spans are code-point indices, matching Kirito's String model.

// --- a small helper: slice a UTF-8 string by code-point indices ------------------------------------
inline std::string cpSlice(const std::string& s, const std::vector<std::size_t>& starts, int a, int b) {
    if (a < 0) a = 0;
    int n = static_cast<int>(starts.size());
    if (b > n) b = n;
    if (a >= b) return "";
    std::size_t from = starts[static_cast<std::size_t>(a)];
    std::size_t to = (b < n) ? starts[static_cast<std::size_t>(b)] : s.size();
    return s.substr(from, to - from);
}

// ============================================================================ Match object
class MatchVal : public NativeClass<MatchVal> {
public:
    static constexpr const char* kTypeName = "Match";
    std::vector<std::string> inspectMembers() const override {
        return {"string: String", "group(index) -> String", "groups(default) -> List", "groupdict(default) -> Dict", "start(group) -> Integer", "end(group) -> Integer", "span(group) -> List"};
    }
    Handle subject;                                  // the String matched against (kept alive via children)
    std::vector<int> slots;                          // 2*(numGroups+1) code-point indices, -1 = absent
    int numGroups;
    std::vector<std::string> names;                  // group index -> name
    fum::unordered_map<std::string, int> nameToGroup;

    MatchVal(Handle subj, std::vector<int> sl, int ng, std::vector<std::string> nm,
             fum::unordered_map<std::string, int> n2g)
        : subject(subj), slots(std::move(sl)), numGroups(ng), names(std::move(nm)), nameToGroup(std::move(n2g)) {}

    void children(std::vector<Handle>& out) const override { out.push_back(subject); }
    std::string str(StringifyCtx&) const override {
        return "<Match span=(" + std::to_string(slots[0]) + ", " + std::to_string(slots[1]) + ")>";
    }

    // Resolve a group key (Integer index or String name) to a group number; throws on a bad key.
    int groupOf(KiritoVM& vm, Handle key) const {
        const Object& o = vm.arena().deref(key);
        if (o.kind() == ValueKind::Integer) {
            int g = static_cast<int>(static_cast<const IntVal&>(o).value());
            if (g < 0 || g > numGroups) throw KiritoError("no such group: " + std::to_string(g));
            return g;
        }
        if (o.kind() == ValueKind::String) {
            auto it = nameToGroup.find(static_cast<const StrVal&>(o).value());
            if (it == nameToGroup.end()) throw KiritoError("no such group: '" + static_cast<const StrVal&>(o).value() + "'");
            return it->second;
        }
        throw KiritoError("group key must be an Integer index or a String name");
    }

    Handle groupString(KiritoVM& vm, int g) const {
        if (slots[2 * g] < 0) return vm.none();       // group didn't participate
        const auto& sv = static_cast<const StrVal&>(vm.arena().deref(subject));
        return vm.makeString(cpSlice(sv.value(), utf8Starts(sv.value()), slots[2 * g], slots[2 * g + 1]));
    }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto me = [](KiritoVM& vm, Handle self) -> MatchVal& { return static_cast<MatchVal&>(vm.arena().deref(self)); };

        if (name == "string") return subject;          // attribute: the original subject String
        if (name == "group")
            return bind("group", {"index"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                MatchVal& M = me(vm, self);
                if (a.empty()) return M.groupString(vm, 0);
                if (a.size() == 1) return M.groupString(vm, M.groupOf(vm, a[0]));
                // several keys -> a List of the requested groups
                List out(vm);
                for (Handle k : a) out.push(M.groupString(vm, M.groupOf(vm, k)));
                return out.handle();
            });
        if (name == "groups")
            return bind("groups", {"default"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                MatchVal& M = me(vm, self);
                Handle dflt = a.empty() ? vm.none() : a[0];
                List out(vm);
                for (int g = 1; g <= M.numGroups; ++g)
                    out.push(M.slots[2 * g] < 0 ? dflt : M.groupString(vm, g));
                return out.handle();
            });
        if (name == "groupdict")
            return bind("groupdict", {"default"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                MatchVal& M = me(vm, self);
                Handle dflt = a.empty() ? vm.none() : a[0];
                Dict out(vm);
                for (int g = 1; g <= M.numGroups; ++g)
                    if (!M.names[g].empty())
                        out.set(M.names[g], M.slots[2 * g] < 0 ? dflt : M.groupString(vm, g));
                return out.handle();
            });
        if (name == "start")
            return bind("start", {"group"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                MatchVal& M = me(vm, self);
                int g = a.empty() ? 0 : M.groupOf(vm, a[0]);
                return vm.makeInt(M.slots[2 * g]);
            });
        if (name == "end")
            return bind("end", {"group"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                MatchVal& M = me(vm, self);
                int g = a.empty() ? 0 : M.groupOf(vm, a[0]);
                return vm.makeInt(M.slots[2 * g + 1]);
            });
        if (name == "span")
            return bind("span", {"group"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                MatchVal& M = me(vm, self);
                int g = a.empty() ? 0 : M.groupOf(vm, a[0]);
                List out(vm); out.push(vm.makeInt(M.slots[2 * g])).push(vm.makeInt(M.slots[2 * g + 1]));
                return out.handle();
            });
        return Object::getAttr(vm, self, name);
    }
};

// ============================================================================ helpers
namespace redetail {

inline Handle makeMatch(KiritoVM& vm, Handle subject, const reng::MatchResult& r, const reng::Program& p) {
    return vm.alloc(std::make_unique<MatchVal>(subject, r.slots, p.numGroups, p.groupNames, p.nameToGroup));
}

// Successive non-overlapping matches over the whole text (incl. the
// empty-match advance). Returns the raw results so callers can build Matches/strings/splits.
inline std::vector<reng::MatchResult> allMatches(const reng::Program& p, const std::vector<int32_t>& text,
                                                 int maxCount, int startPos = 0) {
    std::vector<reng::MatchResult> out;
    int n = static_cast<int>(text.size());
    int pos = startPos < 0 ? 0 : startPos;
    bool mustAdvance = false;   // after an empty match, the next search must not accept an empty match
    while (pos <= n) {          // at the SAME position — Python's must_advance, so a higher-priority
        if (maxCount >= 0 && static_cast<int>(out.size()) >= maxCount) break;   // empty alternative
        reng::MatchResult r = reng::run(p, text, pos, /*anchored=*/false, /*requireEnd=*/false, mustAdvance);
        if (!r.matched) break;   // doesn't mask a non-empty match (`|\w`, `a??`, `a||b`, …).
        int a = r.slots[0], b = r.slots[1];
        out.push_back(std::move(r));
        pos = b;                 // advance to the match end; if it was empty (b==a) pos stays and the
        mustAdvance = (a == b);  // next search must advance — else run returns no match and we stop.
    }
    return out;
}

// Expand a replacement template: \1..\99, \g<n>, \g<name>, \g<0>/\0 (whole match), \\ and \n \t \r.
inline std::string expandTemplate(KiritoVM& vm, const std::string& tmpl, const MatchVal& m,
                                  const std::string& subj, const std::vector<std::size_t>& starts) {
    auto groupText = [&](int g) -> std::string {
        // An out-of-range group number is a template error (an invalid group reference),
        // not a silent empty string; a group that exists but did not participate expands to "".
        if (g < 0 || g > m.numGroups)
            throw KiritoError("invalid group reference " + std::to_string(g) + " in replacement template");
        if (m.slots[2 * g] < 0) return "";
        return cpSlice(subj, starts, m.slots[2 * g], m.slots[2 * g + 1]);
    };
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
        char c = tmpl[i];
        if (c != '\\') { out += c; ++i; continue; }
        if (i + 1 >= tmpl.size()) throw KiritoError("bad replacement: trailing backslash");
        char d = tmpl[i + 1];
        if (d >= '0' && d <= '9') {                       // \1, \12 (up to two digits)
            int g = d - '0'; i += 2;
            if (i < tmpl.size() && tmpl[i] >= '0' && tmpl[i] <= '9') { g = g * 10 + (tmpl[i] - '0'); ++i; }
            out += groupText(g);
        } else if (d == 'g') {                            // \g<name> or \g<number>
            std::size_t j = i + 2;
            if (j >= tmpl.size() || tmpl[j] != '<') throw KiritoError("bad replacement: expected '<' after \\g");
            ++j;
            std::string key;
            while (j < tmpl.size() && tmpl[j] != '>') key += tmpl[j++];
            if (j >= tmpl.size()) throw KiritoError("bad replacement: unterminated \\g<...>");
            i = j + 1;
            bool numeric = !key.empty();
            for (char k : key) if (k < '0' || k > '9') numeric = false;
            int g;
            if (numeric) {
                try { g = std::stoi(key); }                   // a huge \g<NNN> would leak an opaque "stoi"
                catch (const std::exception&) { throw KiritoError("bad replacement: invalid group reference '" + key + "'"); }
            } else {
                auto it = m.nameToGroup.find(key);
                if (it == m.nameToGroup.end()) throw KiritoError("bad replacement: no group named '" + key + "'");
                g = it->second;
            }
            out += groupText(g);
        } else if (d == 'n') { out += '\n'; i += 2; }
        else if (d == 't') { out += '\t'; i += 2; }
        else if (d == 'r') { out += '\r'; i += 2; }
        else if (d == '\\') { out += '\\'; i += 2; }
        else { out += d; i += 2; }                        // \. etc. -> the literal char
    }
    (void)vm;
    return out;
}

// Resolve the optional pos / endpos slots (arg indices 1 and 2 of match/search/finditer/findall).
// Two hazards handled here: (a) makeMethod fills a SKIPPED leading optional with None when a LATER
// arg is passed by keyword (e.g. `re.match(string=s, endpos=2)` leaves pos = None) — a None slot means
// "use the default", not a type error; (b) the int64 value is clamped to [0, len] BEFORE narrowing to
// int, so a huge pos/endpos can't misbehave. `endpos` truncates `text` in place (so `$`/`\b` anchor
// there); the returned start position is clamped to the post-truncation length.
inline int resolvePosEndpos(KiritoVM& vm, Args& args, std::span<const Handle> a,
                            std::vector<int32_t>& text) {
    auto slotVal = [&](std::size_t i, const char* nm) -> std::optional<int64_t> {
        if (i >= a.size() || vm.arena().deref(a[i]).kind() == ValueKind::None) return std::nullopt;
        return args[i].asInt(nm);
    };
    int64_t n = static_cast<int64_t>(text.size());
    if (auto e = slotVal(2, "endpos")) {
        int64_t endpos = std::clamp<int64_t>(*e, 0, n);
        if (endpos < n) { text.resize(static_cast<std::size_t>(endpos)); n = endpos; }
    }
    int64_t pos = 0;
    if (auto p = slotVal(1, "pos")) pos = std::clamp<int64_t>(*p, 0, n);
    return static_cast<int>(pos);
}

}  // namespace redetail

// ============================================================================ Regex object
class RegexVal : public NativeClass<RegexVal> {
public:
    static constexpr const char* kTypeName = "Regex";
    std::vector<std::string> inspectMembers() const override {
        return {"pattern: String", "groups: Integer", "groupindex: Dict",
                "match(string, pos, endpos) -> Match", "search(string, pos, endpos) -> Match", "fullmatch(string, pos, endpos) -> Match", "findall(string, pos, endpos) -> List", "finditer(string, pos, endpos) -> List", "sub(repl, string, count) -> String", "split(string, maxsplit) -> List"};
    }
    reng::Program prog;
    std::string pattern;

    RegexVal(reng::Program p, std::string pat) : prog(std::move(p)), pattern(std::move(pat)) {}

    std::string str(StringifyCtx&) const override { return "<Regex /" + pattern + "/>"; }

    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto re = [](KiritoVM& vm, Handle self) -> RegexVal& { return static_cast<RegexVal&>(vm.arena().deref(self)); };

        if (name == "pattern") return vm.makeString(pattern);
        if (name == "groups") return vm.makeInt(prog.numGroups);
        if (name == "groupindex") {
            Dict d(vm);
            for (const auto& [nm, g] : prog.nameToGroup) d.set(nm, vm.makeInt(g));
            return d.handle();
        }
        if (name == "match" || name == "search" || name == "fullmatch")
            return bind(std::string(name).c_str(), {"string", "pos", "endpos"}, [self, re, name = std::string(name)](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                RegexVal& R = re(vm, self);
                Args args(vm, a, name.c_str());
                std::string s = args.at(0).asStringRef(name.c_str());
                auto text = reng::toCodepoints(s);
                int pos = redetail::resolvePosEndpos(vm, args, a, text);  // None-safe, clamped, applies endpos
                bool anchored = (name != "search");
                bool requireEnd = (name == "fullmatch");
                reng::MatchResult r = reng::run(R.prog, text, pos, anchored, requireEnd);
                if (!r.matched) return vm.none();
                return redetail::makeMatch(vm, args[0].handle(), r, R.prog);
            });
        if (name == "finditer")
            return bind("finditer", {"string", "pos", "endpos"}, [self, re](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                RegexVal& R = re(vm, self);
                Args args(vm, a, "finditer");
                std::string s = args.at(0).asStringRef("finditer");
                auto text = reng::toCodepoints(s);
                int pos = redetail::resolvePosEndpos(vm, args, a, text);
                RootScope rs(vm);
                List out(vm);
                for (auto& r : redetail::allMatches(R.prog, text, -1, pos))
                    out.push(rs.add(redetail::makeMatch(vm, args[0].handle(), r, R.prog)));
                return out.handle();
            });
        if (name == "findall")
            return bind("findall", {"string", "pos", "endpos"}, [self, re](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                RegexVal& R = re(vm, self);
                Args args(vm, a, "findall");
                std::string s = args.at(0).asStringRef("findall");
                auto text = reng::toCodepoints(s);
                auto starts = utf8Starts(s);
                int pos = redetail::resolvePosEndpos(vm, args, a, text);
                List out(vm);
                for (auto& r : redetail::allMatches(R.prog, text, -1, pos)) {
                    auto gtext = [&](int g) { return r.slots[2 * g] < 0 ? std::string("")
                                              : cpSlice(s, starts, r.slots[2 * g], r.slots[2 * g + 1]); };
                    if (R.prog.numGroups == 0) {
                        out.push(vm.makeString(cpSlice(s, starts, r.slots[0], r.slots[1])));
                    } else if (R.prog.numGroups == 1) {
                        out.push(vm.makeString(gtext(1)));
                    } else {
                        List tup(vm);
                        for (int g = 1; g <= R.prog.numGroups; ++g) tup.push(vm.makeString(gtext(g)));
                        out.push(tup.handle());
                    }
                }
                return out.handle();
            });
        if (name == "sub")
            return bind("sub", {"repl", "string", "count"}, [self, re](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                RegexVal& R = re(vm, self);
                Args args(vm, a, "sub");
                Handle repl = args.at(0).handle();
                std::string s = args.at(1).asStringRef("sub string");
                int count = (a.size() > 2) ? static_cast<int>(args[2].asInt("count")) : 0;
                auto text = reng::toCodepoints(s);
                auto starts = utf8Starts(s);
                const Object& ro = vm.arena().deref(repl);
                bool callable = ro.kind() == ValueKind::Function || ro.kind() == ValueKind::NativeFunction
                                || ro.kind() == ValueKind::Instance;
                bool isStr = ro.kind() == ValueKind::String;
                if (!callable && !isStr) throw KiritoError("sub replacement must be a String or a function");
                RootScope rs(vm);
                std::string out;
                int lastEnd = 0;
                for (auto& r : redetail::allMatches(R.prog, text, count > 0 ? count : -1)) {
                    int aPos = r.slots[0], bPos = r.slots[1];
                    out += cpSlice(s, starts, lastEnd, aPos);
                    if (isStr) {
                        MatchVal mv(args[1].handle(), r.slots, R.prog.numGroups, R.prog.groupNames, R.prog.nameToGroup);
                        out += redetail::expandTemplate(vm, static_cast<const StrVal&>(ro).value(), mv, s, starts);
                    } else {
                        Handle mh = rs.add(redetail::makeMatch(vm, args[1].handle(), r, R.prog));
                        std::array<Handle, 1> ca{mh};
                        Handle res = vm.arena().deref(repl).call(vm, ca);
                        const Object& reso = vm.arena().deref(res);
                        if (reso.kind() != ValueKind::String) throw KiritoError("sub replacement function must return a String");
                        out += static_cast<const StrVal&>(reso).value();
                    }
                    lastEnd = bPos;
                }
                out += cpSlice(s, starts, lastEnd, static_cast<int>(text.size()));
                return vm.makeString(out);
            });
        if (name == "split")
            return bind("split", {"string", "maxsplit"}, [self, re](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                RegexVal& R = re(vm, self);
                Args args(vm, a, "split");
                std::string s = args.at(0).asStringRef("split");
                int maxsplit = (a.size() > 1) ? static_cast<int>(args[1].asInt("maxsplit")) : 0;
                auto text = reng::toCodepoints(s);
                auto starts = utf8Starts(s);
                List out(vm);
                int lastEnd = 0, splits = 0;
                for (auto& r : redetail::allMatches(R.prog, text, -1)) {
                    if (maxsplit > 0 && splits >= maxsplit) break;
                    int aPos = r.slots[0], bPos = r.slots[1];
                    // Split on empty matches too, so an empty-capable pattern yields the
                    // leading/inter-character ''s — consistent with findall over the same matches.
                    out.push(vm.makeString(cpSlice(s, starts, lastEnd, aPos)));
                    for (int g = 1; g <= R.prog.numGroups; ++g)        // include captured groups
                        out.push(r.slots[2 * g] < 0 ? vm.none()
                                : vm.makeString(cpSlice(s, starts, r.slots[2 * g], r.slots[2 * g + 1])));
                    lastEnd = bPos; ++splits;
                }
                out.push(vm.makeString(cpSlice(s, starts, lastEnd, static_cast<int>(text.size()))));
                return out.handle();
            });
        return Object::getAttr(vm, self, name);
    }
};

// ============================================================================ the module
class RegexModule : public NativeModule {
public:
    std::string name() const override { return "regex"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();

        // flag constants (and short aliases)
        m.value("IGNORECASE", vm.makeInt(reng::IGNORECASE));
        m.value("MULTILINE", vm.makeInt(reng::MULTILINE));
        m.value("DOTALL", vm.makeInt(reng::DOTALL));
        m.value("I", vm.makeInt(reng::IGNORECASE));
        m.value("M", vm.makeInt(reng::MULTILINE));
        m.value("S", vm.makeInt(reng::DOTALL));

        // compile(pattern[, flags]) -> Regex  (compile once, reuse many times)
        m.fn("compile", {{"pattern", "String"}, {"flags", "Integer", vm.makeInt(0)}}, "Regex",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "compile");
            return compileRegex(vm, args.at(0).asStringRef("pattern"),
                                a.size() > 1 ? static_cast<int>(args[1].asInt("flags")) : 0);
        });

        // escape(s) -> a String safe to drop into a pattern as a literal
        m.fn("escape", {{"s", "String"}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string s = Args(vm, a, "escape").at(0).asStringRef("escape");
            std::string out;
            for (std::size_t st : utf8Starts(s)) {
                unsigned cp = utf8DecodeAt(s, st);
                bool wordish = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
                               || (cp >= '0' && cp <= '9') || cp == '_' || cp > 127;
                if (!wordish) out += '\\';
                utf8Encode(cp, out);
            }
            return vm.makeString(out);
        });

        // One-shot module functions: compile internally, then delegate to the Regex method.
        m.fn("match", {{"pattern", "String"}, {"string", "String"}, {"flags", "Integer", vm.makeInt(0)}}, "",
             [](KiritoVM& vm, std::span<const Handle> a) { return oneShot(vm, a, "match"); });
        m.fn("search", {{"pattern", "String"}, {"string", "String"}, {"flags", "Integer", vm.makeInt(0)}}, "",
             [](KiritoVM& vm, std::span<const Handle> a) { return oneShot(vm, a, "search"); });
        m.fn("fullmatch", {{"pattern", "String"}, {"string", "String"}, {"flags", "Integer", vm.makeInt(0)}}, "",
             [](KiritoVM& vm, std::span<const Handle> a) { return oneShot(vm, a, "fullmatch"); });
        m.fn("findall", {{"pattern", "String"}, {"string", "String"}, {"flags", "Integer", vm.makeInt(0)}}, "List",
             [](KiritoVM& vm, std::span<const Handle> a) { return oneShot(vm, a, "findall"); });
        m.fn("finditer", {{"pattern", "String"}, {"string", "String"}, {"flags", "Integer", vm.makeInt(0)}}, "List",
             [](KiritoVM& vm, std::span<const Handle> a) { return oneShot(vm, a, "finditer"); });
        m.fn("split", {{"pattern", "String"}, {"string", "String"}, {"maxsplit", "Integer", vm.makeInt(0)},
                       {"flags", "Integer", vm.makeInt(0)}}, "List",
             [](KiritoVM& vm, std::span<const Handle> a) { return oneShotExtra(vm, a, "split"); });
        m.fn("sub", {{"pattern", "String"}, {"repl"}, {"string", "String"}, {"count", "Integer", vm.makeInt(0)},
                     {"flags", "Integer", vm.makeInt(0)}}, "String",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "sub");
            // Thread the optional `flags` (5th arg) into compilation, like Python's re.sub(..., flags=).
            Handle rx = compileRegex(vm, args.at(0).asStringRef("pattern"),
                                     a.size() > 4 ? static_cast<int>(args[4].asInt("flags")) : 0);
            RootScope rs(vm); rs.add(rx);
            Handle method = vm.arena().deref(rx).getAttr(vm, rx, "sub");
            std::array<Handle, 3> ma{args.at(1).handle(), args.at(2).handle(),
                                     a.size() > 3 ? args[3].handle() : vm.makeInt(0)};
            return vm.arena().deref(method).call(vm, ma);
        });
    }

private:
    static Handle compileRegex(KiritoVM& vm, const std::string& pattern, int flags) {
        try {
            return vm.alloc(std::make_unique<RegexVal>(reng::compile(pattern, flags), pattern));
        } catch (const reng::RegexError& e) {
            throw KiritoError(std::string("invalid regex: ") + e.what());
        }
    }
    // match/search/fullmatch/findall/finditer: (pattern, string[, flags]) -> delegate to the method.
    static Handle oneShot(KiritoVM& vm, std::span<const Handle> a, const char* method) {
        Args args(vm, a, method);
        Handle rx = compileRegex(vm, args.at(0).asStringRef("pattern"),
                                 a.size() > 2 ? static_cast<int>(args[2].asInt("flags")) : 0);
        RootScope rs(vm); rs.add(rx);
        Handle fn = vm.arena().deref(rx).getAttr(vm, rx, method);
        std::array<Handle, 1> ma{args.at(1).handle()};
        return vm.arena().deref(fn).call(vm, ma);
    }
    // split: (pattern, string[, maxsplit[, flags]]) — the 3rd arg is maxsplit, the 4th is flags.
    static Handle oneShotExtra(KiritoVM& vm, std::span<const Handle> a, const char* method) {
        Args args(vm, a, method);
        Handle rx = compileRegex(vm, args.at(0).asStringRef("pattern"),
                                 a.size() > 3 ? static_cast<int>(args[3].asInt("flags")) : 0);
        RootScope rs(vm); rs.add(rx);
        Handle fn = vm.arena().deref(rx).getAttr(vm, rx, method);
        std::array<Handle, 2> ma{args.at(1).handle(), a.size() > 2 ? args[2].handle() : vm.makeInt(0)};
        return vm.arena().deref(fn).call(vm, ma);
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
