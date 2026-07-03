#!/bin/sh
# Kirito installer for Linux / macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/kiritolang/kiritolang.github.io/main/tools/scripts/install.sh | sh
#
# Installs the `ki` interpreter and the `kpm` package manager into ~/.local/bin (no root needed),
# and creates the package directory ~/.kirito/packages (which `ki` puts on its import path, so
# packages installed by `kpm` are importable directly).
#
# Options (pass after `| sh -s --` when piping, or directly when running the file):
#   --bin-dir DIR     where to install the `ki`/`kpm` launchers   (default: ~/.local/bin)
#   --ref REF         git ref to fetch kpm.ki / build from        (default: main)
#   --from-source     build `ki` from source instead of downloading a release binary
set -eu

REPO="kiritolang/kiritolang.github.io"
BIN_DIR="${KIRITO_BIN:-$HOME/.local/bin}"
KIRITO_HOME="$HOME/.kirito"
REF="main"
FROM_SOURCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --bin-dir) BIN_DIR="$2"; shift 2 ;;
        --ref) REF="$2"; shift 2 ;;
        --from-source) FROM_SOURCE=1; shift ;;
        -h|--help) sed -n '2,14p' "$0" 2>/dev/null || echo "see the header of install.sh"; exit 0 ;;
        *) echo "install.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

OS="$(uname -s)"
ARCH="$(uname -m)"
mkdir -p "$BIN_DIR" "$KIRITO_HOME/packages"

# Pick a downloader.
fetch() {  # fetch <url> <dest>
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -qO "$2" "$1"
    else die "need curl or wget to download files"; fi
}

build_from_source() {
    say "building ki from source ($REPO@$REF)"
    command -v git   >/dev/null 2>&1 || die "git is required to build from source"
    command -v cmake >/dev/null 2>&1 || die "cmake is required to build from source"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    git clone --depth 1 --branch "$REF" "https://github.com/$REPO" "$tmp/src" \
        || git clone --depth 1 "https://github.com/$REPO" "$tmp/src" || die "git clone failed"
    # TLS is needed for kpm (GitHub is HTTPS). Enable it when OpenSSL headers are present.
    tls="OFF"
    if [ -f /usr/include/openssl/ssl.h ] || ls /usr/include/*/openssl/ssl.h >/dev/null 2>&1; then tls="ON"; fi
    [ "$tls" = "ON" ] || warn "building without TLS: kpm cannot fetch from GitHub (install libssl-dev and re-run)"
    cmake -S "$tmp/src" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release -DKIRITO_ENABLE_TLS=$tls >/dev/null
    cmake --build "$tmp/build" --target ki >/dev/null
    install -m 0755 "$tmp/build/ki" "$BIN_DIR/ki"
}

download_release() {
    case "$OS/$ARCH" in
        Linux/x86_64|Linux/amd64)
            url="https://github.com/$REPO/releases/latest/download/ki-linux-x64"
            say "downloading ki-linux-x64"
            if fetch "$url" "$BIN_DIR/ki"; then chmod +x "$BIN_DIR/ki"; return 0; fi
            return 1 ;;
        *)
            return 1 ;;
    esac
}

if [ "$FROM_SOURCE" = "1" ]; then
    build_from_source
elif ! download_release; then
    warn "no prebuilt binary for $OS/$ARCH (or download failed) — building from source"
    build_from_source
fi

# Install the kpm package manager (a Kirito script) + a launcher on PATH.
say "installing kpm"
fetch "https://raw.githubusercontent.com/$REPO/$REF/kpm/kpm.ki" "$KIRITO_HOME/kpm.ki"
cat > "$BIN_DIR/kpm" <<EOF
#!/bin/sh
# KPM_SELF lets \`kpm update-kpm\` overwrite this kpm.ki; KPM_KI_PATH lets \`kpm update-ki\` replace
# the interpreter binary (kpm also falls back to sys.executable for the latter).
export KPM_SELF="$KIRITO_HOME/kpm.ki"
export KPM_KI_PATH="$BIN_DIR/ki"
exec "$BIN_DIR/ki" "$KIRITO_HOME/kpm.ki" "\$@"
EOF
chmod +x "$BIN_DIR/kpm"

say "verifying"
"$BIN_DIR/ki" --help >/dev/null 2>&1 || die "the installed ki binary did not run"

say "Kirito installed:"
echo "    ki  -> $BIN_DIR/ki"
echo "    kpm -> $BIN_DIR/kpm   (packages: $KIRITO_HOME/packages)"
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) warn "$BIN_DIR is not on your PATH — add this to your shell profile:"
       echo "    export PATH=\"$BIN_DIR:\$PATH\"" ;;
esac
echo "Try:  ki --help      |      kpm help"
