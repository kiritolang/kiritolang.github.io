#ifndef KIRITO_VERSION_HPP
#define KIRITO_VERSION_HPP

namespace kirito {

// The Kirito release version (semantic versioning, "MAJOR.MINOR.PATCH"). Releases are git-tagged
// with the BARE version (e.g. `1.6.2`; a leading `v` is tolerated by the `semver` module / kpm).
// This constant is the source of truth, surfaced as `ki --version` and the `sys.version` module
// value so Kirito programs (notably `kpm`) can read it and decide whether a newer interpreter is
// available. Bump it IN THE SAME COMMIT you tag, so the published binary's embedded version matches
// its release. Release binaries are built locally with `tools/scripts/build_all.sh` and uploaded to
// the GitHub Release by hand (this project does not use CI).
inline constexpr const char* kVersion = "1.14.1";

}  // namespace kirito

#endif
