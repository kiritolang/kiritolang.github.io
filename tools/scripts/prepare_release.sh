#!/usr/bin/env bash
# prepare_release.sh — assemble everything to publish into a single release/ directory:
#
#   release/ki-linux-x64        the shippable Linux interpreter (Release, static-as-possible, TLS on)
#   release/ki-windows-x64.exe  the shippable Windows interpreter (fully static mingw-w64 cross-build)
#   release/debug_builds.zip    the asan + tsan sanitizer interpreters (maximal compression)
#   release/docs.zip            the entire docs/ directory (maximal compression)
#
# The four binaries are produced by tools/scripts/build_all.sh (into dist/); this script drives that
# build, then copies the two shippable interpreters and packs the sanitizer builds + docs into zips.
# Prerequisites are build_all.sh's — for the Windows binary you need mingw-w64:
#   sudo apt-get install -y build-essential cmake ninja-build git perl libssl-dev mingw-w64
#
# Usage:  tools/scripts/prepare_release.sh [--no-build] [--out=DIR]
#   --no-build   reuse the existing dist/ (skip the multi-minute rebuild) — for iterating on packaging
#   --out=DIR    release directory to assemble (default: release)

set -euo pipefail
cd "$(dirname "$0")/../.."

OUT="release"
SKIP_BUILD=0
for a in "$@"; do
    case "$a" in
        --no-build) SKIP_BUILD=1 ;;
        --out=*)    OUT="${a#--out=}" ;;
        -h|--help)  grep -E '^# ' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $a (see --help)" >&2; exit 2 ;;
    esac
done

require() { [ -f "$1" ] || { echo "ERROR: expected $1 but it is missing." >&2; exit 1; }; }

# 1. Build the four binaries into dist/ (Linux + Windows Release, asan + tsan sanitizer builds).
if [ "$SKIP_BUILD" -eq 1 ]; then
    echo "prepare-release: --no-build — reusing existing dist/"
else
    tools/scripts/build_all.sh
fi

# 2. A fresh, clean release directory.
rm -rf "$OUT"
mkdir -p "$OUT"

# 3. The two shippable interpreters, copied as-is.
require dist/ki-linux-x64
cp dist/ki-linux-x64 "$OUT/ki-linux-x64"
if [ ! -f dist/ki-windows-x64.exe ]; then
    echo "ERROR: dist/ki-windows-x64.exe is missing — the Windows cross-build was skipped." >&2
    echo "       Install the cross compiler and re-run:  sudo apt-get install -y mingw-w64" >&2
    exit 1
fi
cp dist/ki-windows-x64.exe "$OUT/ki-windows-x64.exe"

# 4. debug_builds.zip — the asan + tsan interpreters, flat, maximal compression (-9).
require dist/debug-asan
require dist/debug-tsan
rm -f "$OUT/debug_builds.zip"
( cd dist && zip -9 -j "$OLDPWD/$OUT/debug_builds.zip" debug-asan debug-tsan >/dev/null )
echo "packed  $OUT/debug_builds.zip"

# 5. docs.zip — the entire docs/ tree, rebuilt fresh so the shipped site is current, maximal compression.
python3 docs/build_docs.py >/dev/null
rm -f "$OUT/docs.zip"
zip -9 -r "$OUT/docs.zip" docs >/dev/null
echo "packed  $OUT/docs.zip"

echo "=================================================================="
echo "release assembled in $OUT/:"
ls -la "$OUT"
