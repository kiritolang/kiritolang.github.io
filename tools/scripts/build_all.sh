#!/usr/bin/env bash
# Build shippable, 64-bit `ki` binaries into dist/ — Release, with HTTPS/TLS enabled and linked as
# statically as possible:
#
#   dist/ki-linux-x64        native gcc + the system OpenSSL; OpenSSL/libstdc++/libgcc are static,
#                            only glibc stays dynamic (a fully -static glibc breaks DNS resolution,
#                            which the net module needs).
#   dist/ki-windows-x64.exe  mingw-w64 cross-compile + a from-source static OpenSSL — fully static,
#                            a standalone .exe with no DLL dependencies.
#
# Prerequisites (Debian/Ubuntu/WSL):
#   sudo apt-get update
#   sudo apt-get install -y build-essential cmake ninja-build git perl libssl-dev mingw-w64
#
# The Windows OpenSSL is built once and cached under .deps/, so re-running only recompiles `ki`.
# This is the only release-build path — the project has no CI; upload dist/* to a GitHub Release
# (tagged with the bare version, e.g. 1.6.1) by hand.

set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD/.deps"
OPENSSL_TAG="openssl-3.3.2"
mkdir -p "$ROOT" dist

# Build a static OpenSSL for the mingw (Windows x64) target, cached.
build_mingw_openssl() {
    local pfx="$ROOT/ssl-win-x64"
    [ -f "$pfx/lib/libssl.a" ] && { echo "openssl[win-x64]: cached"; return; }
    [ -d "$ROOT/openssl" ] || git clone --depth 1 -b "$OPENSSL_TAG" \
        https://github.com/openssl/openssl "$ROOT/openssl"
    local b="$ROOT/build-ssl-win-x64"; rm -rf "$b"; cp -r "$ROOT/openssl" "$b"
    ( cd "$b"
      ./Configure mingw64 no-shared no-tests no-docs --libdir=lib \
          --cross-compile-prefix=x86_64-w64-mingw32- --prefix="$pfx"
      # OpenSSL's generated Makefile carries a self-regeneration rule that fires (regenerating, then
      # exiting 1 with "Please run the same make command again") whenever it judges a build input
      # newer than the Makefile — which happens with the fresh copy+configure timestamps under a
      # parallel make, and is environment-dependent (mtime granularity / make version). Touch the
      # freshly generated Makefile so it's definitively newest, then build; re-run once as the
      # OpenSSL-blessed fallback in case the rule still fires. A genuine build error still fails the
      # second pass.
      touch Makefile
      make -j"$(nproc)" >/dev/null 2>&1 || make -j"$(nproc)" >/dev/null
      make install_sw >/dev/null )
    echo "openssl[win-x64]: built"
}

# build_ki <output-name> <cmake args...>
build_ki() {
    local out="$1"; shift
    local dir="$ROOT/build-$out"; rm -rf "$dir"
    cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DKIRITO_ENABLE_TLS=ON -DOPENSSL_USE_STATIC_LIBS=ON "$@"
    cmake --build "$dir" --target ki
    cp "$dir/ki" "dist/$out" 2>/dev/null || cp "$dir/ki.exe" "dist/$out"
    echo "built dist/$out"
}

# --- Linux x86_64 (host) ---
build_ki ki-linux-x64 -DCMAKE_EXE_LINKER_FLAGS="-s"

# --- Windows x86_64 via mingw-w64 (skipped if the cross compiler isn't installed) ---
if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    build_mingw_openssl
    build_ki ki-windows-x64.exe \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
        -DOPENSSL_ROOT_DIR="$ROOT/ssl-win-x64" \
        -DCMAKE_EXE_LINKER_FLAGS="-s -static"
else
    echo "SKIP[ki-windows-x64.exe]: x86_64-w64-mingw32-g++ not found (sudo apt-get install -y mingw-w64)"
fi

echo "=================================================================="
ls -la dist/ | grep -E 'ki-' || echo "(no binaries produced)"
file dist/ki-* 2>/dev/null || true
