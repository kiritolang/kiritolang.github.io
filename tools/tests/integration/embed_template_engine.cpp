// embed_template_engine.cpp — a {{name}} text templating engine. C++ owns the template strings and
// the scan/splice loop; Kirito owns the per-placeholder RESOLVER — a
// Function(name: String, ctx: Dict) -> String that computes the replacement text from a context Dict
// (computed values, formatting, upper-casing, a default when the key is absent).
//
// Flow per template: C++ scans for {{name}} → for each placeholder → Kirito (resolve name to text)
// → C++ (splice the returned String into the output buffer).

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A tiny template engine: it holds one Kirito resolver and renders template strings that contain
// {{name}} placeholders. Literal text between placeholders passes through verbatim; each placeholder
// is replaced by whatever the resolver returns for that name against the context Dict.
class TemplateEngine {
public:
    TemplateEngine(KiritoVM& vm, Handle resolver) : vm_(vm), resolver_(resolver) {}

    std::string render(const std::string& tmpl, const Dict& ctx) {
        RootScope rs(vm_);
        Handle ctxH = rs.add(ctx.handle());
        std::string out;
        std::size_t i = 0;
        while (i < tmpl.size()) {
            std::size_t open = tmpl.find("{{", i);
            if (open == std::string::npos) {
                out += tmpl.substr(i);
                break;
            }
            out += tmpl.substr(i, open - i); // literal chunk before the placeholder
            std::size_t close = tmpl.find("}}", open + 2);
            if (close == std::string::npos)
                throw KiritoError("template: unterminated placeholder (missing '}}')");
            std::string name = trim(tmpl.substr(open + 2, close - (open + 2)));

            // Hand (name, ctx) to the Kirito resolver; it must return a String.
            Handle nameH = rs.add(Value(vm_, name).handle());
            std::array<Handle, 2> args{nameH, ctxH};
            Handle rH = rs.add(vm_.arena().deref(resolver_).call(vm_, args));
            Value result(vm_, rH);
            if (!result.isString())
                throw KiritoError("template: resolver must return a String, got '" +
                                  result.typeName() + "'");
            out += result.asStringRef("resolved value");
            i = close + 2;
        }
        return out;
    }

private:
    static std::string trim(std::string s) {
        std::size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        std::size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }

    KiritoVM& vm_;
    Handle    resolver_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // The resolver: look the name up in ctx. Missing keys fall back to a default; the "price" name is
    // COMPUTED (formatted from a numeric field); "user" is upper-cased. Exercises String
    // concatenation, .upper(), .format()/f-strings, and a default-when-absent branch.
    Handle resolver = compile(R"KI(
Function(name, ctx) -> String:
    if name == "price":
        var cents = ctx.get("price_cents", 0)
        var dollars = cents / 100.0
        return "$" + format(dollars, ".2f")
    if name == "user":
        var u = ctx.get("user", "anonymous")
        return u.upper()
    if name == "greeting":
        return f"Hello, {ctx.get('user', 'friend')}!"
    if name in ctx:
        return String(ctx[name])
    return "<missing:" + name + ">"
)KI");

    TemplateEngine engine(vm, resolver);

    Dict ctx(vm);
    ctx.set("user", Value(vm, "alice"));
    ctx.set("price_cents", Value(vm, int64_t{1999}));
    ctx.set("item", Value(vm, "Widget"));

    // ---- basic render: literal text + several placeholders, computed + upper-cased ----
    std::string r1 = engine.render("Order for {{user}}: {{item}} at {{price}}.", ctx);
    CHECK(r1 == "Order for ALICE: Widget at $19.99.");

    // ---- whitespace inside the braces is trimmed; an f-string greeting ----
    std::string r2 = engine.render("{{ greeting }}", ctx);
    CHECK(r2 == "Hello, alice!");

    // ---- default when the key is absent ----
    std::string r3 = engine.render("[{{nope}}]", ctx);
    CHECK(r3 == "[<missing:nope>]");

    // ---- no placeholders: pure passthrough ----
    std::string r4 = engine.render("just literal text, no braces", ctx);
    CHECK(r4 == "just literal text, no braces");

    // ---- adjacent + repeated placeholders in one template ----
    std::string r5 = engine.render("{{user}}{{user}} buys {{item}}", ctx);
    CHECK(r5 == "ALICEALICE buys Widget");

    // ---- a different context reuses the same engine/resolver ----
    {
        Dict ctx2(vm);
        ctx2.set("user", Value(vm, "bob"));
        ctx2.set("price_cents", Value(vm, int64_t{500}));
        ctx2.set("item", Value(vm, "Gadget"));
        std::string r = engine.render("{{user}} -> {{item}} ({{price}})", ctx2);
        CHECK(r == "BOB -> Gadget ($5.00)");

        // price defaults to 0 when the field is missing
        Dict ctx3(vm);
        std::string rp = engine.render("{{price}}", ctx3);
        CHECK(rp == "$0.00");
    }

    // ---- empty template renders empty ----
    CHECK(engine.render("", ctx).empty());

    // ---- adversarial: a resolver that returns a non-String must throw ----
    {
        Handle badResolver = compile(R"KI(
Function(name, ctx) -> Any:
    return 42
)KI");
        TemplateEngine bad(vm, badResolver);
        CHECK_THROWS(bad.render("{{x}}", ctx));
    }

    // ---- adversarial: unterminated placeholder throws cleanly ----
    CHECK_THROWS(engine.render("broken {{user", ctx));

    // ---- adversarial: a resolver that throws inside Kirito (bad key access) propagates ----
    {
        Handle throwing = compile(R"KI(
Function(name, ctx) -> String:
    return ctx["definitely_missing_key"]
)KI");
        TemplateEngine te(vm, throwing);
        CHECK_THROWS(te.render("{{anything}}", ctx));
    }

    return RUN_TESTS();
}
