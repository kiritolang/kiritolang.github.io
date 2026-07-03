// Tests kpm's pure (network-free) helper logic by importing kpm.ki as a module. Its command dispatch
// is guarded by `if argmain:`, so importing only DEFINES the helpers (no commands run, no network).
// The semantic-versioning core kpm builds on is covered exhaustively by test_semver; resolveRef's
// network path (tags -> maxsatisfying) is exercised against the live GitHub API only in real use.
//
// The kpm/ directory is supplied at runtime via $KPM_DIR (set by CTest), put on the import path so
// `import("kpm")` resolves kpm.ki — passed by env, not a -D define, to keep the shared PCH reusable.
#include <cstdlib>
#include <string>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static std::string kpmDir() {
    const char* d = std::getenv("KPM_DIR");
    return d && *d ? std::string(d) : std::string(".");
}

// Evaluate an expression against a freshly-imported `kpm`, returning the stringified result.
static std::string ev(const std::string& expr) {
    KiritoVM vm;
    vm.addLibPath(kpmDir());
    return vm.stringify(vm.runSource("var kpm = import(\"kpm\")\n" + expr + "\n"));
}

// Read a field of parseSource(spec).
static std::string ps(const std::string& spec, const std::string& field) {
    return ev("kpm.parseSource(\"" + spec + "\")[\"" + field + "\"]");
}

