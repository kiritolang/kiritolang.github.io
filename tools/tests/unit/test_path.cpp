// test_path.cpp — the `path` module (Kirito's os.path) exercised from the C++/embedding side in a
// bare KiritoVM: pure string ops (join/dirname/basename/splitext) and filesystem queries
// (exists/isfile/isdir/getsize), correct output + edge cases + adversarial/error inputs. Complements
// the pure-Kirito tools/tests/scripts/path_module.ki. Also pins the io/sys -> path move.
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

    // ---- join: correct output ----
    CHECK(evalStr(vm, P + "path.join(\"a\", \"b\", \"c\")") == "a/b/c");
    CHECK(evalStr(vm, P + "path.join(\"a\")") == "a");
    CHECK(evalStr(vm, P + "path.join(\"/usr\", \"local\", \"bin\")") == "/usr/local/bin");
    CHECK(evalStr(vm, P + "path.join(\"a\", \"/b\", \"c\")") == "/b/c");         // absolute resets
    CHECK(evalStr(vm, P + "path.join(\"a/\", \"b\")") == "a/b");                 // no double sep
    CHECK(evalStr(vm, P + "path.join(\"a\", \"\", \"b\")") == "a/b");            // empty middle
    CHECK(evalStr(vm, P + "path.join(\"a\", \"\")") == "a/");                    // trailing empty
    CHECK(evalStr(vm, P + "path.join(\"\", \"\", \"\")") == "");                 // all empty
    CHECK(evalStr(vm, P + "path.join(\"a\", \"\\\\b\")") == "a/\\b");            // backslash NOT absolute

    // ---- join: adversarial / errors ----
    CHECK_THROWS(evalStr(vm, P + "path.join()"));                                // needs >= 1 part
    CHECK_THROWS(evalStr(vm, P + "path.join(\"a\", 5)"));                        // non-String part
    CHECK_THROWS(evalStr(vm, P + "path.join(None)"));

    // ---- dirname ----
    CHECK(evalStr(vm, P + "path.dirname(\"a/b/c\")") == "a/b");
    CHECK(evalStr(vm, P + "path.dirname(\"file\")") == "");
    CHECK(evalStr(vm, P + "path.dirname(\"\")") == "");
    CHECK(evalStr(vm, P + "path.dirname(\"a/b/\")") == "a/b");
    CHECK(evalStr(vm, P + "path.dirname(\"/\")") == "/");
    CHECK(evalStr(vm, P + "path.dirname(\"/a\")") == "/");
    CHECK(evalStr(vm, P + "path.dirname(\"//a\")") == "//");
    CHECK(evalStr(vm, P + "path.dirname(\"a/b\\\\c\")") == "a/b");               // last of / or backslash

    // ---- basename ----
    CHECK(evalStr(vm, P + "path.basename(\"a/b/c\")") == "c");
    CHECK(evalStr(vm, P + "path.basename(\"/abs/f.txt\")") == "f.txt");
    CHECK(evalStr(vm, P + "path.basename(\"file\")") == "file");
    CHECK(evalStr(vm, P + "path.basename(\"a/b/\")") == "");
    CHECK(evalStr(vm, P + "path.basename(\"/\")") == "");
    CHECK(evalStr(vm, P + "path.basename(\"\\\\a\\\\b\")") == "b");

    // ---- splitext ----
    CHECK(evalStr(vm, P + "path.splitext(\"a.txt\")") == "['a', '.txt']");
    CHECK(evalStr(vm, P + "path.splitext(\"archive.tar.gz\")") == "['archive.tar', '.gz']");
    CHECK(evalStr(vm, P + "path.splitext(\"noext\")") == "['noext', '']");
    CHECK(evalStr(vm, P + "path.splitext(\".bashrc\")") == "['.bashrc', '']");
    CHECK(evalStr(vm, P + "path.splitext(\"..\")") == "['..', '']");
    CHECK(evalStr(vm, P + "path.splitext(\"file.\")") == "['file', '.']");
    CHECK(evalStr(vm, P + "path.splitext(\"dir.x/file\")") == "['dir.x/file', '']");

    // ---- string-op type errors ----
    CHECK_THROWS(evalStr(vm, P + "path.dirname(5)"));
    CHECK_THROWS(evalStr(vm, P + "path.basename(None)"));
    CHECK_THROWS(evalStr(vm, P + "path.splitext([\"a\"])"));

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
    CHECK_THROWS(evalStr(vm, base + "path.exists(5)"));

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

    // ---- inspect(path) describes the module (incl. the moved fs ops + their kwargs) ----
    std::string ins = evalStr(vm, P + "inspect(path)");
    CHECK(ins.find("module path:") != std::string::npos);
    CHECK(ins.find("join(...)") != std::string::npos);
    CHECK(ins.find("dirname(path: String) -> String") != std::string::npos);
    CHECK(ins.find("getsize(path: String) -> Integer") != std::string::npos);
    CHECK(ins.find("mkdir(path: String, exist_ok: Bool = False)") != std::string::npos);
    CHECK(ins.find("remove(path: String, missing_ok: Bool = False)") != std::string::npos);
    CHECK(ins.find("rmtree(path: String, missing_ok: Bool = False)") != std::string::npos);
    CHECK(ins.find("listdir(path: String) -> List") != std::string::npos);

    // ---- the split is real: all path/filesystem ops moved OUT of io / sys ----
    CHECK_THROWS(evalStr(vm, "import(\"io\").dirname(\"/a/b\")"));
    CHECK_THROWS(evalStr(vm, "import(\"io\").exists(\".\")"));
    CHECK_THROWS(evalStr(vm, "import(\"io\").mkdir(\".\")"));
    CHECK_THROWS(evalStr(vm, "import(\"io\").remove(\".\")"));
    CHECK_THROWS(evalStr(vm, "import(\"io\").getcwd()"));
    CHECK_THROWS(evalStr(vm, "import(\"io\").listdir(\".\")"));
    CHECK_THROWS(evalStr(vm, "import(\"sys\").joinpath(\"a\", \"b\")"));
    // io keeps only actual I/O
    CHECK(evalStr(vm, "type(import(\"path\").getcwd())") == "String");

    // gettempdir + executable are filesystem locations, so they live in `path` (moved from `sys`).
    CHECK(evalStr(vm, "import(\"path\").isdir(import(\"path\").gettempdir())") == "True");
    CHECK(evalStr(vm, "len(import(\"path\").gettempdir()) > 0") == "True");
    // fasttemp(): the fastest available scratch dir — RAM tmpfs (/dev/shm) on Linux, else gettempdir.
    // Whatever it resolves to must be an existing, non-empty directory path (portable, best-effort).
    CHECK(evalStr(vm, "var p = import(\"path\")\np.isdir(p.fasttemp()) and len(p.fasttemp()) > 0") == "True");
    CHECK(evalStr(vm, "type(import(\"path\").fasttemp()) == \"String\"") == "True");
    // it is a usable scratch location: build a path under it, write, read the size back, remove.
    CHECK(evalStr(vm, "var p = import(\"path\")\nvar io = import(\"io\")\n"
                      "var f = p.join(p.fasttemp(), \"kirito_fasttemp_cpp.txt\")\n"
                      "var h = io.open(f, \"w\")\ndiscard h.write(\"xyz\")\nh.close()\n"
                      "var okw = p.exists(f) and p.getsize(f) == 3\ndiscard p.remove(f, missing_ok = True)\nokw") == "True");
    CHECK(evalStr(vm, "type(import(\"path\").executable) == \"String\" and len(import(\"path\").executable) > 0") == "True");
    // ...and no longer in sys.
    CHECK(evalStr(vm, "hasattr(import(\"sys\"), \"gettempdir\") or hasattr(import(\"sys\"), \"executable\")") == "False");

    return RUN_TESTS();
}
