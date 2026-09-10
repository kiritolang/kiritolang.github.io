// Deep test of fum::unordered_map heterogeneous ("transparent") lookup (added so instance-attribute
// access can look up by string_view without constructing a temporary std::string key). Covers:
// transparent find/count/contains, string / string_view / const char* consistency, adversarial keys,
// the non-transparent path staying unaffected, and a differential run against std::unordered_map.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../check.hpp"
#include "fum/unordered_map.hpp"

struct SVHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(std::string_view(s)); }
};
using TMap = fum::unordered_map<std::string, int, SVHash, std::equal_to<>>;

int main() {
    // ---- heterogeneous lookup locates an entry stored under a std::string key ----
    TMap m;
    m["alpha"] = 1;
    m["beta"] = 2;
    m["gamma"] = 3;
    std::string_view sv = "beta";
    CHECK(m.find(sv) != m.end() && m.find(sv)->second == 2);          // string_view
    CHECK(m.find("gamma") != m.end() && m.find("gamma")->second == 3); // const char*
    CHECK(m.find(std::string("alpha")) != m.end() && m.find(std::string("alpha"))->second == 1);
    CHECK(m.count(sv) == 1 && m.contains("gamma"));
    CHECK(m.find(std::string_view("missing")) == m.end());
    CHECK(m.count("missing") == 0 && !m.contains("missing"));
    const TMap& cm = m;
    CHECK(cm.find(std::string_view("alpha")) != cm.end() && cm.find("alpha")->second == 1);

    // ---- consistency: string / string_view / const char* forms of an equal key all agree ----
    for (const char* k : {"alpha", "beta", "gamma"}) {
        std::string_view v{k};
        CHECK(m.find(k) != m.end());
        CHECK(m.find(k)->second == m.find(v)->second);
        CHECK(m.find(v)->second == m.find(std::string(k))->second);
    }

    // ---- adversarial keys: empty string and an embedded NUL (length matters, not C-string) ----
    m[std::string("")] = 10;
    CHECK(m.find(std::string_view("")) != m.end() && m.find("")->second == 10);
    std::string nul("a\0b", 3);
    m[nul] = 42;
    std::string_view nulsv(nul.data(), nul.size());
    CHECK(m.find(nulsv) != m.end() && m.find(nulsv)->second == 42);
    CHECK(m.find("a") == m.end());  // "a" (len 1) must not match "a\0b" (len 3)

    // ---- the non-transparent default map is unaffected and still works ----
    fum::unordered_map<std::string, int> plain;
    plain["x"] = 7;
    CHECK(plain.find(std::string("x")) != plain.end() && plain.at("x") == 7);
    CHECK(plain.count("x") == 1 && plain.contains("x"));

    // ---- differential vs std::unordered_map over a deterministic random op sequence ----
    TMap fm;
    std::unordered_map<std::string, int> ref;
    std::uint64_t s = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 8000; ++i) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        std::string key = "k" + std::to_string(static_cast<int>((s >> 33) % 250));
        int op = static_cast<int>((s >> 17) & 3);
        if (op == 0 || op == 1) {
            fm[key] = i;
            ref[key] = i;
        } else if (op == 2) {
            fm.erase(key);
            ref.erase(key);
        } else {
            std::string_view kv = key;   // look up via string_view; must match the reference exactly
            bool a = fm.find(kv) != fm.end();
            bool b = ref.find(key) != ref.end();
            CHECK(a == b);
            if (a) CHECK(fm.find(kv)->second == ref.find(key)->second);
        }
        CHECK(fm.size() == ref.size());
    }
    return RUN_TESTS();
}
