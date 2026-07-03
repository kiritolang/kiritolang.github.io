// embed_pluralize.cpp — an i18n message formatter with locale-specific plural rules. C++ owns the
// message catalogs (one Dict of {category: template} per message key per locale); Kirito owns the
// PLURAL-CATEGORY rule (a Function(n: Integer) -> String returning "one"/"few"/"many"/"other")
// and the FORMATTING (a Function(template: String, n: Integer) -> String that substitutes the
// count). C++ picks the template variant that matches the category the Kirito rule chose, then hands
// it back to Kirito to render.
//
// Flow per message: C++ (locale + key + count) → Kirito plural rule (category) → C++ (select the
// {category: template} variant) → Kirito formatter (render) → C++ (final String).

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A locale bundles its two Kirito policy functions with its catalog. The catalog maps a message key
// (e.g. "files") to a Kirito Dict of {category -> template}, where the template holds a "{n}"
// placeholder that the formatter fills in.
struct Locale {
    Handle pluralRule;   // Function(n: Integer) -> String
    Handle formatter;    // Function(template: String, n: Integer) -> String
    std::unordered_map<std::string, Handle> catalog;  // key -> Dict{category: template}
};

class Formatter {
public:
    explicit Formatter(KiritoVM& vm) : vm_(vm) {}

    void addLocale(const std::string& name, Handle rule, Handle fmt) {
        locales_[name] = Locale{rule, fmt, {}};
    }
    // Register a catalog entry from a C++ table of {category, template} pairs.
    void addMessage(const std::string& locale, const std::string& key,
                    const std::vector<std::pair<std::string, std::string>>& variants) {
        Dict d(vm_);
        for (const auto& [cat, tmpl] : variants)
            d.set(cat, Value(vm_, tmpl));
        locales_.at(locale).catalog[key] = d.handle();
    }

    // The full pipeline for one message. Throws if the plural rule picks a category the catalog
    // has no variant for — the adversarial path.
    std::string format(const std::string& locale, const std::string& key, int64_t n) {
        RootScope rs(vm_);
        const Locale& loc = locales_.at(locale);

        // 1) Kirito plural rule → category String.
        std::array<Handle, 1> ruleArgs{vm_.makeInt(n)};
        Handle catH = rs.add(vm_.arena().deref(loc.pluralRule).call(vm_, ruleArgs));
        Value category(vm_, catH);
        if (!category.isString())
            throw KiritoError("pluralize: plural rule must return a String category, got '" +
                              category.typeName() + "'");
        std::string cat = category.asStringRef("category");

        // 2) C++ selects the matching template variant. Missing variant → throw.
        Value variants(vm_, loc.catalog.at(key));
        if (!variants.has(cat))
            throw KiritoError("pluralize: no message variant for category '" + cat +
                              "' (key '" + key + "', locale '" + locale + "')");
        std::string tmpl = variants.get(cat).asStringRef("template");

        // 3) Kirito formatter renders template + count → final String.
        std::array<Handle, 2> fmtArgs{Value(vm_, tmpl).handle(), vm_.makeInt(n)};
        Handle outH = rs.add(vm_.arena().deref(loc.formatter).call(vm_, fmtArgs));
        Value out(vm_, outH);
        if (!out.isString())
            throw KiritoError("pluralize: formatter must return a String, got '" +
                              out.typeName() + "'");
        return out.asStringRef("formatted");
    }

private:
    KiritoVM& vm_;
    std::unordered_map<std::string, Locale> locales_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    Formatter fmt(vm);

    // --- English: two categories, "one" (n == 1) and "other" (everything else). ---
    Handle enRule = compile(R"KI(
Function(n) -> String:
    if n == 1:
        return "one"
    return "other"
)KI");
    // Shared English formatter: fill the "{n}" placeholder with the count via .format().
    Handle enFmt = compile(R"KI(
Function(tmpl, n) -> String:
    return tmpl.format(n)
)KI");
    fmt.addLocale("en", enRule, enFmt);
    fmt.addMessage("en", "files", {
        {"one",   "{} file"},
        {"other", "{} files"},
    });

    // --- Slavic (e.g. Russian/Polish-style): "one"/"few"/"many"/"other" by the last-digit /
    //     last-two-digits rule. n%10==1 & n%100!=11 → one; n%10 in 2..4 & n%100 not in 12..14 →
    //     few; the rest (including 0 and the teens) → many. ---
    Handle slRule = compile(R"KI(
Function(n) -> String:
    var d = n % 10
    var t = n % 100
    if d == 1 and t != 11:
        return "one"
    if d >= 2 and d <= 4 and (t < 12 or t > 14):
        return "few"
    return "many"
)KI");
    // Slavic formatter built with an f-string instead of .format(), to exercise both paths.
    Handle slFmt = compile(R"KI(
Function(tmpl, n) -> String:
    return tmpl.replace("{}", String(n))
)KI");
    fmt.addLocale("sl", slRule, slFmt);
    fmt.addMessage("sl", "files", {
        {"one",  "{} файл"},
        {"few",  "{} файла"},
        {"many", "{} файлов"},
    });

    // ---- English assertions ----
    CHECK(fmt.format("en", "files", 1) == "1 file");
    CHECK(fmt.format("en", "files", 0) == "0 files");
    CHECK(fmt.format("en", "files", 5) == "5 files");
    CHECK(fmt.format("en", "files", 42) == "42 files");
    // A count that arrives as a Kirito-computed Integer crosses back via asInt.
    Value computed(vm, vm.runSource("2 + 3\n"));
    CHECK(fmt.format("en", "files", computed.asInt("count")) == "5 files");

    // ---- Slavic assertions: the classic 1 / 2 / 5 / 21 spread plus the teens exception ----
    CHECK(fmt.format("sl", "files", 1)  == "1 файл");     // one
    CHECK(fmt.format("sl", "files", 2)  == "2 файла");    // few
    CHECK(fmt.format("sl", "files", 5)  == "5 файлов");   // many
    CHECK(fmt.format("sl", "files", 21) == "21 файл");    // one (last digit 1, not 11)
    CHECK(fmt.format("sl", "files", 22) == "22 файла");   // few
    CHECK(fmt.format("sl", "files", 11) == "11 файлов");  // many (teen exception)
    CHECK(fmt.format("sl", "files", 12) == "12 файлов");  // many (teen exception, not few)
    CHECK(fmt.format("sl", "files", 0)  == "0 файлов");   // many

    // ---- adversarial: a plural rule that yields a category the catalog lacks must throw. English
    //      has no "few" variant, so a rule that returns "few" fails the has/get selection. ----
    {
        Formatter bad(vm);
        Handle brokenRule = compile("Function(n): return \"few\"\n");
        bad.addLocale("xx", brokenRule, enFmt);
        bad.addMessage("xx", "files", {
            {"one",   "{} file"},
            {"other", "{} files"},
        });
        CHECK_THROWS(bad.format("xx", "files", 3));
    }

    // ---- adversarial: a plural rule returning a non-String category throws cleanly ----
    {
        Formatter bad(vm);
        Handle numRule = compile("Function(n): return n\n");
        bad.addLocale("yy", numRule, enFmt);
        bad.addMessage("yy", "files", {{"other", "{} files"}});
        CHECK_THROWS(bad.format("yy", "files", 2));
    }

    return RUN_TESTS();
}
