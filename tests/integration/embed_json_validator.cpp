// embed_json_validator.cpp — a schema validator for JSON-like records. C++ owns the document
// parsing (via Kirito's json module), the rule registry, and the error-aggregation loop; Kirito
// owns each field rule: a Function(record: Dict) -> List of String returning human-readable error
// messages (an empty list means the record satisfies that rule).
//
// Flow: C++ hands a JSON source string to Kirito's json.loads → gets a Dict record → runs every
// rule Kirito-side → C++ collects and flattens all the returned error messages.

#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Parse a JSON source string into a Kirito value by driving the `json` module from C++. Kirito owns
// the actual parse; C++ only supplies the source and receives the resulting Dict/List.
class JsonValidator {
public:
    explicit JsonValidator(KiritoVM& vm) : vm_(vm) {
        // A compiled Kirito function value: json.loads over a source String argument. Making the
        // Function literal the trailing expression yields its handle from runSource.
        parser_ = vm_.runSource(R"KI(
var __json = import("json")
Function(src):
    return __json.loads(src)
)KI");
    }

    // Register a field rule: Function(record: Dict) -> List of String.
    void addRule(Handle fn) { rules_.push_back(fn); }

    // Parse `json` and return the parsed Kirito value wrapped for C++ inspection.
    Value parse(const std::string& json) {
        std::array<Handle, 1> args{Value(vm_, json).handle()};
        Handle out = vm_.arena().deref(parser_).call(vm_, args);
        return Value(vm_, out);
    }

    // Validate one parsed record against every rule, aggregating all error messages in rule order.
    std::vector<std::string> validate(Value record) {
        std::vector<std::string> errors;
        RootScope rs(vm_);
        Handle recH = rs.add(record.handle());
        for (Handle rH : rules_) {
            std::array<Handle, 1> args{recH};
            Handle resH = rs.add(vm_.arena().deref(rH).call(vm_, args));
            Value result(vm_, resH);
            // A rule MUST return a List of Strings; anything else is a schema bug — fail loudly.
            if (!result.isList())
                throw KiritoError("validator: rule must return a List, got '" + result.typeName() + "'");
            for (Value item : result.items()) {
                if (!item.isString())
                    throw KiritoError("validator: rule error entry must be a String, got '" +
                                      item.typeName() + "'");
                errors.push_back(item.asStringRef("rule error"));
            }
        }
        return errors;
    }

private:
    KiritoVM&           vm_;
    Handle              parser_{};
    std::vector<Handle> rules_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    JsonValidator v(vm);

    // Rule: "name" is required and must be a non-empty String.
    v.addRule(compile(R"KI(
Function(rec) -> List:
    if "name" not in rec:
        return ["name: required field missing"]
    if not isinstance(rec["name"], String):
        return ["name: must be a String"]
    if len(rec["name"]) == 0:
        return ["name: must not be empty"]
    return []
)KI"));

    // Rule: "age" is required, must be an Integer, and must fall in [0, 150].
    v.addRule(compile(R"KI(
Function(rec) -> List:
    var errs = []
    if "age" not in rec:
        errs.append("age: required field missing")
        return errs
    if not isinstance(rec["age"], Integer):
        errs.append("age: must be an Integer")
        return errs
    if rec["age"] < 0:
        errs.append("age: must not be negative")
    if rec["age"] > 150:
        errs.append("age: exceeds maximum of 150")
    return errs
)KI"));

    // Rule: "email", if present, must be a String containing "@".
    v.addRule(compile(R"KI(
Function(rec) -> List:
    if "email" not in rec:
        return []
    if not isinstance(rec["email"], String):
        return ["email: must be a String"]
    if not ("@" in rec["email"]):
        return ["email: must contain '@'"]
    return []
)KI"));

    // Rule: "tags", if present, must be a List whose every element is a String.
    v.addRule(compile(R"KI(
Function(rec) -> List:
    if "tags" not in rec:
        return []
    if not isinstance(rec["tags"], List):
        return ["tags: must be a List"]
    var errs = []
    for t in rec["tags"]:
        if not isinstance(t, String):
            errs.append("tags: every tag must be a String")
            return errs
    return errs
)KI"));

    // ---- a valid record: zero aggregated errors ----
    {
        Value rec = v.parse(R"({"name": "Asuna", "age": 17, "email": "asuna@sao.net",
                                 "tags": ["swordswoman", "cook"]})");
        CHECK(rec.isDict());
        std::vector<std::string> errs = v.validate(rec);
        CHECK(errs.empty());
    }

    // ---- an invalid record: several rules fire; errors aggregate in rule order ----
    {
        // name empty, age out of range + also present as Integer, email missing '@', tags has a
        // non-String element.
        Value rec = v.parse(R"({"name": "", "age": 999, "email": "no-at-sign",
                                 "tags": ["ok", 42]})");
        std::vector<std::string> errs = v.validate(rec);
        CHECK(errs.size() == 4);
        CHECK(errs.at(0) == "name: must not be empty");
        CHECK(errs.at(1) == "age: exceeds maximum of 150");
        CHECK(errs.at(2) == "email: must contain '@'");
        CHECK(errs.at(3) == "tags: every tag must be a String");
    }

    // ---- a record missing required fields entirely ----
    {
        Value rec = v.parse(R"({"email": "x@y.z"})");
        std::vector<std::string> errs = v.validate(rec);
        CHECK(errs.size() == 2);
        CHECK(errs.at(0) == "name: required field missing");
        CHECK(errs.at(1) == "age: required field missing");
    }

    // ---- wrong-type fields ----
    {
        Value rec = v.parse(R"({"name": 5, "age": "old"})");
        std::vector<std::string> errs = v.validate(rec);
        CHECK(errs.size() == 2);
        CHECK(errs.at(0) == "name: must be a String");
        CHECK(errs.at(1) == "age: must be an Integer");
    }

    // ---- the parser itself yields the right shapes ----
    {
        Value arr = v.parse(R"([1, 2, 3])");
        CHECK(arr.isList());
        CHECK(arr.len() == 3);
        Value scalar = v.parse(R"(42)");
        CHECK(scalar.isInt());
        CHECK(scalar.asInt("scalar") == 42);
    }

    // ---- adversarial: a rule that returns the wrong type (a String, not a List) throws ----
    {
        JsonValidator bad(vm);
        bad.addRule(compile("Function(rec): return \"not-a-list\"\n"));
        Value rec = bad.parse(R"({"name": "ok", "age": 10})");
        CHECK_THROWS(bad.validate(rec));
    }

    // ---- adversarial: a rule that returns a List of non-Strings throws ----
    {
        JsonValidator bad(vm);
        bad.addRule(compile("Function(rec): return [1, 2, 3]\n"));
        Value rec = bad.parse(R"({"name": "ok", "age": 10})");
        CHECK_THROWS(bad.validate(rec));
    }

    // ---- adversarial: a rule that itself throws — it indexes a key that may be missing WITHOUT a
    //      guard, so on a record lacking "score" the Kirito KeyError crosses the boundary and is
    //      caught by the C++ call wrapper. ----
    {
        JsonValidator bad(vm);
        bad.addRule(compile(R"KI(
Function(rec) -> List:
    if rec["missing"] > 0:
        return ["score: too high"]
    return []
)KI"));
        Value rec = bad.parse(R"({"name": "ok", "age": 10})");
        CHECK_THROWS(bad.validate(rec));
    }

    // ---- adversarial: feeding json.loads a malformed document throws at parse time ----
    {
        CHECK_THROWS(v.parse(R"({"name": )"));
    }

    return RUN_TESTS();
}
