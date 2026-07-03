#ifndef KIRITO_CLI_PATHS_HPP
#define KIRITO_CLI_PATHS_HPP

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// Import-path discovery for the `ki` CLI: the directories contributed by the environment, on top of
// the current directory, any --lib flags, and the running script's directory. Kept header-only and
// dependency-free (it touches only the filesystem) so it is unit-testable in isolation, and so the
// embeddable VM core stays free of any opinion about where a user keeps installed packages.

namespace kirito {

// Split a PATH-style env value (e.g. KIRITO_PATH) on the platform separator, dropping empties.
inline std::vector<std::string> splitPathList(const std::string& value, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : value) {
        if (c == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The per-user package directory where `kpm` installs packages: <home>/.kirito/packages.
inline std::string userPackagesDir(const std::string& home) {
    if (home.empty()) return {};
    return (std::filesystem::path(home) / ".kirito" / "packages").string();
}

// Directories the CLI adds to the module import path from the environment, in search order:
//   1. every directory listed in KIRITO_PATH (separator-split),
//   2. the user packages directory itself, and
//   3. each immediate sub-directory of it — so a package installed by `kpm` as
//      <packages>/<name>/<name>.ki is importable simply as import("name").
// Only directories that exist contribute their sub-directories; the packages root is always listed
// (harmless if absent — imports just won't resolve there).
inline std::vector<std::string> environmentLibPaths(const std::string& kiritoPath,
                                                    const std::string& home, char sep) {
    std::vector<std::string> out = splitPathList(kiritoPath, sep);
    std::string pkgs = userPackagesDir(home);
    if (pkgs.empty()) return out;
    out.push_back(pkgs);
    std::error_code ec;
    if (std::filesystem::is_directory(pkgs, ec)) {
        std::vector<std::string> subs;
        for (const auto& e : std::filesystem::directory_iterator(pkgs, ec))
            if (e.is_directory(ec)) subs.push_back(e.path().string());
        std::sort(subs.begin(), subs.end());  // stable, deterministic order
        for (auto& s : subs) out.push_back(std::move(s));
    }
    return out;
}

}  // namespace kirito

#endif