int main() {
    // ---------- parseSource: host detection (GitHub default) + owner/repo/ref split ----------
    CHECK(ps("owner/repo", "kind") == "github");
    CHECK(ps("owner/repo", "host") == "github.com");
    CHECK(ps("owner/repo", "path") == "owner/repo");
    CHECK(ev("String(kpm.parseSource(\"owner/repo\")[\"ref\"])") == "None");
    CHECK(ps("owner/repo@main", "ref") == "main");
    CHECK(ps("o/r@^1.2.0", "ref") == "^1.2.0");
    CHECK(ps("o/r@feature/x", "ref") == "feature/x");        // a slash in the ref is kept
    CHECK(ps("github.com/owner/repo", "kind") == "github");
    CHECK(ps("gitlab.com/owner/repo", "kind") == "gitlab");
    CHECK(ps("gitlab.com/owner/repo", "host") == "gitlab.com");
    CHECK(ps("gitlab:owner/repo", "kind") == "gitlab");      // shorthand
    CHECK(ps("https://gitlab.com/owner/repo@v2.0.0", "kind") == "gitlab");
    CHECK(ps("https://gitlab.com/owner/repo@v2.0.0", "ref") == "v2.0.0");
    CHECK(ps("https://github.com/owner/repo.git", "path") == "owner/repo");  // .git stripped
    CHECK(ps("gitlab+https://git.acme.com/group/sub/repo@main", "kind") == "gitlab");
    CHECK(ps("gitlab+https://git.acme.com/group/sub/repo@main", "host") == "git.acme.com");
    CHECK(ps("gitlab+https://git.acme.com/group/sub/repo@main", "path") == "group/sub/repo");  // nested group
    CHECK(ps("gitlab+http://localhost:8080/o/r", "apibase") == "http://localhost:8080/api/v4");
    {
        KiritoVM vm;
        vm.addLibPath(kpmDir());
        CHECK_THROWS(vm.runSource("import(\"kpm\").parseSource(\"norepo\")\n"));            // missing '/'
        CHECK_THROWS(vm.runSource("import(\"kpm\").parseSource(\"https://x.invalid/o/r\")\n")); // unknown host
        CHECK_THROWS(vm.runSource("import(\"kpm\").parseSource(\"\")\n"));                   // empty
    }

    // ---------- safeRelPath: blocks absolute paths, drive letters, and . / .. traversal ----------
    CHECK(ev("kpm.safeRelPath(\"a/b.ki\")") == "True");
    CHECK(ev("kpm.safeRelPath(\"mod.ki\")") == "True");
    CHECK(ev("kpm.safeRelPath(\"../escape.ki\")") == "False");
    CHECK(ev("kpm.safeRelPath(\"a/../../x\")") == "False");
    CHECK(ev("kpm.safeRelPath(\"/etc/passwd\")") == "False");
    CHECK(ev("kpm.safeRelPath(\"a/./b\")") == "False");
    CHECK(ev("kpm.safeRelPath(\"\")") == "False");
    CHECK(ev("kpm.safeRelPath(\"a//b\")") == "False");       // empty component
    {
        KiritoVM vm; vm.addLibPath(kpmDir());
        CHECK(vm.stringify(vm.runSource("import(\"kpm\").safeRelPath(\"C:\\\\win\")")) == "False");  // drive
        CHECK(vm.stringify(vm.runSource("import(\"kpm\").safeRelPath(\"..\\\\x\")")) == "False");    // backslash ..
    }

    // ---------- validateManifest: returns [name, version, modules, deps]; throws on bad/unsafe ----------
    CHECK(ev("kpm.validateManifest({\"name\": \"p\", \"version\": \"1.2.3\", \"modules\": [\"m.ki\"]}, \"w\")[0]") == "p");
    CHECK(ev("kpm.validateManifest({\"name\": \"p\", \"version\": \"1.2.3\", \"modules\": [\"m.ki\"]}, \"w\")[1]") == "1.2.3");
    CHECK(ev("kpm.validateManifest({\"name\": \"p\", \"modules\": [\"m.ki\"]}, \"w\")[1]") == "0.0.0");  // version defaults
    {
        KiritoVM vm; vm.addLibPath(kpmDir());
        // missing name / missing modules / empty modules / unsafe module / bad name / non-list deps
        CHECK_THROWS(vm.runSource("import(\"kpm\").validateManifest({\"modules\": [\"m.ki\"]}, \"w\")\n"));
        CHECK_THROWS(vm.runSource("import(\"kpm\").validateManifest({\"name\": \"p\"}, \"w\")\n"));
        CHECK_THROWS(vm.runSource("import(\"kpm\").validateManifest({\"name\": \"p\", \"modules\": []}, \"w\")\n"));
        CHECK_THROWS(vm.runSource("import(\"kpm\").validateManifest({\"name\": \"p\", \"modules\": [\"../x.ki\"]}, \"w\")\n"));
        CHECK_THROWS(vm.runSource("import(\"kpm\").validateManifest({\"name\": \"../bad\", \"modules\": [\"m.ki\"]}, \"w\")\n"));
        CHECK_THROWS(vm.runSource("import(\"kpm\").validateManifest({\"name\": \"p\", \"modules\": [\"m.ki\"], \"dependencies\": \"x\"}, \"w\")\n"));
    }

    // ---------- extractKpmVersion: pulls KPM_VERSION out of source text; None when absent ----------
    CHECK(ev("kpm.extractKpmVersion(\"var KPM_VERSION = \\\"9.9.9\\\"\\n\")") == "9.9.9");
    CHECK(ev("kpm.extractKpmVersion(\"no version here\")") == "None");
    // the KPM_VERSION constant is itself a valid semver (so self-update can compare against it)
    CHECK(ev("var v = import(\"semver\")\nv.valid(kpm.KPM_VERSION) != None") == "True");

    // ---------- kiAssetName: the release-asset filename for this platform ----------
    CHECK(ev("kpm.kiAssetName().startswith(\"ki-\")") == "True");
    CHECK(ev("\"x64\" in kpm.kiAssetName()") == "True");  // CI toolchains are x64

    // ---------- hasFlag ----------
    CHECK(ev("kpm.hasFlag([\"a\", \"--force\"], \"--force\")") == "True");
    CHECK(ev("kpm.hasFlag([\"a\", \"-f\"], \"--force\")") == "False");
    CHECK(ev("kpm.hasFlag([], \"--force\")") == "False");

    // ---------- ghHeaders: Accept always; Authorization only when a token is configured ----------
    CHECK(ev("kpm.ghHeaders()[\"Accept\"]") == "application/vnd.github+json");
    // explicitly clear any ambient token -> no Authorization header
    CHECK(ev("var sys = import(\"sys\")\nsys.unsetenv(\"GITHUB_TOKEN\")\n"
             "sys.unsetenv(\"KPM_GITHUB_TOKEN\")\n\"Authorization\" in kpm.ghHeaders()") == "False");
    // with a token set, the bearer header appears
    CHECK(ev("var sys = import(\"sys\")\nsys.setenv(\"KPM_GITHUB_TOKEN\", \"tok123\")\n"
             "kpm.ghHeaders()[\"Authorization\"]") == "Bearer tok123");

    // ---------- srcHeaders: GitLab uses PRIVATE-TOKEN; GitHub a bearer ----------
    CHECK(ev("var sys = import(\"sys\")\nsys.setenv(\"KPM_GITLAB_TOKEN\", \"gl9\")\n"
             "kpm.srcHeaders(kpm.parseSource(\"gitlab.com/o/r\"))[\"PRIVATE-TOKEN\"]") == "gl9");
    CHECK(ev("var sys = import(\"sys\")\nsys.unsetenv(\"GITHUB_TOKEN\")\nsys.unsetenv(\"KPM_GITHUB_TOKEN\")\n"
             "\"Accept\" in kpm.srcHeaders(kpm.parseSource(\"o/r\"))") == "True");

    return RUN_TESTS();
}
