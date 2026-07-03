// embed_sql.cpp — a minimal SQL query engine over in-memory rows. C++ parses
//   SELECT col-or-udf(...) [, ...] FROM table [WHERE udf(row)]
// with user-defined functions supplied from Kirito. A UDF is a Kirito
// Function(row: Dict) -> Any registered by name; the parser recognises `NAME(*)` or `NAME(*, args)`
// syntax where `*` binds to the whole row. Filter predicates use the same UDF machinery: `WHERE
// pred(*)` calls a Bool-returning UDF per row.

#include <algorithm>
#include <cctype>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// A tiny row store: a Dict per row.
using Table = std::vector<Handle>;

class SqlEngine {
public:
    explicit SqlEngine(KiritoVM& vm) : vm_(vm) {}
    void registerTable(const std::string& name, Table t) { tables_[name] = std::move(t); }
    void registerUdf(const std::string& name, Handle fn) { udfs_[name] = fn; }

    // Run the query; returns a List of result rows (each is a List of column values).
    std::vector<std::vector<Handle>> query(const std::string& src) {
        Parser p(src);
        auto q = p.parseSelect();
        auto it = tables_.find(q.table);
        if (it == tables_.end()) throw std::runtime_error("sql: unknown table '" + q.table + "'");
        std::vector<std::vector<Handle>> out;
        RootScope rs(vm_);
        for (Handle rowH : it->second) {
            Handle rowR = rs.add(rowH);
            if (!q.whereUdf.empty()) {
                Handle predResult = callUdf(rs, q.whereUdf, rowR);
                if (!Value(vm_, predResult).truthy()) continue;
            }
            std::vector<Handle> row;
            for (const auto& proj : q.projections) {
                if (proj.udf.empty()) {
                    Value r(vm_, rowR);
                    row.push_back(r.get(proj.column).handle());
                } else {
                    row.push_back(callUdf(rs, proj.udf, rowR));
                }
            }
            out.push_back(std::move(row));
        }
        return out;
    }

private:
    struct Proj { std::string column; std::string udf; };
    struct Q { std::vector<Proj> projections; std::string table; std::string whereUdf; };

    class Parser {
    public:
        explicit Parser(const std::string& s) : s_(s) {}
        Q parseSelect() {
            skipWs();
            expectKw("SELECT");
            Q q;
            q.projections = parseProjections();
            expectKw("FROM");
            q.table = ident();
            skipWs();
            if (peekKw("WHERE")) {
                expectKw("WHERE");
                q.whereUdf = udfCall();
            }
            skipWs();
            if (i_ < s_.size()) throw std::runtime_error("sql: trailing input at pos " + std::to_string(i_));
            return q;
        }

    private:
        std::vector<Proj> parseProjections() {
            std::vector<Proj> out;
            while (true) {
                skipWs();
                if (isIdentStart(s_[i_])) {
                    std::string name = ident();
                    skipWs();
                    if (i_ < s_.size() && s_[i_] == '(') {
                        expectSym('('); expectSym('*'); expectSym(')');
                        out.push_back({"", name});
                    } else {
                        out.push_back({name, ""});
                    }
                }
                skipWs();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                break;
            }
            return out;
        }
        std::string udfCall() {
            skipWs();
            std::string name = ident();
            skipWs();
            expectSym('('); expectSym('*'); expectSym(')');
            return name;
        }
        std::string ident() {
            skipWs();
            if (i_ >= s_.size() || !isIdentStart(s_[i_])) throw std::runtime_error("sql: expected identifier at pos " + std::to_string(i_));
            std::size_t j = i_;
            while (j < s_.size() && (isIdentCont(s_[j]))) ++j;
            std::string t = s_.substr(i_, j - i_);
            i_ = j;
            return t;
        }
        bool peekKw(const std::string& kw) {
            std::size_t save = i_;
            skipWs();
            std::string t;
            std::size_t j = i_;
            while (j < s_.size() && isIdentCont(s_[j])) t += s_[j++];
            i_ = save;
            return icmp(t, kw);
        }
        void expectKw(const std::string& kw) {
            skipWs();
            std::string t = ident();
            if (!icmp(t, kw)) throw std::runtime_error("sql: expected keyword " + kw + " at pos " + std::to_string(i_));
        }
        void expectSym(char c) { skipWs(); if (i_ >= s_.size() || s_[i_] != c) throw std::runtime_error(std::string("sql: expected '") + c + "'"); ++i_; }
        void skipWs() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
        static bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
        static bool isIdentCont(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
        static bool icmp(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t k = 0; k < a.size(); ++k)
                if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(b[k]))) return false;
            return true;
        }
        std::string s_; std::size_t i_ = 0;
    };

    Handle callUdf(RootScope& rs, const std::string& name, Handle rowH) {
        auto it = udfs_.find(name);
        if (it == udfs_.end()) throw std::runtime_error("sql: unknown UDF '" + name + "'");
        std::array<Handle, 1> args{rowH};
        return rs.add(vm_.arena().deref(it->second).call(vm_, args));
    }

    KiritoVM& vm_;
    std::unordered_map<std::string, Table> tables_;
    std::unordered_map<std::string, Handle> udfs_;
};

