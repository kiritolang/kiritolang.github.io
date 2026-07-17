// CLI argument handling tests. Drives the built `ki` binary as a subprocess with various argv
// configurations and verifies argv-ownership semantics: `--lib X` BEFORE the script file registers
// X as an import path, but the same `--lib Y` AFTER the script file belongs to the script's
// arglist; `arglist` / `argmain` reflect the run mode; environment variables are honoured.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../check.hpp"

namespace fs = std::filesystem;

namespace {

const char* g_ki = nullptr;
fs::path g_tmpRoot;

// Run a command and capture stdout. Returns (exit_status, captured stdout). Stderr is sent to
// /dev/null to keep deterministic check output (some probes intentionally exercise stderr paths).
struct CmdResult { int status; std::string out; };
CmdResult runCmd(const std::string& cmd) {
    std::string full = cmd + " 2>/dev/null";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    std::array<char, 4096> buf;
    while (auto n = std::fread(buf.data(), 1, buf.size(), p)) out.append(buf.data(), n);
    int st = pclose(p);
    return {st, out};
}
// Run + also capture stderr separately, for tests that probe stderr behaviour.
CmdResult runCmdErr(const std::string& cmd) {
    std::string full = cmd + " 2>&1";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    std::array<char, 4096> buf;
    while (auto n = std::fread(buf.data(), 1, buf.size(), p)) out.append(buf.data(), n);
    int st = pclose(p);
    return {st, out};
}

void writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

std::string q(const fs::path& p) { return "\"" + p.string() + "\""; }
std::string q(const std::string& s) {
    std::string out = "\"";
    for (char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out + "\"";
}

}  // anon

int main(int argc, char** argv) {
    g_ki = (argc >= 2) ? argv[1] : "./build-release/ki";
    g_tmpRoot = fs::temp_directory_path() / "kirito_cli_tests";
    fs::remove_all(g_tmpRoot);
    fs::create_directories(g_tmpRoot);

    // ---- BASELINE: --version, --help, unknown option ----
    {
        auto r = runCmd(std::string(g_ki) + " --version");
        CHECK(r.status == 0);
        CHECK(r.out.find("ki (Kirito)") != std::string::npos);

        auto r2 = runCmd(std::string(g_ki) + " -v");
        CHECK(r2.status == 0);
        CHECK(r2.out == r.out);  // -v is an alias

        auto h = runCmd(std::string(g_ki) + " --help");
        CHECK(h.status == 0);
        CHECK(h.out.find("--lib") != std::string::npos);
        CHECK(h.out.find("--version") != std::string::npos);

        auto bad = runCmdErr(std::string(g_ki) + " --bogus");
        CHECK(bad.status != 0);
        CHECK(bad.out.find("unknown option") != std::string::npos);

        auto missingFile = runCmdErr(std::string(g_ki) + " " + q(g_tmpRoot / "missing.ki"));
        CHECK(missingFile.status != 0);
        CHECK(missingFile.out.find("cannot open") != std::string::npos);
    }

    // ---- arglist / argmain: a directly-run script gets its args, an imported module does NOT ----
    {
        fs::path main = g_tmpRoot / "argprobe.ki";
        writeFile(main,
            "var io = import(\"io\")\n"
            "io.print(\"argmain=\", argmain)\n"
            "io.print(\"arglist=\", arglist)\n"
            "io.print(\"len=\", len(arglist))\n");
        auto r = runCmd(std::string(g_ki) + " " + q(main) + " hello 42 \"a b\"");
        CHECK(r.status == 0);
        CHECK(r.out.find("argmain= True") != std::string::npos);
        CHECK(r.out.find("len= 3") != std::string::npos);
        CHECK(r.out.find("hello") != std::string::npos);
        CHECK(r.out.find("a b") != std::string::npos);

        // arglist is empty when no extra args
        auto r2 = runCmd(std::string(g_ki) + " " + q(main));
        CHECK(r2.status == 0);
        CHECK(r2.out.find("len= 0") != std::string::npos);
    }

    // ---- CORE TEST: --lib BEFORE script registers it; --lib AFTER belongs to the script. ----
    // The user-reported case: `ki --lib libdir1 main.ki --lib libdir2`. Only libdir1 is a real lib
    // path; libdir2 is a literal token passed to main.ki.
    {
        fs::path lib1 = g_tmpRoot / "lib1";
        fs::path lib2 = g_tmpRoot / "lib2";
        fs::create_directories(lib1);
        fs::create_directories(lib2);
        writeFile(lib1 / "marker_lib1.ki", "var probe = \"from-lib1\"\n");
        writeFile(lib2 / "marker_lib2.ki", "var probe = \"from-lib2\"\n");
        fs::path main = g_tmpRoot / "splitargs.ki";
        writeFile(main,
            "var io = import(\"io\")\n"
            "io.print(\"arglist=\", arglist)\n"
            "var m1 = import(\"marker_lib1\")\n"
            "io.print(\"lib1.probe=\", m1.probe)\n"
            "var ok = False\n"
            "try:\n"
            "    var m2 = import(\"marker_lib2\")\n"
            "    io.print(\"WRONG-lib2-imported=\", m2.probe)\n"
            "catch as e:\n"
            "    ok = True\n"
            "    io.print(\"lib2-NOT-imported (good)\")\n");
        auto r = runCmd(std::string(g_ki) + " --lib " + q(lib1) + " "
                        + q(main) + " --lib " + q(lib2));
        CHECK(r.status == 0);
        // arglist must contain the literal "--lib" and the lib2 path
        CHECK(r.out.find("--lib") != std::string::npos);
        CHECK(r.out.find(lib2.string()) != std::string::npos);
        // lib1 was registered as a real import path
        CHECK(r.out.find("lib1.probe= from-lib1") != std::string::npos);
        // lib2 was NOT registered (it's a script argument), so import("marker_lib2") fails
        CHECK(r.out.find("lib2-NOT-imported (good)") != std::string::npos);
        CHECK(r.out.find("WRONG-lib2-imported") == std::string::npos);
    }

    // ---- Multiple --lib paths before the script all register ----
    {
        fs::path la = g_tmpRoot / "libA";
        fs::path lb = g_tmpRoot / "libB";
        fs::create_directories(la);
        fs::create_directories(lb);
        writeFile(la / "modA.ki", "var name = \"A\"\n");
        writeFile(lb / "modB.ki", "var name = \"B\"\n");
        fs::path main = g_tmpRoot / "multilib.ki";
        writeFile(main,
            "var io = import(\"io\")\n"
            "io.print(import(\"modA\").name, import(\"modB\").name)\n");
        auto r = runCmd(std::string(g_ki) + " --lib " + q(la) + " --lib " + q(lb) + " " + q(main));
        CHECK(r.status == 0);
        CHECK(r.out.find("A B") != std::string::npos);
    }

    // ---- --lib=<dir> equals-form is also accepted ----
    {
        fs::path lc = g_tmpRoot / "libC";
        fs::create_directories(lc);
        writeFile(lc / "modC.ki", "var v = 123\n");
        fs::path main = g_tmpRoot / "libeq.ki";
        writeFile(main, "var io = import(\"io\")\nio.print(import(\"modC\").v)\n");
        auto r = runCmd(std::string(g_ki) + " --lib=" + lc.string() + " " + q(main));
        CHECK(r.status == 0);
        CHECK(r.out.find("123") != std::string::npos);
    }

    // ---- --lib with a missing path argument errors out ----
    {
        auto r = runCmdErr(std::string(g_ki) + " --lib");
        CHECK(r.status != 0);
        CHECK(r.out.find("--lib needs a directory") != std::string::npos);
    }

    // ---- The script's own directory is automatically searched for sibling modules ----
    {
        fs::path project = g_tmpRoot / "project";
        fs::create_directories(project);
        writeFile(project / "sibling.ki", "var v = \"sib\"\n");
        writeFile(project / "main.ki",
            "var io = import(\"io\")\n"
            "io.print(import(\"sibling\").v)\n");
        auto r = runCmd(std::string(g_ki) + " " + q(project / "main.ki"));
        CHECK(r.status == 0);
        CHECK(r.out.find("sib") != std::string::npos);
    }

    // ---- argmain is False for an imported module (the standard idiom) ----
    {
        fs::path lib = g_tmpRoot / "libImport";
        fs::create_directories(lib);
        writeFile(lib / "imported.ki",
            "var io = import(\"io\")\n"
            "io.print(\"imported.argmain=\", argmain)\n"
            "io.print(\"imported.arglist=\", arglist)\n"
            "var hello = Function(): return \"hi\"\n");
        fs::path main = g_tmpRoot / "uses_imported.ki";
        writeFile(main,
            "var io = import(\"io\")\n"
            "io.print(\"main.argmain=\", argmain)\n"
            "var m = import(\"imported\")\n"
            "io.print(m.hello())\n");
        auto r = runCmd(std::string(g_ki) + " --lib " + q(lib) + " " + q(main) + " script-arg");
        CHECK(r.status == 0);
        CHECK(r.out.find("main.argmain= True") != std::string::npos);
        CHECK(r.out.find("imported.argmain= False") != std::string::npos);
        // The imported module's arglist is the empty list (it doesn't see the script's args)
        CHECK(r.out.find("imported.arglist= []") != std::string::npos);
        CHECK(r.out.find("hi") != std::string::npos);
    }

    // ---- -w / --no-warn suppresses static-analysis warnings ----
    {
        fs::path main = g_tmpRoot / "warnprobe.ki";
        // a function with an unused local triggers an analyzer warning at the variable definition
        writeFile(main,
            "var io = import(\"io\")\n"
            "var f = Function():\n"
            "    var unused = 99\n"
            "    return 1\n"
            "io.print(f())\n");
        auto with = runCmdErr(std::string(g_ki) + " " + q(main));
        CHECK(with.status == 0);
        CHECK(with.out.find("unused") != std::string::npos);  // warning text mentions the var name
        CHECK(with.out.find("1") != std::string::npos);        // program ran

        auto without = runCmdErr(std::string(g_ki) + " --no-warn " + q(main));
        CHECK(without.status == 0);
        CHECK(without.out.find("warning") == std::string::npos);
        CHECK(without.out.find("1") != std::string::npos);

        auto withoutShort = runCmdErr(std::string(g_ki) + " -w " + q(main));
        CHECK(withoutShort.out.find("warning") == std::string::npos);
    }

    // ---- KIRITO_PATH env adds import paths ----
    {
        fs::path lp = g_tmpRoot / "envlib";
        fs::create_directories(lp);
        writeFile(lp / "envmod.ki", "var marker = \"from-env\"\n");
        fs::path main = g_tmpRoot / "env_main.ki";
        writeFile(main, "var io = import(\"io\")\nio.print(import(\"envmod\").marker)\n");
        auto r = runCmd("KIRITO_PATH=" + q(lp) + " " + std::string(g_ki) + " " + q(main));
        CHECK(r.status == 0);
        CHECK(r.out.find("from-env") != std::string::npos);

        // ":"-separated multiple paths
        fs::path lp2 = g_tmpRoot / "envlib2";
        fs::create_directories(lp2);
        writeFile(lp2 / "envmod2.ki", "var v = 42\n");
        fs::path main2 = g_tmpRoot / "env_main2.ki";
        writeFile(main2,
            "var io = import(\"io\")\n"
            "io.print(import(\"envmod\").marker, import(\"envmod2\").v)\n");
        auto r2 = runCmd("KIRITO_PATH=" + q(lp.string() + ":" + lp2.string())
                         + " " + std::string(g_ki) + " " + q(main2));
        CHECK(r2.status == 0);
        CHECK(r2.out.find("from-env") != std::string::npos);
        CHECK(r2.out.find("42") != std::string::npos);
    }

    // ---- Script args preserve order, count, and leading-dash tokens ----
    {
        fs::path main = g_tmpRoot / "tokens.ki";
        writeFile(main,
            "var io = import(\"io\")\n"
            "var i = 0\n"
            "while i < len(arglist):\n"
            "    io.print(i, arglist[i])\n"
            "    i = i + 1\n");
        auto r = runCmd(std::string(g_ki) + " " + q(main)
                        + " -x --foo=bar 5 \"with spaces\" -- --after-doubledash");
        CHECK(r.status == 0);
        CHECK(r.out.find("0 -x") != std::string::npos);
        CHECK(r.out.find("1 --foo=bar") != std::string::npos);
        CHECK(r.out.find("2 5") != std::string::npos);
        CHECK(r.out.find("3 with spaces") != std::string::npos);
        CHECK(r.out.find("4 --") != std::string::npos);
        CHECK(r.out.find("5 --after-doubledash") != std::string::npos);
    }

    // ---- Empty arglist still binds (and is empty), not undefined ----
    {
        fs::path main = g_tmpRoot / "no_args.ki";
        writeFile(main,
            "var io = import(\"io\")\n"
            "io.print(type(arglist))\n"
            "io.print(len(arglist))\n");
        auto r = runCmd(std::string(g_ki) + " " + q(main));
        CHECK(r.status == 0);
        CHECK(r.out.find("List") != std::string::npos);
        CHECK(r.out.find("0") != std::string::npos);
    }

    // ---- ENV propagation: parallel.cpucount() is reachable from a script (proves the CLI builds
    //      the dispatcher; an embedded VM without a dispatcher cannot import 'parallel'). ----
    {
        fs::path main = g_tmpRoot / "cpu.ki";
        writeFile(main,
            "var io = import(\"io\")\n"
            "var p = import(\"parallel\")\n"
            "var n = p.cpucount()\n"
            "io.print(\"cpus=\", n)\n"
            "io.print(\"positive=\", n > 0)\n");
        auto r = runCmd(std::string(g_ki) + " " + q(main));
        CHECK(r.status == 0);
        CHECK(r.out.find("cpus=") != std::string::npos);
        CHECK(r.out.find("positive= True") != std::string::npos);
    }

    fs::remove_all(g_tmpRoot);
    return 0;
}
