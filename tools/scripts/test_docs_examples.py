#!/usr/bin/env python3
"""Extract every fenced ```kirito block in docs/pages/*.md and run each through `ki`, asserting
no crash. This is the "documentation as executable tests" idea from Python's doctest culture: docs
that used to rot silently now fail the CI when a bumped stdlib signature or dropped built-in
breaks an example.

A block is EXCLUDED (not executed) if it is preceded by an HTML comment naming a directive that
says "this snippet is not runnable on its own":

    <!--norun-->                      generic skeleton (has `...` placeholders, no side-effect)
    <!--norun (reason)-->             same, with a human-readable reason

Blocks are otherwise concatenated with a small preamble that pre-imports `io` (docs commonly use
`io.print` without an explicit import for brevity) and executed as one program per block.

Usage:
    tools/scripts/test_docs_examples.py                 # uses `ki` from PATH
    tools/scripts/test_docs_examples.py --ki PATH       # or a specific binary

Exit status is the number of blocks that failed (0 iff all passed or SKIPped).
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# Only unindented (column-0) fences are tested. Indented fences appear inside list items and are
# almost always illustrative fragments (a one-line example next to a bullet); they don't stand on
# their own as runnable programs, and their closers are indented too, which makes them ambiguous
# to lex with a simple regex.
FENCE_RE = re.compile(
    r"(?:(?P<pre><!--.*?-->)\s*\n)?"
    r"^```(?P<lang>kirito|ki)\s*\n(?P<body>.*?)^```",
    re.DOTALL | re.MULTILINE,
)


def extract_blocks(md_text):
    """Yield (line_no, body, skip_reason) tuples for every kirito/ki fence in `md_text`."""
    for m in FENCE_RE.finditer(md_text):
        pre = (m.group("pre") or "").strip()
        line = md_text[:m.start()].count("\n") + 1
        skip = None
        if pre:
            inner = pre[len("<!--"):-len("-->")].strip()
            if inner.startswith("norun"):
                skip = inner
        yield (line, m.group("body"), skip)


# Docs habitually use these modules without spelling out the `import(...)` for brevity — mirror
# what a reader would do at the REPL. The doc snippets that DO include their own imports are fine:
# Kirito allows re-importing (module cache dedups), and the `-w` flag silences the re-declaration
# warning so the run is clean.
PREAMBLE = "\n".join([
    "var io      = import(\"io\")",
    "var math    = import(\"math\")",
    "var random  = import(\"random\")",
    "var path    = import(\"path\")",
    "var sys     = import(\"sys\")",
    "var re      = import(\"regex\")",
    "var regex   = import(\"regex\")",
    "var json    = import(\"json\")",
    "var hash    = import(\"hash\")",
    "var complex = import(\"complex\")",
    "var matrix  = import(\"matrix\")",
    "var tensor  = import(\"tensor\")",
    "var time    = import(\"time\")",
    "",
])


def run_block(ki, body):
    """Write `body` to a temp file, run it under `ki`, return (rc, combined stdout+stderr)."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".ki", delete=False, encoding="utf-8") as f:
        f.write(PREAMBLE)
        f.write(body)
        tmp = f.name
    try:
        # -w silences the "re-declared in this block" warning when a snippet re-imports something
        # we've already pre-loaded — those warnings are noise, not failures.
        p = subprocess.run([ki, "-w", tmp], capture_output=True, timeout=30)
        return p.returncode, (p.stdout + p.stderr).decode(errors="replace")
    finally:
        os.unlink(tmp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ki", default="ki", help="path to the ki interpreter")
    ap.add_argument("--pages", default=os.path.join(ROOT, "docs", "pages"),
                    help="docs/pages directory to scan")
    args = ap.parse_args()

    pages = sorted(glob.glob(os.path.join(args.pages, "*.md")))
    if not pages:
        print("no pages found under " + args.pages, file=sys.stderr)
        return 2

    total = 0
    ran = 0
    skipped = 0
    failed = []
    for page in pages:
        rel = os.path.relpath(page, ROOT)
        with open(page, encoding="utf-8") as f:
            md = f.read()
        for line, body, skip in extract_blocks(md):
            total += 1
            if skip is not None:
                skipped += 1
                continue
            rc, out = run_block(args.ki, body)
            ran += 1
            if rc != 0:
                failed.append((rel, line, rc, out))
                print("  FAIL  %s:%d  (exit %d)" % (rel, line, rc))
                for tail in out.strip().splitlines()[-8:]:
                    print("        " + tail)
    print()
    print("%d fenced blocks total (%d ran, %d skipped by <!--norun-->, %d failed)"
          % (total, ran, skipped, len(failed)))
    return len(failed)


if __name__ == "__main__":
    sys.exit(main())
