#!/usr/bin/env python3
# End-to-end test harness for kpm (the Kirito package manager, kpm/kpm.ki).
#
# It runs the REAL kpm under `ki` against a LOCAL mock git host served over http (no external network,
# no TLS), so the full install/resolve/dependency/update/remove path is exercised — plus every failure
# mode (missing/invalid manifest, 404 module, path traversal, version conflict, cycle) and BOTH the
# GitHub and GitLab adapters.
#
# The mock serves three shapes from one in-memory registry:
#   GitHub API  (KPM_GITHUB_API -> /ghapi):  /ghapi/repos/<o>/<r>  and  /ghapi/repos/<o>/<r>/tags
#   GitHub raw  (KPM_GITHUB_RAW -> /ghraw):   /ghraw/<o>/<r>/<ref>/<path>
#   GitLab API  (gitlab+http://host/<o>/<r>): /api/v4/projects/<enc>/...   (+ .../repository/files/.../raw)
#
# Usage:  kpm_integration.py <path-to-ki>      (kpm.ki is found relative to this file)
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
KPM_KI = os.path.normpath(os.path.join(HERE, "..", "..", "kpm", "kpm.ki"))

# registry[(owner, repo)] = {"default_branch": str, "tags": [str], "refs": {ref: {path: content}}}
REGISTRY = {}

def reg(owner, repo, default_branch="main", tags=None):
    REGISTRY[(owner, repo)] = {"default_branch": default_branch, "tags": tags or [], "refs": {}}
    return (owner, repo)

def put_file(key, ref, path, content):
    REGISTRY[key]["refs"].setdefault(ref, {})[path] = content

