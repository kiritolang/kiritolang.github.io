// The built-in `tabular` module (Series/DataFrame): a C++ integration test that the frozen Kirito
// module loads and its core surface works end-to-end. Exhaustive behavioral + fuzz coverage lives in
// tests/scripts/spec_tabular.ki; this guards the embedded module and the import path.
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

// Run a program that ends in an expression and return its stringified value. Each call uses a fresh
// VM so the cases stay independent.
static std::string run(const std::string& body) {
    KiritoVM vm;
    return vm.stringify(vm.runSource("var pd = import(\"tabular\")\n" + body));
}

int main() {
    // --- Series ---
    CHECK(run("pd.Series([1, 2, 3, 4]).sum()") == "10");
    CHECK(run("pd.Series([1, 2, 3, 4]).mean()") == "2.5");
    CHECK(run("pd.Series([3, 1, 2]).sortvalues().tolist()") == "[1, 2, 3]");
    CHECK(run("(pd.Series([1, 2, 3]) * 10).tolist()") == "[10, 20, 30]");
    CHECK(run("(pd.Series([1, 2, 3, 4]) > 2).tolist()") == "[False, False, True, True]");
    // ==/!= are element-wise (like >/<), so a DataFrame can be filtered by an equality mask.
    CHECK(run("(pd.Series([1, 2, 1]) == 1).tolist()") == "[True, False, True]");
    CHECK(run("(pd.Series([\"a\", \"b\", \"a\"]) != \"a\").tolist()") == "[False, True, False]");
    CHECK(run("var d = pd.DataFrame({\"k\": [\"x\", \"y\", \"x\"], \"v\": [1, 2, 3]}, columns=[\"k\", \"v\"])\n"
              "d[d[\"k\"] == \"x\"][\"v\"].tolist()") == "[1, 3]");
    CHECK(run("pd.Series([1, None, 3]).sum()") == "4");          // missing skipped
    CHECK(run("pd.Series([1, None, 3]).dropna().tolist()") == "[1, 3]");

    // --- DataFrame construction + selection ---
    const char* mk = "var df = pd.DataFrame([[\"Ada\", 36], [\"Alan\", 41], [\"Grace\", 85]], columns=[\"name\", \"age\"])\n";
    CHECK(run(std::string(mk) + "df.shape()") == "[3, 2]");
    CHECK(run(std::string(mk) + "df.columns") == "['name', 'age']");
    CHECK(run(std::string(mk) + "df[\"age\"].tolist()") == "[36, 41, 85]");
    CHECK(run(std::string(mk) + "df.iloc[1][\"name\"]") == "Alan");
    CHECK(run(std::string(mk) + "df[df[\"age\"] > 40][\"name\"].tolist()") == "['Alan', 'Grace']");
    CHECK(run(std::string(mk) + "df.sortvalues(\"age\", ascending=False)[\"name\"].tolist()") == "['Grace', 'Alan', 'Ada']");

    // --- CSV round-trip + type inference ---
    CHECK(run("type(pd.readcsv(\"a,b\\n1,2.5\")[\"a\"][0])") == "Integer");
    CHECK(run("type(pd.readcsv(\"a,b\\n1,2.5\")[\"b\"][0])") == "Float");
    CHECK(run("pd.readcsv(\"x\\n1\\n2\\n3\")[\"x\"].sum()") == "6");

    // --- group-by ---
    const char* gb = "var df = pd.DataFrame({\"k\": [\"a\", \"b\", \"a\", \"b\"], \"v\": [1, 2, 3, 4]}, columns=[\"k\", \"v\"])\n"
                     "var g = df.groupby(\"k\")\n";
    CHECK(run(std::string(gb) + "g.sum()[\"v\"].tolist()") == "[4, 6]");
    CHECK(run(std::string(gb) + "g.mean()[\"v\"].tolist()") == "[2.0, 3.0]");
    CHECK(run(std::string(gb) + "g.size().tolist()") == "[2, 2]");

    // --- joins + concat ---
    const char* jn = "var l = pd.DataFrame([[1, \"x\"], [2, \"y\"]], columns=[\"id\", \"a\"])\n"
                     "var r = pd.DataFrame([[1, \"p\"], [3, \"q\"]], columns=[\"id\", \"b\"])\n";
    CHECK(run(std::string(jn) + "pd.merge(l, r, on=\"id\").shape()") == "[1, 3]");
    CHECK(run(std::string(jn) + "pd.merge(l, r, on=\"id\", how=\"left\")[\"b\"].tolist()") == "['p', None]");
    CHECK(run("pd.concat([pd.DataFrame([[1]], columns=[\"x\"]), pd.DataFrame([[2]], columns=[\"x\"])])[\"x\"].tolist()") == "[1, 2]");

    // --- missing-data on a frame ---
    CHECK(run("len(pd.readcsv(\"a,b\\n1,\\n2,3\").dropna())") == "1");
    CHECK(run("pd.readcsv(\"a,b\\n1,\\n2,3\").fillna(0)[\"b\"].tolist()") == "[0, 3]");

    // --- edge cases: empty / all-missing / out-of-bounds degrade gracefully ---
    CHECK(run("len(pd.Series([]))") == "0");
    CHECK(run("pd.Series([]).sum()") == "0");
    CHECK(run("pd.Series([]).mean()") == "None");            // mean of nothing is None, not a crash
    CHECK(run("pd.Series([]).count()") == "0");
    CHECK(run("pd.Series([None, None]).count()") == "0");
    CHECK(run("pd.Series([None, None]).mean()") == "None");
    CHECK(run("pd.Series([None, 1, None]).fillna(0).tolist()") == "[0, 1, 0]");
    CHECK(run("pd.Series([7]).std()") == "None");             // std needs >= 2 values
    CHECK(run("pd.readcsv(\"\").shape()") == "[0, 0]");        // empty CSV
    CHECK(run("pd.readcsv(\"a,b,c\").shape()") == "[0, 3]");   // header only: columns, no rows
    CHECK(run("pd.DataFrame({\"a\": [1, 2, 3]}, columns=[\"a\"]).head(0).shape()") == "[0, 1]");
    CHECK(run("pd.DataFrame({\"a\": [1, 2]}, columns=[\"a\"]).head(99).shape()") == "[2, 1]");
    // one-to-many merge expands; a no-match inner merge is empty
    CHECK(run("pd.merge(pd.DataFrame([[1,\"a\"],[2,\"b\"]], columns=[\"id\",\"x\"]), "
              "pd.DataFrame([[1,\"p\"],[1,\"q\"],[2,\"r\"]], columns=[\"id\",\"y\"]), on=\"id\").shape()") == "[3, 3]");
    CHECK(run("len(pd.merge(pd.DataFrame([[1]], columns=[\"id\"]), pd.DataFrame([[9]], columns=[\"id\"]), on=\"id\"))") == "0");
    // sortvalues is stable on ties
    CHECK(run("pd.DataFrame([[1,\"a\"],[1,\"b\"],[1,\"c\"]], columns=[\"k\",\"t\"]).sortvalues(\"k\")[\"t\"].tolist()") == "['a', 'b', 'c']");

    // --- adversarial: malformed input and out-of-domain operations throw (never crash) ---
    {
        KiritoVM vm;
        auto bad = [&](const std::string& expr) {
            CHECK_THROWS(vm.runSource("var pd = import(\"tabular\")\n" + expr));
        };
        bad("pd.DataFrame({\"a\": [1, 2], \"b\": [1]}, columns=[\"a\", \"b\"])");   // ragged columns
        bad("pd.Series([1, 2, 3], index=[\"a\"])");                                 // index length mismatch
        bad("pd.DataFrame({\"a\": [1]}, columns=[\"a\"])[\"nope\"]");               // missing column
        bad("pd.Series([1, 2, 3]) + pd.Series([1, 2])");                            // Series length mismatch
        bad("pd.DataFrame({\"a\": [1]}, columns=[\"a\"]).iloc[5]");                 // row out of range
        bad("pd.DataFrame({\"a\": [1]}, columns=[\"a\"]).groupby(\"nope\")");       // group by missing column
        bad("pd.DataFrame({\"k\": [\"x\"], \"v\": [1]}, columns=[\"k\", \"v\"]).groupby(\"k\").agg({\"v\": \"bogus\"})");  // unknown reduction
    }

    return RUN_TESTS();
}
