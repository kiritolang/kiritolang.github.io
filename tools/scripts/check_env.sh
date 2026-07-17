#!/usr/bin/env bash
# check_env.sh — verify the local toolchain satisfies the pinned versions in tools/versions.env.
# Fails loudly with an actionable message per missing/too-old tool, so a misconfigured environment is
# caught before a confusing build error. Exit 0 iff every REQUIRED tool is present and new enough.
#
# Usage: tools/scripts/check_env.sh [--quiet]   (--quiet: print only on failure)
set -euo pipefail

QUIET=0
for arg in "$@"; do
  case "$arg" in
    --quiet) QUIET=1 ;;
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "check_env.sh: unknown argument '$arg' (try --help)" >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSIONS_FILE="$SCRIPT_DIR/../versions.env"
[ -f "$VERSIONS_FILE" ] || { echo "check_env.sh: missing pinned versions file: $VERSIONS_FILE" >&2; exit 2; }
# shellcheck disable=SC1090
. "$VERSIONS_FILE"

say() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }

# ver_lt A B  ->  true iff A < B (semantic, via sort -V). Empty A counts as "missing" (caller handles).
ver_ge() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$2" ]; }

# extract the first dotted version token from a tool's --version output
tool_version() { "$@" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1; }

fails=0
# check_tool  NAME  MIN  REF  REQUIRED(1/0)  CMD...
check_tool() {
  local name="$1" min="$2" ref="$3" required="$4"; shift 4
  local bin="$1"
  if ! command -v "$bin" >/dev/null 2>&1; then
    if [ "$required" -eq 1 ]; then
      echo "MISSING (required): $name — install $bin >= $min (project reference: $ref)" >&2
      fails=$((fails + 1))
    else
      say "optional : $name not found — only needed for its opt-in feature (reference: $ref)"
    fi
    return
  fi
  local have; have="$(tool_version "$@")"
  if [ -z "$have" ]; then
    say "note     : $name found ($bin) but could not parse a version; skipping version check"
    return
  fi
  if ver_ge "$have" "$min"; then
    say "ok       : $name $have (>= $min; reference $ref)"
  else
    echo "TOO OLD (required): $name $have — need >= $min (project reference: $ref)" >&2
    [ "$required" -eq 1 ] && fails=$((fails + 1))
  fi
}

say "Checking toolchain against $(cd "$SCRIPT_DIR/.." && pwd)/versions.env (C++$CXX_STANDARD) ..."
check_tool "g++"     "$GXX_MIN"     "$GXX_REF"     1 g++ --version
check_tool "cmake"   "$CMAKE_MIN"   "$CMAKE_REF"   1 cmake --version
check_tool "ninja"   "$NINJA_MIN"   "$NINJA_REF"   1 ninja --version
check_tool "clang++" "$CLANGXX_MIN" "$CLANGXX_REF" 1 clang++ --version
check_tool "python3" "$PYTHON3_MIN" "$PYTHON3_REF" 1 python3 --version

if [ "$fails" -gt 0 ]; then
  echo "" >&2
  echo "check_env.sh: $fails required tool(s) missing or too old — fix the above before building." >&2
  exit 1
fi
say "Environment OK."