def put_manifest(key, ref, name, version, modules, deps=None, *, raw=None):
    m = {"name": name, "version": version, "modules": modules}
    if deps is not None:
        m["dependencies"] = deps
    put_file(key, ref, "kirito.json", raw if raw is not None else json.dumps(m))

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):  # quiet
        pass

    def _send(self, code, body, ctype="application/json"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        p = u.path
        try:
            if p.startswith("/ghapi/repos/"):
                rest = p[len("/ghapi/repos/"):]
                if rest.endswith("/tags"):
                    o, r = rest[:-len("/tags")].split("/", 1)
                    e = REGISTRY.get((o, r))
                    if not e: return self._send(404, "{}")
                    return self._send(200, json.dumps([{"name": t} for t in e["tags"]]))
                o, r = rest.split("/", 1)
                e = REGISTRY.get((o, r))
                if not e: return self._send(404, "{}")
                return self._send(200, json.dumps({"default_branch": e["default_branch"]}))
            if p.startswith("/ghraw/"):
                o, r, ref, path = p[len("/ghraw/"):].split("/", 3)
                return self._raw(o, r, ref, path)
            if p.startswith("/api/v4/projects/"):
                rest = p[len("/api/v4/projects/"):]
                enc = rest.split("/", 1)[0]
                proj = urllib.parse.unquote(enc)         # owner%2Frepo -> owner/repo
                o, r = proj.split("/", 1)
                e = REGISTRY.get((o, r))
                if not e: return self._send(404, "{}")
                tail = rest[len(enc):]
                if tail == "" or tail == "/":
                    return self._send(200, json.dumps({"default_branch": e["default_branch"]}))
                if tail == "/repository/tags":
                    return self._send(200, json.dumps([{"name": t} for t in e["tags"]]))
                if tail.startswith("/repository/files/") and tail.endswith("/raw"):
                    fenc = tail[len("/repository/files/"):-len("/raw")]
                    fpath = urllib.parse.unquote(fenc)
                    ref = urllib.parse.parse_qs(u.query).get("ref", ["main"])[0]
                    return self._raw(o, r, ref, fpath)
            return self._send(404, "not found: " + p, "text/plain")
        except Exception as ex:
            return self._send(500, "mock error: " + str(ex), "text/plain")

    def _raw(self, o, r, ref, path):
        e = REGISTRY.get((o, r))
        if not e or ref not in e["refs"] or path not in e["refs"][ref]:
            return self._send(404, "no file " + path + "@" + ref, "text/plain")
        return self._send(200, e["refs"][ref][path], "text/plain")

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close(); return port

# ---- test driver ----
KI = sys.argv[1] if len(sys.argv) > 1 else "ki"
PORT = free_port()
SERVER = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
threading.Thread(target=SERVER.serve_forever, daemon=True).start()
BASE = "http://127.0.0.1:%d" % PORT

PASS = 0
FAILED = []
HOME = tempfile.mkdtemp(prefix="kpm_it_")

def env(extra=None):
    e = dict(os.environ)
    e["HOME"] = HOME
    e.pop("USERPROFILE", None)
    e["KPM_GITHUB_API"] = BASE + "/ghapi"
    e["KPM_GITHUB_RAW"] = BASE + "/ghraw"
    # make sure no real tokens leak host behavior
    for k in ("GITHUB_TOKEN", "KPM_GITHUB_TOKEN", "GITLAB_TOKEN", "KPM_GITLAB_TOKEN"):
        e.pop(k, None)
    if extra: e.update(extra)
    return e

def kpm(*args, extra=None):
    r = subprocess.run([KI, KPM_KI] + list(args), env=env(extra),
                       capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout, r.stderr

def pkgdir(name):
    return os.path.join(HOME, ".kirito", "packages", name)

def check(label, cond, detail=""):
    global PASS
    if cond:
        PASS += 1
        print("  ok  " + label)
    else:
        FAILED.append(label)
        print("FAIL  " + label + ("  :: " + detail if detail else ""))

def reset_home():
    pk = os.path.join(HOME, ".kirito", "packages")
    if os.path.isdir(pk): shutil.rmtree(pk)

try:
    # ---- 1. basic install (GitHub default), multi-module ----
    REGISTRY.clear()
    k = reg("alice", "tools", tags=[])
    put_manifest(k, "main", "tools", "1.0.0", ["tools.ki", "tools/util.ki"])
    put_file(k, "main", "tools.ki", "var hello = Function(): return 1\n")
    put_file(k, "main", "tools/util.ki", "var u = 2\n")
    rc, out, err = kpm("install", "alice/tools")
    check("install rc==0", rc == 0, err)
    check("install wrote tools.ki", os.path.exists(os.path.join(pkgdir("tools"), "tools.ki")))
    check("install wrote nested module", os.path.exists(os.path.join(pkgdir("tools"), "tools", "util.ki")))
    check("install wrote .kpm.json", os.path.exists(os.path.join(pkgdir("tools"), ".kpm.json")))
    if os.path.exists(os.path.join(pkgdir("tools"), ".kpm.json")):
        rec = json.load(open(os.path.join(pkgdir("tools"), ".kpm.json")))
        check("record source==alice/tools", rec["source"] == "alice/tools", rec.get("source"))
        check("record version==1.0.0", rec["version"] == "1.0.0", rec.get("version"))

    # ---- 2. list ----
    rc, out, err = kpm("list")
    check("list shows tools 1.0.0", rc == 0 and "tools" in out and "1.0.0" in out, out)

    # ---- 3. remove ----
    rc, out, err = kpm("remove", "tools")
    check("remove rc==0", rc == 0, err)
    check("remove deleted dir", not os.path.exists(pkgdir("tools")))

    # ---- 4. dependencies (A -> B) ----
    reset_home(); REGISTRY.clear()
    a = reg("alice", "app"); b = reg("bob", "lib")
    put_manifest(a, "main", "app", "1.0.0", ["app.ki"], deps=["bob/lib"])
    put_file(a, "main", "app.ki", "var a = 1\n")
    put_manifest(b, "main", "lib", "1.0.0", ["lib.ki"])
    put_file(b, "main", "lib.ki", "var b = 1\n")
    rc, out, err = kpm("install", "alice/app")
    check("dep install rc==0", rc == 0, err)
    check("dep installed app", os.path.exists(pkgdir("app")))
    check("dep installed lib (transitive)", os.path.exists(pkgdir("lib")))

    # ---- 5. semver constraint resolution ----
    reset_home(); REGISTRY.clear()
    s = reg("sam", "semverpkg", tags=["v1.0.0", "v1.1.0", "v1.2.0", "v2.0.0"])
    for t in ["v1.0.0", "v1.1.0", "v1.2.0", "v2.0.0"]:
        ver = t[1:]
        put_manifest(s, t, "semverpkg", ver, ["m.ki"]); put_file(s, t, "m.ki", "var v=\"%s\"\n" % ver)
    rc, out, err = kpm("install", "sam/semverpkg@^1.0.0")
    recpath = os.path.join(pkgdir("semverpkg"), ".kpm.json")
    rec = json.load(open(recpath)) if os.path.exists(recpath) else {}
    check("semver ^1.0.0 picks 1.2.0", rec.get("version") == "1.2.0", str(rec.get("version")))
    check("semver records constraint", rec.get("constraint") == "^1.0.0", str(rec.get("constraint")))

    # ---- 6. outdated + update (publish a newer tag, re-resolve) ----
    s2 = REGISTRY[s]; s2["tags"].append("v1.3.0")
    put_manifest(s, "v1.3.0", "semverpkg", "1.3.0", ["m.ki"]); put_file(s, "v1.3.0", "m.ki", "var v=\"1.3.0\"\n")
    rc, out, err = kpm("outdated")
    check("outdated sees 1.3.0", rc == 0 and "1.3.0" in out, out)
    rc, out, err = kpm("update", "semverpkg")
    rec = json.load(open(os.path.join(pkgdir("semverpkg"), ".kpm.json")))
    check("update -> 1.3.0", rec.get("version") == "1.3.0", str(rec.get("version")))

    # ---- 7. GitLab adapter (gitlab+http://) ----
    reset_home(); REGISTRY.clear()
    g = reg("group", "glpkg", tags=["v0.5.0"])
    put_manifest(g, "v0.5.0", "glpkg", "0.5.0", ["g.ki"]); put_file(g, "v0.5.0", "g.ki", "var g=1\n")
    rc, out, err = kpm("install", "gitlab+%s/group/glpkg@^0.5.0" % BASE)
    check("gitlab install rc==0", rc == 0, err)
    check("gitlab installed glpkg", os.path.exists(os.path.join(pkgdir("glpkg"), "g.ki")), err)
    if os.path.exists(os.path.join(pkgdir("glpkg"), ".kpm.json")):
        rec = json.load(open(os.path.join(pkgdir("glpkg"), ".kpm.json")))
        check("gitlab record re-parseable source", rec["source"].startswith("gitlab+"), rec.get("source"))
        # the recorded source must round-trip through update
        rc2, o2, e2 = kpm("update", "glpkg")
        check("gitlab update rc==0 (source round-trips)", rc2 == 0, e2)

    # ---- 8. version conflict (A->dep@^1, C->dep@^2) ----
    reset_home(); REGISTRY.clear()
    A = reg("x", "A"); C = reg("x", "C"); D = reg("x", "dep", tags=["v1.0.0", "v2.0.0"])
    put_manifest(A, "main", "A", "1.0.0", ["a.ki"], deps=["x/dep@^1.0.0"]); put_file(A, "main", "a.ki", "var a=1\n")
    put_manifest(C, "main", "C", "1.0.0", ["c.ki"], deps=["x/dep@^2.0.0"]); put_file(C, "main", "c.ki", "var c=1\n")
    for t in ["v1.0.0", "v2.0.0"]:
        put_manifest(D, t, "dep", t[1:], ["d.ki"]); put_file(D, t, "d.ki", "var d=1\n")
    rc, out, err = kpm("install", "x/A", "x/C")
    check("conflict install completes", rc == 0, err)
    check("conflict warns", "version conflict" in err or "version conflict" in out, err)

    # ---- 9. cycle (A<->B) doesn't hang ----
    reset_home(); REGISTRY.clear()
    ca = reg("c", "A"); cb = reg("c", "B")
    put_manifest(ca, "main", "cyA", "1.0.0", ["a.ki"], deps=["c/B"]); put_file(ca, "main", "a.ki", "var a=1\n")
    put_manifest(cb, "main", "cyB", "1.0.0", ["b.ki"], deps=["c/A"]); put_file(cb, "main", "b.ki", "var b=1\n")
    rc, out, err = kpm("install", "c/A")
    check("cycle terminates", rc == 0, err)
    check("cycle installed both", os.path.exists(pkgdir("cyA")) and os.path.exists(pkgdir("cyB")))

    # ---- 10. misconfigured repositories (each must fail cleanly, non-zero, no crash) ----
    def expect_fail(label, key_setup, spec, needle):
        reset_home(); REGISTRY.clear(); key_setup()
        rc, out, err = kpm("install", spec)
        msg = (out + err).lower()
        check(label, rc != 0 and needle.lower() in msg, "rc=%d out=%r err=%r" % (rc, out, err))

    def m_missing():
        reg("e", "nomanifest")  # no kirito.json served
    expect_fail("missing kirito.json fails", m_missing, "e/nomanifest", "no kirito.json")

    def m_badjson():
        kk = reg("e", "badjson"); put_file(kk, "main", "kirito.json", "{ this is not json ")
    expect_fail("invalid JSON manifest fails", m_badjson, "e/badjson", "not valid json")

    def m_nomodules():
        kk = reg("e", "nomod"); put_file(kk, "main", "kirito.json", json.dumps({"name": "nomod", "version": "1.0.0"}))
    expect_fail("manifest without modules fails", m_nomodules, "e/nomod", "modules")

    def m_traversal():
        kk = reg("e", "evil")
        put_manifest(kk, "main", "evil", "1.0.0", ["../../escape.ki"])
        put_file(kk, "main", "../../escape.ki", "PWNED\n")
    expect_fail("path-traversal module rejected", m_traversal, "e/evil", "unsafe")
    check("path traversal wrote nothing outside", not os.path.exists(os.path.join(HOME, "escape.ki"))
          and not os.path.exists(os.path.join(HOME, "..", "escape.ki")))

    def m_badname():
        kk = reg("e", "badname"); put_file(kk, "main", "kirito.json",
            json.dumps({"name": "../oops", "version": "1.0.0", "modules": ["m.ki"]}))
    expect_fail("malicious package name rejected", m_badname, "e/badname", "invalid package name")

    def m_404mod():
        kk = reg("e", "missingmod"); put_manifest(kk, "main", "missingmod", "1.0.0", ["present.ki", "absent.ki"])
        put_file(kk, "main", "present.ki", "var p=1\n")  # absent.ki intentionally not served
    expect_fail("missing module file fails", m_404mod, "e/missingmod", "not found")

    def m_badsemver():
        reg("e", "notags", tags=[])  # constraint with no matching tags
        put_manifest(("e", "notags"), "main", "notags", "1.0.0", ["m.ki"])
    expect_fail("unsatisfiable constraint fails", m_badsemver, "e/notags@^9.9.9", "satisfies")

    # ---- 11. unrecognized host needs an explicit kind hint ----
    reset_home(); REGISTRY.clear()
    rc, out, err = kpm("install", "https://git.unknown.example/o/r")
    check("unknown host errors with guidance", rc != 0 and ("unrecognized git host" in (out+err)), err)

    # ---- 12. remove of a not-installed package is a clean message, not a crash ----
    rc, out, err = kpm("remove", "doesnotexist")
    check("remove missing is graceful", rc == 0 and "not installed" in (out+err), out+err)

finally:
    SERVER.shutdown()
    shutil.rmtree(HOME, ignore_errors=True)

print("\n%d checks passed, %d failed" % (PASS, len(FAILED)))
if FAILED:
    print("FAILURES: " + ", ".join(FAILED))
    sys.exit(1)
print("ALL KPM INTEGRATION TESTS PASSED")
