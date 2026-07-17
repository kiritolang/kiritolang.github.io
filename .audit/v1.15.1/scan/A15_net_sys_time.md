# A15 — net + sys + time (audit v1.15.1)

Scope:
- `src/kirito/stdlib_net.hpp`
- `src/kirito/net_compat.hpp`
- `src/kirito/stdlib_sys.hpp`
- `src/kirito/proc_compat.hpp`
- `src/kirito/stdlib_time.hpp`

Rules honoured: loopback/local servers only (no external network); `settimeout` on every blocking
call; every probe run against the real `./build-debug/ki`; no builds.

Known false positives (NOT to re-report):
- `sys.exit` flushes std streams but not open user `fstream`s (v1.14 A19-5) — documented tradeoff.
- `Session.headers` go to every host (v1.15 A12-1b) — deliberate, matches `requests`.
- v1.15 fixes A12-1 (host-scoped jar), A12-2 (socketpair family), A12-3 (negative settimeout) — verify, don't re-fix.

---

## Findings

### A15-1: a bad `cwd` is misreported as "failed to start '<program>'"  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/proc_compat.hpp:266-270` (POSIX child), surfaced via `stdlib_sys.hpp:53` (`ProcError` -> `KiritoError`)
- What: the child uses ONE exec-error channel (`exP`) for two different failures — `chdir(cwd)` failing and
  `execvp` failing — writing only a bare `errno`. The parent cannot tell them apart and unconditionally
  reports `"failed to start '" + argv[0] + "': " + strerror(childErrno)`. So a missing/unusable **cwd** is
  blamed on the **program**, which demonstrably exists. Breaks the CLAUDE.md contract that errors are
  "a message a user can act on".
- Repro (`/tmp/a15_sys2.ki`, real output):
```
--- cwd failure misattributed to the program? ---
cwd missing  : failed to start '/bin/pwd': No such file or directory
/bin/pwd exists? True
cwd not a dir: failed to start '/bin/pwd': Not a directory
cwd no perm  : failed to start '/bin/pwd': Permission denied
shell bad cwd: failed to start '/bin/sh': No such file or directory
```
  Note the last line especially: `sys.shell("pwd", "/nonexistent/dir/xyz")` claims **/bin/sh** is missing.
- Impact: anyone passing `cwd=` to `sys.createprocess`/`sys.shell`. The diagnostic sends them hunting for a
  missing interpreter/binary (or a PATH problem) when the real fault is the working directory. Actively
  misleading, not merely terse.
- Proposed fix: write a 2-field payload on the error channel — a stage tag plus the errno — instead of a bare
  int. The child already writes `sizeof(int)`; widen it to `struct { char stage; int err; }` (or write two
  ints), with stage `'c'` for chdir and `'x'` for exec. The parent then reports
  `"failed to change directory to '<cwd>': <strerror>"` vs the existing exec message. Mirror it in the Windows
  branch, where `CreateProcessW` already fails as a unit (a bad cwd there yields
  `ERROR_DIRECTORY`/`ERROR_PATH_NOT_FOUND`) — that branch can special-case those codes to the same wording.
  Risks no documented contract (it only sharpens an error string); the existing `.experr` tests don't pin it
  (see coverage gaps).
- Proposed test: `tools/tests/scripts/sys_proc.ki` (or a new `.experr` case) — assert `sys.createprocess(["/bin/pwd"],
  "/nonexistent/dir/xyz")` throws a message containing `"directory"` and NOT `"failed to start"`; plus a
  positive control that a genuinely missing program still says `failed to start`.

### A15-2: an embedded NUL silently truncates an argv element / shell command / cwd  [severity: MED] [confidence: confirmed]
- Location: `src/kirito/proc_compat.hpp:248` (`cargv.push_back(const_cast<char*>(s.c_str()))`), `:266`
  (`chdir(cwd.c_str())`); reached from `stdlib_sys.hpp:221` (argv build) and `:235` (`shell` command)
- What: a Kirito `String` is byte-transparent and may hold `\0` (`len("a\0b") == 3`), but argv/cwd cross the
  syscall boundary as NUL-terminated `c_str()`. Everything from the first NUL on is **silently discarded** —
  no error, no warning. The language accepts a value it then quietly mangles.
- Repro (`/tmp/a15_sys2.ki`, real output):
```
kirito String len of 'a\0b' = 3
echo arg with NUL -> stdout bytes: 2 repr=a
sh -c with NUL -> start
shell with NUL -> start
```
  i.e. `sys.createprocess(["/bin/echo", "a\0b"])` echoes `a` — the `b` vanished; and
  `sys.shell("echo start\0; echo INJECTED")` runs only `echo start`.
- Impact: silent data loss, and the classic **poison-NUL truncation bypass**: a caller that validates a
  user-supplied string (`if p.endswith(".png")` / an allowlist check) passes for `"evil.sh\0.png"` while
  `execvp` sees `"evil.sh"`. Validation and execution disagree about the same value — exactly the split the
  language's byte-transparent String makes reachable. CPython closes this hole by raising
  `ValueError: embedded null byte` from `subprocess`.
- Proposed fix: reject rather than truncate, matching CPython and matching how this codebase already handles
  the analogous injection vector (net's `setHdr` **throws** on CR/LF rather than encoding). Validate in
  `stdlib_sys.hpp` where the Kirito values are still in hand — each argv element, `command`, and `cwd` — e.g.
  `if (s.find('\0') != std::string::npos) throw KiritoError("createprocess: argument must not contain a NUL byte")`.
  Single-source it as one small helper used by both `createprocess` and `shell` (and `cwd` in
  `runExternalProcess`). Breaks no documented contract: no test or doc pins NUL-truncation, and today's
  behaviour is silent corruption. (`input` is NOT affected — it correctly uses `.data()`/`.size()`.)
- Proposed test: `tools/tests/scripts/sys_proc_adversarial.ki` — assert `createprocess(["/bin/echo", "a\0b"])`,
  `shell("echo a\0b")` and a NUL-bearing `cwd` each throw, and that the message names the NUL byte.
