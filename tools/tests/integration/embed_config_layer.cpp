// embed_config_layer.cpp — a layered configuration resolver (defaults ← environment ← overrides).
// C++ owns an ordered list of layer Dicts and folds them left-to-right through a Kirito deep-merge
// Function(base: Dict, overlay: Dict) -> Dict (overlay wins; nested Dicts merge recursively). A
// second Kirito Function(cfg: Dict) -> List of String validates the effective config (missing
// required keys, out-of-range values). C++ owns the layer stack + the fold loop; Kirito owns the
// merge policy and the validation rules.
//
// Flow: C++ (layer stack) → fold each layer → Kirito merge (deep) → effective Dict → Kirito
// validate → List of error Strings → C++ (assert).

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A resolver owns an ordered layer stack and the two Kirito policy functions. resolve() folds the
// layers left-to-right through the Kirito merge; validate() runs the Kirito validator on a config.
class ConfigResolver {
public:
    ConfigResolver(KiritoVM& vm, Handle merge, Handle validate)
        : vm_(vm), merge_(merge), validate_(validate) {}

    void addLayer(Handle layer) { layers_.push_back(layer); }

    // Fold all layers through the Kirito merge. Start from an empty Dict so a single layer, or no
    // layer at all, is well-defined. Returns the effective config Dict handle.
    Handle resolve() {
        RootScope rs(vm_);
        Handle acc = rs.add(Dict(vm_).handle());
        for (Handle layer : layers_) {
            std::array<Handle, 2> args{acc, layer};
            Handle merged = vm_.arena().deref(merge_).call(vm_, args);
            acc = rs.add(merged);
            Value mv(vm_, acc);
            // The merge policy MUST return a Dict, or the fold is meaningless. Fail loudly.
            if (!mv.isDict())
                throw KiritoError("config: merge must return a Dict, got '" + mv.typeName() + "'");
        }
        return acc;
    }

    // Run the Kirito validator. It must return a List of Strings (each an error message); an empty
    // list means the config is valid.
    std::vector<std::string> validate(Value cfg) {
        std::array<Handle, 1> args{cfg.handle()};
        Handle rH = vm_.arena().deref(validate_).call(vm_, args);
        Value result(vm_, rH);
        if (!result.isList())
            throw KiritoError("config: validate must return a List, got '" + result.typeName() + "'");
        std::vector<std::string> errs;
        for (Value item : result.items())
            errs.push_back(item.asStringRef("validation error"));
        return errs;
    }

private:
    KiritoVM&           vm_;
    Handle              merge_;
    Handle              validate_;
    std::vector<Handle> layers_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // The merge policy: deep-merge two config layers. overlay wins; where BOTH sides hold a Dict for
    // the same key, recurse. Otherwise overlay's value replaces base's outright — that is the
    // type-conflict rule (a scalar overlay silently overrides a Dict base and vice versa).
    Handle merge = compile(R"KI(
Function(base : Dict, overlay : Dict) -> Dict:
    var out = {}
    for k in base.keys():
        out[k] = base[k]
    for k in overlay.keys():
        if k in base and isinstance(base[k], Dict) and isinstance(overlay[k], Dict):
            out[k] = merge(base[k], overlay[k])
        else:
            out[k] = overlay[k]
    return out

var merge = Function(base : Dict, overlay : Dict) -> Dict:
    var out = {}
    for k in base.keys():
        out[k] = base[k]
    for k in overlay.keys():
        if k in base and isinstance(base[k], Dict) and isinstance(overlay[k], Dict):
            out[k] = merge(base[k], overlay[k])
        else:
            out[k] = overlay[k]
    return out
merge
)KI");

    // The validator: return a List of error Strings. Required top-level keys must be present; the
    // "server" section's "port" must be in range; "log"."level" must be one of the allowed set.
    Handle validate = compile(R"KI(
Function(cfg : Dict) -> List:
    var errs = []
    for req in ["server", "log"]:
        if not req in cfg:
            errs.append("missing required section: " + req)
    if "server" in cfg:
        var srv = cfg["server"]
        if "port" in srv:
            var port = srv["port"]
            if port < 1 or port > 65535:
                errs.append("server.port out of range: " + String(port))
        else:
            errs.append("missing server.port")
    if "log" in cfg:
        var lvl = cfg["log"]["level"]
        if not (lvl == "debug" or lvl == "info" or lvl == "warn" or lvl == "error"):
            errs.append("bad log.level: " + lvl)
    return errs
)KI");

    ConfigResolver resolver(vm, merge, validate);

