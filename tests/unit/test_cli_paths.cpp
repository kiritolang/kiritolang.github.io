#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito/cli_paths.hpp"

using namespace kirito;
namespace fs = std::filesystem;

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

int main() {
    // --- splitPathList ---
    CHECK(splitPathList("", ':').empty());
    CHECK(splitPathList("a", ':') == (std::vector<std::string>{"a"}));
    CHECK(splitPathList("a:b:c", ':') == (std::vector<std::string>{"a", "b", "c"}));
    CHECK(splitPathList("a::b", ':') == (std::vector<std::string>{"a", "b"}));   // empties dropped
    CHECK(splitPathList(":a:", ':') == (std::vector<std::string>{"a"}));          // leading/trailing
    CHECK(splitPathList("a;b", ';') == (std::vector<std::string>{"a", "b"}));     // Windows separator

    // --- userPackagesDir ---
    CHECK(userPackagesDir("").empty());
    CHECK(userPackagesDir("/home/u") == (fs::path("/home/u") / ".kirito" / "packages").string());

    // --- environmentLibPaths: KIRITO_PATH dirs come first, then the packages dir + its sub-dirs ---
    fs::path tmp = fs::temp_directory_path() / fs::path("kirito_cli_paths_test");
    fs::remove_all(tmp);
    fs::path pkgs = tmp / ".kirito" / "packages";
    fs::create_directories(pkgs / "alpha");
    fs::create_directories(pkgs / "beta");
    { std::ofstream(pkgs / "loose.ki") << "x\n"; }  // a file, must be ignored (not a dir)

    auto paths = environmentLibPaths("/extra:/more", tmp.string(), ':');
    CHECK(has(paths, "/extra"));
    CHECK(has(paths, "/more"));
    CHECK(has(paths, pkgs.string()));               // the packages root itself
    CHECK(has(paths, (pkgs / "alpha").string()));    // each package sub-directory
    CHECK(has(paths, (pkgs / "beta").string()));
    CHECK(!has(paths, (pkgs / "loose.ki").string())); // the loose file is not on the path
    // KIRITO_PATH entries precede the packages root.
    auto idxExtra = std::find(paths.begin(), paths.end(), std::string("/extra"));
    auto idxPkgs = std::find(paths.begin(), paths.end(), pkgs.string());
    CHECK(idxExtra < idxPkgs);

    // --- a home with no packages dir yet still lists the (absent) root, no sub-dirs ---
    fs::path empty = fs::temp_directory_path() / fs::path("kirito_cli_paths_empty");
    fs::remove_all(empty);
    auto p2 = environmentLibPaths("", empty.string(), ':');
    CHECK(p2.size() == 1);
    CHECK(p2[0] == (empty / ".kirito" / "packages").string());

    fs::remove_all(tmp);
    fs::remove_all(empty);
    return RUN_TESTS();
}
