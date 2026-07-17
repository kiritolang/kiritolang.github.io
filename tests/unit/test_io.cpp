#include <filesystem>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string evalStr(KiritoVM& vm, const std::string& src) {
    return vm.stringify(vm.runSource(src));
}

int main() {
    KiritoVM vm;
    auto dir = std::filesystem::temp_directory_path() / "kirito_io_test";
    std::filesystem::remove_all(dir);
    std::string base = "var io = import(\"io\")\nvar path = import(\"path\")\nvar dir = \"" + dir.string() + "\"\n";

    // mkdir + writelines + line iteration
    CHECK(evalStr(vm, base + R"(
path.mkdir(dir, exist_ok = True)
with io.open(dir + "/a.txt", "w") as f:
    f.writelines(["one\n", "two\n", "three\n"])
var n = 0
with io.open(dir + "/a.txt", "r") as f:
    for line in f:
        n = n + 1
n
)") == "3");

    // readlines
    CHECK(evalStr(vm, base + R"(
with io.open(dir + "/a.txt", "r") as f:
    var lines = f.readlines()
len(lines)
)") == "3");

    // seek / tell
    CHECK(evalStr(vm, base + R"(
var g = io.open(dir + "/a.txt", "r")
var start = g.tell()
g.seek(4)
var pos = g.tell()
g.close()
String(start) + "," + String(pos)
)") == "0,4");

    // append mode
    CHECK(evalStr(vm, base + R"(
with io.open(dir + "/a.txt", "a") as f:
    f.write("four\n")
var c = 0
with io.open(dir + "/a.txt", "r") as f:
    for line in f:
        c = c + 1
c
)") == "4");

    // filesystem helpers all live in `path` now: path.exists (query) + path.listdir / path.rename / path.remove (mutation)
    CHECK(evalStr(vm, base + "path.exists(dir + \"/a.txt\")") == "True");
    CHECK(evalStr(vm, base + "path.exists(dir + \"/missing.txt\")") == "False");
    CHECK(evalStr(vm, base + "\"a.txt\" in path.listdir(dir)") == "True");
    CHECK(evalStr(vm, base + R"(
path.rename(dir + "/a.txt", dir + "/b.txt")
path.exists(dir + "/b.txt") and not path.exists(dir + "/a.txt")
)") == "True");
    CHECK(evalStr(vm, base + R"(
path.remove(dir + "/b.txt")
path.exists(dir + "/b.txt")
)") == "False");

    // --- chmod: set POSIX permission bits from an octal Integer; verify the exact bits land ---
    {
        std::filesystem::create_directories(dir);
        std::string f = (dir / "perm.txt").string();
        std::string pbase = base + "var p = \"" + f + "\"\n";
        namespace fs = std::filesystem;
        // create the file, then chmod 0o600 -> rw------- ; returns success
        CHECK(evalStr(vm, pbase + R"(
with io.open(p, "w") as fh:
    fh.write("x")
path.chmod(p, 0o600)
)") == "True");
        auto perm = fs::status(f).permissions();
        CHECK((perm & fs::perms::owner_all) == (fs::perms::owner_read | fs::perms::owner_write));
        CHECK((perm & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        // bump to 0o755 -> rwxr-xr-x ; the keyword-argument form works too (chmod is signatured)
        CHECK(evalStr(vm, pbase + "path.chmod(path = p, mode = 0o755)") == "True");
        perm = fs::status(f).permissions();
        CHECK((perm & fs::perms::owner_all) == fs::perms::owner_all);
        CHECK((perm & fs::perms::group_exec) == fs::perms::group_exec);
        CHECK((perm & fs::perms::others_write) == fs::perms::none);
        // a missing path throws nothing and reports failure as False
        CHECK(evalStr(vm, base + "path.chmod(dir + \"/nope.txt\", 0o644)") == "False");
    }

    // --- the stream= keyword on print / write / eprint / input / read ---
    CHECK(evalStr(vm, base + R"(
var buf = io.BytesIO()
io.print("hi", 7, stream=buf)
io.write("raw", stream=buf)
io.eprint("e", stream=buf)
buf.getvalue()
)") == "hi 7\nrawe\n");
    CHECK(evalStr(vm, base + R"(
var src = io.BytesIO("one\ntwo\nrest")
io.input(stream=src) + "|" + io.read(3, stream=src) + "|" + io.read(stream=src)
)") == "one|two|\nrest");
    // no stream= still writes to the (redirected) stdout
    CHECK(evalStr(vm, base + R"(
var cap = io.BytesIO()
io.stdout = cap
io.print("x", stream=io.BytesIO())   # to a throwaway buffer, not cap
io.print("y")                        # to cap
io.stdout = io.__stdout__
cap.getvalue()
)") == "y\n");
    // unknown keyword and non-stream target both throw
    CHECK_THROWS(vm.runSource("import(\"io\").print(\"x\", bogus=1)\n"));
    CHECK_THROWS(vm.runSource("import(\"io\").print(\"x\", stream=42)\n"));

    std::filesystem::remove_all(dir);
    return RUN_TESTS();
}