    // Layer 1 — DEFAULTS: a full, valid baseline with nested sections.
    {
        Dict defaults(vm);
        Dict server(vm);
        server.set("host", Value(vm, "0.0.0.0"));
        server.set("port", Value(vm, (int64_t)8080));
        server.set("workers", Value(vm, (int64_t)4));
        defaults.set("server", Value(vm, server.handle()));
        Dict log(vm);
        log.set("level", Value(vm, "info"));
        log.set("format", Value(vm, "text"));
        defaults.set("log", Value(vm, log.handle()));
        defaults.set("debug", Value(vm, false));
        resolver.addLayer(defaults.handle());
    }
    // Layer 2 — ENVIRONMENT: a deep override of server.port + a new nested section.
    {
        Dict env(vm);
        Dict server(vm);
        server.set("port", Value(vm, (int64_t)9090));   // deep override of the default 8080
        env.set("server", Value(vm, server.handle()));
        Dict cache(vm);
        cache.set("ttl", Value(vm, (int64_t)300));
        env.set("cache", Value(vm, cache.handle()));
        resolver.addLayer(env.handle());
    }
    // Layer 3 — OVERRIDES: flip debug, deepen log.level.
    {
        Dict over(vm);
        over.set("debug", Value(vm, true));
        Dict log(vm);
        log.set("level", Value(vm, "debug"));
        over.set("log", Value(vm, log.handle()));
        resolver.addLayer(over.handle());
    }

    RootScope rs(vm);
    Value cfg(vm, rs.add(resolver.resolve()));

    // ---- resolved-value assertions ----
    Value server = cfg.get("server");
    // deep override: port came from the ENVIRONMENT layer, not the default
    CHECK(server.get("port").asInt("port") == 9090);
    // inherited default: host + workers were never overridden, so they survive from layer 1
    CHECK(server.get("host").asStringRef("host") == "0.0.0.0");
    CHECK(server.get("workers").asInt("workers") == 4);
    // nested merge preserved the sibling key: log.format from defaults, log.level from overrides
    Value log = cfg.get("log");
    CHECK(log.get("level").asStringRef("level") == "debug");
    CHECK(log.get("format").asStringRef("format") == "text");
    // a whole new section contributed by a later layer is present
    CHECK(cfg.get("cache").get("ttl").asInt("ttl") == 300);
    // a scalar overridden by a later layer takes the later value
    CHECK(cfg.get("debug").asBool("debug") == true);

    // ---- validation passes on the well-formed effective config ----
    std::vector<std::string> ok = resolver.validate(cfg);
    CHECK(ok.empty());

    // ---- validation FAILS: build a config missing "log" and with an out-of-range port ----
    {
        Dict bad(vm);
        Dict server2(vm);
        server2.set("port", Value(vm, (int64_t)70000));   // out of range
        bad.set("server", Value(vm, server2.handle()));
        std::vector<std::string> errs = resolver.validate(Value(vm, bad.handle()));
        // exactly two problems: missing "log" section and the port range
        CHECK(errs.size() == 2);
        bool sawMissingLog = false, sawPort = false;
        for (const std::string& e : errs) {
            if (e.find("missing required section: log") != std::string::npos) sawMissingLog = true;
            if (e.find("server.port out of range") != std::string::npos)      sawPort = true;
        }
        CHECK(sawMissingLog);
        CHECK(sawPort);
    }

    // ---- adversarial: type-conflict merge rule (overlay wins). base holds a Dict at "server";
    //      overlay holds a scalar there. Our rule says overlay replaces outright. ----
    {
        ConfigResolver conflict(vm, merge, validate);
        Dict base(vm);
        Dict inner(vm);
        inner.set("port", Value(vm, (int64_t)80));
        base.set("server", Value(vm, inner.handle()));
        conflict.addLayer(base.handle());
        Dict overlay(vm);
        overlay.set("server", Value(vm, "disabled"));   // scalar clobbers the Dict
        conflict.addLayer(overlay.handle());
        RootScope rs2(vm);
        Value merged(vm, rs2.add(conflict.resolve()));
        CHECK(merged.get("server").isString());
        CHECK(merged.get("server").asStringRef("server") == "disabled");
    }

    // ---- adversarial: the reverse conflict — base scalar, overlay Dict — overlay still wins ----
    {
        ConfigResolver conflict(vm, merge, validate);
        Dict base(vm);
        base.set("server", Value(vm, (int64_t)1));
        conflict.addLayer(base.handle());
        Dict overlay(vm);
        Dict inner(vm);
        inner.set("port", Value(vm, (int64_t)443));
        overlay.set("server", Value(vm, inner.handle()));
        conflict.addLayer(overlay.handle());
        RootScope rs2(vm);
        Value merged(vm, rs2.add(conflict.resolve()));
        CHECK(merged.get("server").isDict());
        CHECK(merged.get("server").get("port").asInt("port") == 443);
    }

    // ---- adversarial: a validation rule that reaches a missing nested key UNGUARDED must throw.
    //      This validator indexes cfg["log"]["level"] with no has()-guard on "log". ----
    {
        Handle unguarded = compile(R"KI(
Function(cfg : Dict) -> List:
    var lvl = cfg["log"]["level"]
    return []
)KI");
        ConfigResolver bad(vm, merge, unguarded);
        Dict noLog(vm);
        Dict srv(vm);
        srv.set("port", Value(vm, (int64_t)8080));
        noLog.set("server", Value(vm, srv.handle()));
        CHECK_THROWS(bad.validate(Value(vm, noLog.handle())));
    }

    return RUN_TESTS();
}
