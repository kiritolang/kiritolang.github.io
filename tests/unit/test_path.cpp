// test_path.cpp — the `path` module (Kirito's os.path) exercised from the C++/embedding side in a
// bare KiritoVM: filesystem queries (exists/isfile/isdir/getsize) and mutation (mkdir/remove/
// rmtree) against a real temp file/dir, correct output + edge cases + adversarial/error inputs.
// (The pure string-op cases — join/dirname/basename/splitext, their throws, inspect(path)'s
// surface, the io/sys -> path split, and the read-only gettempdir/fasttemp/executable checks —
// were converted to tests/scripts/unit_path.ki; only the "usable scratch location" write-based
// fasttemp() case stays here.) Complements the pure-Kirito tools/tests/scripts/path_module.ki.
#include <filesystem>
#include <fstream>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;
    const std::string P = "var path = import(\"path\")\n";

    // ---- filesystem queries against a real temp file + dir ----
    auto dir = std::filesystem::temp_directory_path() / "kirito_test_path_cpp";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto file = dir / "probe.txt";
    { std::ofstream f(file); f << "abcdef"; }   // 6 bytes
    const std::string base =
        P + "var f = \"" + file.string() + "\"\nvar d = \"" + dir.string() + "\"\n";

    CHECK(evalStr(vm, base + "path.exists(f)") == "True");
    CHECK(evalStr(vm, base + "path.exists(d)") == "True");
    CHECK(evalStr(vm, base + "path.isfile(f)") == "True");
    CHECK(evalStr(vm, base + "path.isfile(d)") == "False");
    CHECK(evalStr(vm, base + "path.isdir(d)") == "True");
    CHECK(evalStr(vm, base + "path.isdir(f)") == "False");
    CHECK(evalStr(vm, base + "path.getsize(f)") == "6");
    CHECK(evalStr(vm, base + "type(path.getsize(f))") == "Integer");

    // tolerant on a missing path (False, never a throw)
    CHECK(evalStr(vm, base + "path.exists(d + \"/nope_zzz\")") == "False");
    CHECK(evalStr(vm, base + "path.isfile(d + \"/nope_zzz\")") == "False");
    CHECK(evalStr(vm, base + "path.isdir(d + \"/nope_zzz\")") == "False");

    // getsize: adversarial (missing / a directory) throws
    CHECK_THROWS(evalStr(vm, base + "path.getsize(d + \"/nope_zzz\")"));
    CHECK_THROWS(evalStr(vm, base + "path.getsize(d)"));

    std::filesystem::remove_all(dir);

    // ---- filesystem mutation: strict by default + opt-in leniency ----
    auto mk = dir.string();   // a fresh scratch dir (removed above)
    const std::string fb = P + "var io = import(\"io\")\nvar d = \"" + mk + "\"\n";
    CHECK(evalStr(vm, fb + "path.mkdir(d)") == "True");               // creates -> True
    CHECK(evalStr(vm, fb + "path.isdir(d)") == "True");
    CHECK_THROWS(evalStr(vm, fb + "path.mkdir(d)"));                  // existing -> throws
    CHECK(evalStr(vm, fb + "path.mkdir(d, exist_ok = True)") == "False");  // existing + exist_ok -> False
    CHECK(evalStr(vm, fb + "var f = d + \"/x.txt\"\nio.open(f, \"w\").close()\npath.remove(f)") == "True");
    CHECK_THROWS(evalStr(vm, fb + "path.remove(d + \"/x.txt\")"));    // missing -> throws
    CHECK(evalStr(vm, fb + "path.remove(d + \"/x.txt\", missing_ok = True)") == "False");
    CHECK_THROWS(evalStr(vm, fb + "io.open(d + \"/y.txt\", \"w\").close()\npath.remove(d)"));  // non-empty dir throws
    CHECK(evalStr(vm, fb + "path.rmtree(d)") == "True");           // recursive delete
    CHECK(evalStr(vm, fb + "path.exists(d)") == "False");
    CHECK_THROWS(evalStr(vm, fb + "path.rmtree(d)"));              // missing -> throws
    CHECK(evalStr(vm, fb + "path.rmtree(d, missing_ok = True)") == "False");
    std::filesystem::remove_all(dir);

    // fasttemp() is a usable scratch location: build a path under it, write, read the size back,
    // remove. (fasttemp()'s own isdir/type/read-only checks were converted to the .ki golden.)
    CHECK(evalStr(vm, "var p = import(\"path\")\nvar io = import(\"io\")\n"
                      "var f = p.join(p.fasttemp(), \"kirito_fasttemp_cpp.txt\")\n"
                      "var h = io.open(f, \"w\")\ndiscard h.write(\"xyz\")\nh.close()\n"
                      "var okw = p.exists(f) and p.getsize(f) == 3\ndiscard p.remove(f, missing_ok = True)\nokw") == "True");

    return RUN_TESTS();
}
