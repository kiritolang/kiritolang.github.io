// embed_lint.cpp — a small config-file linter whose RULES are Kirito. C++ parses a bracket-INI
// style config into a two-level Dict {section: {key: String}}, then walks a list of Kirito
// linter rules to gather findings. Each rule is a Kirito Function(cfg: Dict) -> List of
// {severity, path, msg}; the C++ engine aggregates + counts by severity + verifies.

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Finding {
    std::string severity;   // "error" | "warning" | "info"
    std::string path;       // "section.key"
    std::string msg;
};

// Parse a bracket-INI style file:
//   # comment
//   [section]
//   key = value
static Handle parseIni(KiritoVM& vm, const std::string& src) {
    // Root Dict allocated via arena so we can mutate its DictVal in place across many lines
    // without dragging non-movable RAII builders into a container. Section Dicts are done the
    // same way: a fresh DictVal per section, referenced from the root and later mutated in place.
    RootScope rs(vm);
    Handle rootH = rs.add(vm.alloc(std::make_unique<DictVal>()));
    Handle curSectionH = Handle{};
    bool haveSection = false;

    auto putSectionInRoot = [&](const std::string& name) {
        Handle newSec = rs.add(vm.alloc(std::make_unique<DictVal>()));
        Handle keyH = rs.add(vm.makeString(name));
        static_cast<DictVal&>(vm.arena().deref(rootH)).set(vm.arena(), keyH, newSec);
        curSectionH = newSec;
        haveSection = true;
    };

    std::size_t i = 0;
    while (i < src.size()) {
        std::size_t j = i;
        while (j < src.size() && src[j] != '\n') ++j;
        std::string line = src.substr(i, j - i);
        i = j + 1;
        std::size_t a = 0, b = line.size();
        while (a < b && std::isspace(static_cast<unsigned char>(line[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(line[b - 1]))) --b;
        line = line.substr(a, b - a);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            putSectionInRoot(line.substr(1, line.size() - 2));
        } else {
            auto eq = line.find('=');
            if (eq == std::string::npos)
                throw KiritoError("lint: bad line '" + line + "' (expected key=value)");
            if (!haveSection)
                throw KiritoError("lint: key/value outside of a section");
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
            while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
            Handle keyH = rs.add(vm.makeString(k));
            Handle valH = rs.add(vm.makeString(v));
            static_cast<DictVal&>(vm.arena().deref(curSectionH)).set(vm.arena(), keyH, valH);
        }
    }
    return rootH;
}

static std::vector<Finding> lint(KiritoVM& vm, const std::vector<Handle>& rules, Handle cfgH) {
    std::vector<Finding> out;
    RootScope rs(vm);
    Handle cfgR = rs.add(cfgH);
    for (Handle rH : rules) {
        std::array<Handle, 1> args{cfgR};
        Handle resH = rs.add(vm.arena().deref(rH).call(vm, args));
        Value res(vm, resH);
        if (!res.isList())
            throw KiritoError("lint: rule must return a List, got '" + res.typeName() + "'");
        for (Value f : res.items()) {
            out.push_back({ f.get("severity").asStringRef("severity"),
                            f.get("path").asStringRef("path"),
                            f.get("msg").asStringRef("msg") });
        }
    }
    return out;
}

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // Rule 1: [server] section must exist and have a port; port must parse to 1..65535.
    Handle r_port = compile(R"KI(
Function(cfg) -> List:
    var out = []
    if "server" not in cfg:
        out.append({"severity": "error", "path": "server", "msg": "missing [server] section"})
        return out
    var srv = cfg["server"]
    if "port" not in srv:
        out.append({"severity": "error", "path": "server.port", "msg": "missing port"})
        return out
    try:
        var p = Integer(srv["port"])
        if p < 1 or p > 65535:
            out.append({"severity": "error", "path": "server.port", "msg": "out of range: " + srv["port"]})
    catch String as e:
        out.append({"severity": "error", "path": "server.port", "msg": "not an integer: " + srv["port"]})
    return out
)KI");

    // Rule 2: warn if [logging] level is unknown; error if [logging] is missing entirely.
    Handle r_log = compile(R"KI(
Function(cfg) -> List:
    var out = []
    if "logging" not in cfg:
        out.append({"severity": "error", "path": "logging", "msg": "missing [logging] section"})
        return out
    var known = {"trace": True, "debug": True, "info": True, "warning": True, "error": True}
    if "level" in cfg["logging"]:
        var lvl = cfg["logging"]["level"]
        if lvl not in known:
            out.append({"severity": "warning", "path": "logging.level", "msg": "unknown level: " + lvl})
    else:
        out.append({"severity": "info", "path": "logging.level", "msg": "using default level=info"})
    return out
)KI");

    // Rule 3: info if a deprecated key is present.
    Handle r_dep = compile(R"KI(
Function(cfg) -> List:
    var out = []
    if "server" in cfg and "workers" in cfg["server"]:
        out.append({"severity": "info", "path": "server.workers", "msg": "'workers' is deprecated; use 'concurrency'"})
    return out
)KI");

    std::vector<Handle> rules{r_port, r_log, r_dep};

    // ---- a healthy config: no errors, one info from the default-level rule (level omitted) ----
    {
        std::string ok = R"INI(
[server]
port = 8080

[logging]
)INI";
        Handle cfg = parseIni(vm, ok);
        auto f = lint(vm, rules, cfg);
        int errs = 0, warns = 0, infos = 0;
        for (const auto& x : f) {
            if (x.severity == "error") ++errs;
            if (x.severity == "warning") ++warns;
            if (x.severity == "info") ++infos;
        }
        CHECK(errs == 0);
        CHECK(warns == 0);
        CHECK(infos == 1);
    }

    // ---- a broken config: bad port + unknown log level + deprecated workers ----
    {
        std::string bad = R"INI(
[server]
port = 99999
workers = 8

[logging]
level = shout
)INI";
        Handle cfg = parseIni(vm, bad);
        auto f = lint(vm, rules, cfg);
        bool hasPortErr = false, hasLvlWarn = false, hasDepInfo = false;
        for (const auto& x : f) {
            if (x.severity == "error" && x.path == "server.port") hasPortErr = true;
            if (x.severity == "warning" && x.path == "logging.level") hasLvlWarn = true;
            if (x.severity == "info" && x.path == "server.workers") hasDepInfo = true;
        }
        CHECK(hasPortErr);
        CHECK(hasLvlWarn);
        CHECK(hasDepInfo);
    }

    // ---- non-integer port ----
    {
        std::string s = R"INI(
[server]
port = eighty

[logging]
level = info
)INI";
        Handle cfg = parseIni(vm, s);
        auto f = lint(vm, rules, cfg);
        bool caught = false;
        for (const auto& x : f)
            if (x.severity == "error" && x.msg.find("not an integer") != std::string::npos) caught = true;
        CHECK(caught);
    }

    // ---- both required sections missing ----
    {
        std::string s = "[other]\nx = 1\n";
        Handle cfg = parseIni(vm, s);
        auto f = lint(vm, rules, cfg);
        int missing = 0;
        for (const auto& x : f)
            if (x.severity == "error" && x.msg.find("missing") != std::string::npos) ++missing;
        CHECK(missing == 2);   // server + logging
    }

    // ---- runtime rule registration + adversarial rule (returns None) ----
    {
        std::vector<Handle> extra = rules;
        extra.push_back(compile("Function(cfg): return None\n"));
        std::string ok = "[server]\nport=1\n[logging]\n";
        Handle cfg = parseIni(vm, ok);
        CHECK_THROWS(lint(vm, extra, cfg));   // None fails the isList guard
    }

    // ---- parser error: key outside a section ----
    {
        std::string s = "orphan = 1\n";
        CHECK_THROWS(parseIni(vm, s));
    }

    return RUN_TESTS();
}
