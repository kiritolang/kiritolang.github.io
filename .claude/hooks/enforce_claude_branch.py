#!/usr/bin/env python3
"""PreToolUse Bash hook: enforce that Claude only commits and pushes on `claude-branch`.

Contract (Claude Code hooks): read the tool call as JSON on stdin, decide, and either exit 0 to
allow the tool or exit 2 with a reason on stderr to block it. Only inspects Bash `git push` /
`git commit` invocations; every other tool call is passed through untouched.

Detection uses `shlex` so that the words "git push" appearing inside a quoted argument (for
example, in a commit message body) do NOT trigger the rule — only an actual `git <op>` invocation
does. If tokenization fails, we fall back to matching only at the very start of the command.

Rules (see CLAUDE.md's `## Git` section):
  * commits are only allowed while HEAD is on `claude-branch`
  * pushes are only allowed from `claude-branch` and may never target `main`
"""
import json
import re
import shlex
import subprocess
import sys

ALLOWED_BRANCH = "claude-branch"


def read_command() -> str:
    try:
        data = json.load(sys.stdin)
    except Exception:
        return ""
    return (data.get("tool_input") or {}).get("command") or ""


def tokens(cmd: str):
    try:
        return shlex.split(cmd, posix=True)
    except ValueError:
        return None


def _skip_git_options(tks, i):
    # After a `git` token, skip `-c key=val` / `--git-dir=...` style top-level options so we can
    # reach the subcommand token.
    j = i + 1
    while j < len(tks):
        t = tks[j]
        if t == "-c" and j + 1 < len(tks):
            j += 2
            continue
        if t.startswith("--") and "=" in t:
            j += 1
            continue
        break
    return j


def uses_git(op: str, cmd: str) -> bool:
    tks = tokens(cmd)
    if tks is not None:
        for i, t in enumerate(tks):
            if t == "git":
                j = _skip_git_options(tks, i)
                if j < len(tks) and tks[j] == op:
                    return True
        return False
    # Fallback: only trip when the command literally *starts* with `git <op>`.
    return bool(re.match(rf'\s*(?:[A-Za-z_]\w*=\S*\s+)*git(?:\s+-c\s+\S+)*\s+{op}\b', cmd))


def push_touches_main(cmd: str) -> bool:
    tks = tokens(cmd)
    if tks is None:
        # Fallback: be conservative and inspect the whole string.
        return bool(re.search(r'(^|[\s:/])main(\s|$|:)', cmd) or "refs/heads/main" in cmd)
    for i, t in enumerate(tks):
        if t != "git":
            continue
        j = _skip_git_options(tks, i)
        if j >= len(tks) or tks[j] != "push":
            continue
        # Look at the tokens that follow `git push`. A refspec that mentions `main` — as `main`,
        # `HEAD:main`, or `refs/heads/main` — is what we care about; unrelated tokens are ignored.
        for arg in tks[j + 1:]:
            if arg.startswith("-"):
                continue
            if arg == "main":
                return True
            if ":" in arg and arg.rsplit(":", 1)[1] == "main":
                return True
            if arg.endswith("refs/heads/main"):
                return True
    return False


def current_branch() -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], stderr=subprocess.DEVNULL
        )
        return out.decode().strip()
    except Exception:
        return ""


def deny(msg: str) -> None:
    print(f"blocked by claude-branch policy: {msg}", file=sys.stderr)
    print(
        f"hint: work only on '{ALLOWED_BRANCH}' (see CLAUDE.md '## Git'). Restart the "
        f"branch with `git fetch origin main && git checkout -B {ALLOWED_BRANCH} origin/main`.",
        file=sys.stderr,
    )
    sys.exit(2)


def main() -> None:
    cmd = read_command()
    if not cmd:
        sys.exit(0)

    if uses_git("push", cmd):
        if push_touches_main(cmd):
            deny("git push targets 'main'; push only to claude-branch")
        br = current_branch()
        if br and br != ALLOWED_BRANCH:
            deny(f"current branch is '{br}'; push only from {ALLOWED_BRANCH}")

    if uses_git("commit", cmd):
        br = current_branch()
        if br and br != ALLOWED_BRANCH:
            deny(f"current branch is '{br}'; commit only on {ALLOWED_BRANCH}")

    sys.exit(0)


if __name__ == "__main__":
    main()