int main() {
    KiritoVM vm;
    SqlEngine sql(vm);

    // Build a `people` table: three rows with name (String) + age (Integer) + city (String).
    auto row = [&](const std::string& n, int64_t a, const std::string& c) {
        Dict d(vm);
        d.set("name", Value(vm, n));
        d.set("age",  Value(vm, a));
        d.set("city", Value(vm, c));
        return d.build().handle();
    };
    sql.registerTable("people", {row("Ada", 36, "London"),
                                 row("Alan", 41, "Manchester"),
                                 row("Grace", 79, "New York"),
                                 row("Linus", 54, "Portland")});

    // UDFs written in Kirito.
    auto compile = [&](const char* s) { return vm.runSource(s); };
    sql.registerUdf("upper_name", compile("Function(r): return r[\"name\"].upper()\n"));
    sql.registerUdf("age_bucket", compile(R"KI(
Function(r):
    if r["age"] < 40:
        return "young"
    if r["age"] < 65:
        return "middle"
    return "senior"
)KI"));
    sql.registerUdf("is_senior", compile("Function(r): return r[\"age\"] >= 65\n"));
    sql.registerUdf("from_UK",   compile("Function(r): return r[\"city\"] == \"London\" or r[\"city\"] == \"Manchester\"\n"));

    // --- SELECT name, age FROM people
    {
        auto rows = sql.query("SELECT name, age FROM people");
        CHECK(rows.size() == 4);
        CHECK(rows[0].size() == 2);
        CHECK(Value(vm, rows[0][0]).asStringRef("") == "Ada");
        CHECK(Value(vm, rows[0][1]).asInt("") == 36);
    }

    // --- SELECT upper_name(*), age_bucket(*) FROM people WHERE is_senior(*)
    {
        auto rows = sql.query("SELECT upper_name(*), age_bucket(*) FROM people WHERE is_senior(*)");
        CHECK(rows.size() == 1);       // Grace only
        CHECK(Value(vm, rows[0][0]).asStringRef("") == "GRACE");
        CHECK(Value(vm, rows[0][1]).asStringRef("") == "senior");
    }

    // --- SELECT name FROM people WHERE from_UK(*)
    {
        auto rows = sql.query("SELECT name FROM people WHERE from_UK(*)");
        CHECK(rows.size() == 2);
        std::vector<std::string> got;
        for (auto& r : rows) got.push_back(Value(vm, r[0]).asStringRef(""));
        std::vector<std::string> want{"Ada", "Alan"};
        CHECK(got == want);
    }

    // --- case-insensitive keywords ----
    {
        auto rows = sql.query("select name FROM people where from_UK(*)");
        CHECK(rows.size() == 2);
    }

    // --- adversarial: unknown table + unknown UDF + missing WHERE-udf + parse errors ----
    CHECK_THROWS(sql.query("SELECT name FROM nobody"));
    CHECK_THROWS(sql.query("SELECT bogus_udf(*) FROM people"));
    CHECK_THROWS(sql.query("SELECT name FROM people WHERE unknown_pred(*)"));
    CHECK_THROWS(sql.query("SELECT FROM people"));
    CHECK_THROWS(sql.query("SELECT name FROM"));

    // --- a UDF that throws surfaces the message ----
    sql.registerUdf("kaboom", compile("Function(r): throw \"bang\"\n"));
    CHECK_THROWS(sql.query("SELECT kaboom(*) FROM people"));

    // --- register a NEW UDF and re-run — no restart needed ----
    sql.registerUdf("name_length", compile("Function(r): return len(r[\"name\"])\n"));
    {
        auto rows = sql.query("SELECT name_length(*) FROM people");
        CHECK(rows.size() == 4);
        CHECK(Value(vm, rows[0][0]).asInt("") == 3);   // "Ada" == 3
    }

    return RUN_TESTS();
}
