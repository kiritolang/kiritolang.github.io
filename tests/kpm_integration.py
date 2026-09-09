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
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
KPM_KI = os.path.normpath(os.path.join(HERE, "..", "kpm", "kpm.ki"))

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

    @staticmethod
    def _paged(items, query):
        # Honor ?per_page=&page= exactly like the real GitHub/GitLab tag APIs, so the mock can exercise
        # kpm's multi-page tag walk. Out-of-range pages yield an empty list (the walk's stop signal).
        q = urllib.parse.parse_qs(query)
        per = int(q.get("per_page", ["100"])[0])
        page = int(q.get("page", ["1"])[0])
        start = max(0, (page - 1) * per)
        return items[start:start + per]

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        p = u.path
        # A deliberately slow endpoint (owner "slowhost") to exercise kpm's request timeout: the server
        # thread stalls before responding, so a client with a short socket timeout must fail fast.
        if "slowhost" in p:
            time.sleep(5)
        try:
            if p.startswith("/ghapi/repos/"):
                rest = p[len("/ghapi/repos/"):]
                if rest.endswith("/tags"):
                    o, r = rest[:-len("/tags")].split("/", 1)
                    e = REGISTRY.get((o, r))
                    if not e: return self._send(404, "{}")
                    return self._send(200, json.dumps([{"name": t} for t in self._paged(e["tags"], u.query)]))
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
                    return self._send(200, json.dumps([{"name": t} for t in self._paged(e["tags"], u.query)]))
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
    msg = (out + err).lower()
    check("incompatible constraints fail the install", rc != 0, err)
    check("conflict names the unsatisfiable set", "satisfies all constraints" in msg, out + err)
    check("nothing installed on conflict (atomic resolve)",
          not os.path.exists(pkgdir("A")) and not os.path.exists(pkgdir("C")) and not os.path.exists(pkgdir("dep")))

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

    def m_emptyimp():
        kk = reg("e", "emptyimp"); put_manifest(kk, "main", "emptyimp", "1.0.0", [".ki"])
        put_file(kk, "main", ".ki", "var x=1\n")
    expect_fail("empty-importable-name module rejected", m_emptyimp, "e/emptyimp", "empty importable name")

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

    # ---- 11b. a GitHub 3-component path is rejected with clear guidance (no confusing 404 later) ----
    rc, out, err = kpm("install", "owner/repo/oops")
    check("github nested path rejected", rc != 0 and "no nested paths" in (out + err), out + err)

    # ---- 12. remove of a not-installed package is a clean message, not a crash ----
    rc, out, err = kpm("remove", "doesnotexist")
    check("remove missing is graceful", rc == 0 and "not installed" in (out+err), out+err)

    # ---- 13. module-name collision detection ----
    # 13a. A second package that would shadow an already-installed module's importable name is
    #      refused; the first install stays put.
    reset_home(); REGISTRY.clear()
    a = reg("owner1", "pkgA"); b = reg("owner2", "pkgB")
    put_manifest(a, "main", "pkgA", "1.0.0", ["shared.ki"])
    put_file(a, "main", "shared.ki", "var v='A'\n")
    put_manifest(b, "main", "pkgB", "1.0.0", ["shared.ki"])
    put_file(b, "main", "shared.ki", "var v='B'\n")
    rc, out, err = kpm("install", "owner1/pkgA")
    check("collision setup: first install ok", rc == 0, err)
    rc, out, err = kpm("install", "owner2/pkgB")
    msg = (out + err).lower()
    check("cross-package collision rejected", rc != 0 and "already provided by" in msg, out + err)
    check("collision leaves the loser uninstalled", not os.path.exists(pkgdir("pkgB")))
    check("collision keeps the winner intact",
          os.path.exists(os.path.join(pkgdir("pkgA"), "shared.ki")))

    # 13b. A single manifest that declares two module paths mapping to the same importable name
    #      (e.g. `foo.bar.ki` and `foo/bar.ki` both collapse to `import("foo.bar")`) is refused
    #      before any files are written.
    reset_home(); REGISTRY.clear()
    d = reg("owner", "dup")
    put_manifest(d, "main", "dup", "1.0.0", ["foo.bar.ki", "foo/bar.ki"])
    put_file(d, "main", "foo.bar.ki", "var v=1\n")
    put_file(d, "main", "foo/bar.ki", "var v=2\n")
    # Both modules collapse to importable name "foo.bar" — reject.
    rc, out, err = kpm("install", "owner/dup")
    msg = (out + err).lower()
    check("intra-manifest duplicate rejected",
          rc != 0 and "same importable name" in msg, out + err)
    check("intra-manifest duplicate wrote nothing", not os.path.exists(pkgdir("dup")))

    # 13c. A dependency chain whose two packages would install the same importable name is caught
    #      during the same install run — before any files land.
    reset_home(); REGISTRY.clear()
    top = reg("owner", "top"); dep = reg("owner", "dep")
    put_manifest(top, "main", "top", "1.0.0", ["thing.ki"], deps=["owner/dep"])
    put_file(top, "main", "thing.ki", "var v='top'\n")
    put_manifest(dep, "main", "dep", "1.0.0", ["thing.ki"])
    put_file(dep, "main", "thing.ki", "var v='dep'\n")
    rc, out, err = kpm("install", "owner/top")
    msg = (out + err).lower()
    check("intra-run collision (dep) rejected",
          rc != 0 and "already provided by" in msg, out + err)
    check("intra-run collision wrote no top", not os.path.exists(pkgdir("top")))

    # 13d. Reinstalling the SAME package is not a self-collision (excludeName path).
    reset_home(); REGISTRY.clear()
    s = reg("owner", "same")
    put_manifest(s, "main", "same", "1.0.0", ["same.ki"])
    put_file(s, "main", "same.ki", "var v=1\n")
    rc, out, err = kpm("install", "owner/same")
    check("self-reinstall setup ok", rc == 0, err)
    rc, out, err = kpm("install", "owner/same")
    check("reinstalling same package does not self-collide", rc == 0, err)

    # ---- 13e. cross-run reverse-dependency conflict is caught and the install is refused atomically.
    #      appA (installed) needs libX@^1; installing appB (needs libX@^2) must fail, changing nothing.
    reset_home(); REGISTRY.clear()
    appA = reg("r", "appA"); libX = reg("r", "libX", tags=["v1.0.0", "v2.0.0"]); appB = reg("r", "appB")
    put_manifest(appA, "main", "appA", "1.0.0", ["a.ki"], deps=["r/libX@^1.0.0"]); put_file(appA, "main", "a.ki", "var a=1\n")
    put_manifest(appB, "main", "appB", "1.0.0", ["b.ki"], deps=["r/libX@^2.0.0"]); put_file(appB, "main", "b.ki", "var b=1\n")
    for t in ["v1.0.0", "v2.0.0"]:
        put_manifest(libX, t, "libX", t[1:], ["x.ki"]); put_file(libX, t, "x.ki", "var x=1\n")
    rc, out, err = kpm("install", "r/appA")
    check("reverse-dep setup: appA + libX installed", rc == 0 and os.path.exists(pkgdir("libX")), err)
    recX = json.load(open(os.path.join(pkgdir("libX"), ".kpm.json")))
    check("reverse-dep setup: libX@1.0.0", recX.get("version") == "1.0.0", str(recX.get("version")))
    recA = json.load(open(os.path.join(pkgdir("appA"), ".kpm.json")))
    check("install record carries resolved dependencies", recA.get("dependencies") == ["r/libX@^1.0.0"], str(recA.get("dependencies")))
    rc, out, err = kpm("install", "r/appB")           # needs libX@^2, but installed appA needs libX@^1
    msg = (out + err).lower()
    check("cross-run reverse-dep conflict fails", rc != 0 and "satisfies all constraints" in msg, out + err)
    check("reverse-dep conflict wrote no appB", not os.path.exists(pkgdir("appB")))
    check("reverse-dep conflict left libX at 1.0.0",
          json.load(open(os.path.join(pkgdir("libX"), ".kpm.json"))).get("version") == "1.0.0")

    # ---- 13f. compatible constraints from two packages UNIFY to a single version satisfying both. ----
    reset_home(); REGISTRY.clear()
    p1 = reg("q", "p1"); p2 = reg("q", "p2")
    libY = reg("q", "libY", tags=["v1.0.0", "v1.5.0", "v1.8.0", "v2.0.0"])
    put_manifest(p1, "main", "p1", "1.0.0", ["p1.ki"], deps=["q/libY@^1.0.0"]); put_file(p1, "main", "p1.ki", "var a=1\n")
    put_manifest(p2, "main", "p2", "1.0.0", ["p2.ki"], deps=["q/libY@^1.5.0"]); put_file(p2, "main", "p2.ki", "var b=1\n")
    for t in ["v1.0.0", "v1.5.0", "v1.8.0", "v2.0.0"]:
        put_manifest(libY, t, "libY", t[1:], ["y.ki"]); put_file(libY, t, "y.ki", "var y=1\n")
    rc, out, err = kpm("install", "q/p1", "q/p2")     # ^1.0.0 AND ^1.5.0 -> highest common is 1.8.0
    check("compatible unify install ok", rc == 0, err)
    recY = json.load(open(os.path.join(pkgdir("libY"), ".kpm.json")))
    check("unified to 1.8.0 (satisfies ^1.0.0 and ^1.5.0)", recY.get("version") == "1.8.0", str(recY.get("version")))

    # ---- 13g. a deep transitive chain A->B->C->D resolves fully (exercises the multi-pass fixpoint). ----
    reset_home(); REGISTRY.clear()
    chain = ["cA", "cB", "cC", "cD"]
    for i, nm in enumerate(chain):
        k = reg("d", nm)
        deps = ["d/%s" % chain[i + 1]] if i + 1 < len(chain) else None
        put_manifest(k, "main", nm, "1.0.0", ["%s.ki" % nm], deps=deps)
        put_file(k, "main", "%s.ki" % nm, "var v=1\n")
    rc, out, err = kpm("install", "d/cA")
    check("deep chain install ok", rc == 0, err)
    check("deep chain installed the leaf", os.path.exists(pkgdir("cD")))
    check("deep chain installed all four", all(os.path.exists(pkgdir(n)) for n in chain))

    # ---- 13h. a self-dependency (A depends on A) terminates and installs once. ----
    reset_home(); REGISTRY.clear()
    sd = reg("d", "selfdep")
    put_manifest(sd, "main", "selfdep", "1.0.0", ["s.ki"], deps=["d/selfdep"])
    put_file(sd, "main", "s.ki", "var v=1\n")
    rc, out, err = kpm("install", "d/selfdep")
    check("self-dependency terminates", rc == 0, err)
    check("self-dependency installed", os.path.exists(pkgdir("selfdep")))

    # ---- 13i. re-resolving a package that bumped its OWN dependency must not falsely conflict with the
    #      stale dependency it previously recorded (its fresh manifest supersedes its own record). ----
    reset_home(); REGISTRY.clear()
    appc = reg("s", "appc"); lib = reg("s", "lib", tags=["v1.0.0", "v2.0.0"])
    for t in ["v1.0.0", "v2.0.0"]:
        put_manifest(lib, t, "lib", t[1:], ["l.ki"]); put_file(lib, t, "l.ki", "var l=1\n")
    put_manifest(appc, "main", "appc", "1.0.0", ["a.ki"], deps=["s/lib@^1.0.0"]); put_file(appc, "main", "a.ki", "var a=1\n")
    rc, out, err = kpm("install", "s/appc")
    recL = json.load(open(os.path.join(pkgdir("lib"), ".kpm.json")))
    check("self-bump setup: lib@1.0.0 via appc ^1", rc == 0 and recL.get("version") == "1.0.0", str(recL.get("version")))
    put_manifest(appc, "main", "appc", "1.1.0", ["a.ki"], deps=["s/lib@^2.0.0"])   # appc now needs lib@^2
    rc, out, err = kpm("install", "s/appc")           # must NOT conflict with appc's own old ^1 record
    check("self-bumped dependency does not falsely conflict", rc == 0, out + err)
    recL = json.load(open(os.path.join(pkgdir("lib"), ".kpm.json")))
    check("self-bumped dependency upgraded lib to 2.0.0", recL.get("version") == "2.0.0", str(recL.get("version")))

    # ---- 13j. a repo carrying NON-SEMVER tags (latest/nightly/…) still resolves a semver constraint —
    #      regression: semver.satisfies() throws on a non-semver version, so pickRef must skip them. ----
    reset_home(); REGISTRY.clear()
    mix = reg("m", "mixed", tags=["latest", "v1.0.0", "nightly", "v1.3.0", "release"])
    put_manifest(mix, "v1.3.0", "mixed", "1.3.0", ["m.ki"]); put_file(mix, "v1.3.0", "m.ki", "var v=1\n")
    rc, out, err = kpm("install", "m/mixed@^1.0.0")
    mp = os.path.join(pkgdir("mixed"), ".kpm.json")
    recM = json.load(open(mp)) if os.path.exists(mp) else {}
    check("non-semver tags are skipped, constraint still resolves",
          rc == 0 and recM.get("version") == "1.3.0", "rc=%d %s %s" % (rc, err, recM))

    # ---- 13k. removing a package that others depend on WARNS (does not silently break dependents). ----
    reset_home(); REGISTRY.clear()
    hostp = reg("w", "hostp"); depp = reg("w", "depp")
    put_manifest(hostp, "main", "hostp", "1.0.0", ["h.ki"], deps=["w/depp"]); put_file(hostp, "main", "h.ki", "var h=1\n")
    put_manifest(depp, "main", "depp", "1.0.0", ["d.ki"]); put_file(depp, "main", "d.ki", "var d=1\n")
    kpm("install", "w/hostp")
    rc, out, err = kpm("remove", "depp")
    check("removing a depended-on package warns", rc == 0 and "required by" in (out + err) and "hostp" in (out + err), out + err)
    check("removed anyway (warning, not a block)", not os.path.exists(pkgdir("depp")))

    # ---- 13l. a stalled host is bounded by the request timeout, not left to hang forever. The mock
    #      sleeps 5s for any /slowhost/ path; with KPM_TIMEOUT=1 the install must fail in ~1s. ----
    reset_home(); REGISTRY.clear()
    reg("slowhost", "x")
    t0 = time.time()
    rc, out, err = kpm("install", "slowhost/x", extra={"KPM_TIMEOUT": "1"})
    elapsed = time.time() - t0
    check("stalled host fails cleanly (does not hang)", rc != 0, out + err)
    check("stalled host is bounded by the timeout (well under the 5s stall)", elapsed < 4.0, "elapsed=%.1fs" % elapsed)

    # ---- 14. tag pagination: a repo with >100 tags, highest match on page 2 ----
    # Without paging kpm would only see the first 100 tags and resolve ^1.0.0 to v1.99.0; the correct
    # answer v1.149.0 lives on the second page.
    reset_home(); REGISTRY.clear()
    many_tags = ["v1.%d.0" % i for i in range(0, 150)]      # v1.0.0 .. v1.149.0 (insertion order)
    mt = reg("many", "tagspkg", tags=many_tags)
    put_manifest(mt, "v1.149.0", "tagspkg", "1.149.0", ["m.ki"]); put_file(mt, "v1.149.0", "m.ki", "var v=1\n")
    rc, out, err = kpm("install", "many/tagspkg@^1.0.0")
    check("pagination install rc==0", rc == 0, err)
    recpath = os.path.join(pkgdir("tagspkg"), ".kpm.json")
    rec = json.load(open(recpath)) if os.path.exists(recpath) else {}
    check("pagination resolves highest of >100 tags (v1.149.0)",
          rec.get("version") == "1.149.0", str(rec.get("version")))

    # ---- 15. same package NAME from a DIFFERENT source is refused (no silent clobber) ----
    reset_home(); REGISTRY.clear()
    o1 = reg("teamA", "widget"); o2 = reg("teamB", "widget")
    put_manifest(o1, "main", "widget", "1.0.0", ["w.ki"]); put_file(o1, "main", "w.ki", "var v='A'\n")
    put_manifest(o2, "main", "widget", "2.0.0", ["w.ki"]); put_file(o2, "main", "w.ki", "var v='B'\n")
    rc, out, err = kpm("install", "teamA/widget")
    check("name-source setup: first install ok", rc == 0, err)
    rc, out, err = kpm("install", "teamB/widget")
    msg = (out + err).lower()
    check("same-name different-source refused",
          rc != 0 and ("different source" in msg or "already installed from" in msg), out + err)
    rec = json.load(open(os.path.join(pkgdir("widget"), ".kpm.json")))
    check("refusal leaves the original intact", rec.get("source") == "teamA/widget", str(rec.get("source")))
    # ... but reinstalling the SAME source is still fine (the update path must not trip the guard).
    rc, out, err = kpm("install", "teamA/widget")
    check("same-source reinstall still allowed", rc == 0, err)

    # ---- 16. a corrupt .kpm.json is skipped, not fatal (list/outdated stay usable) ----
    reset_home(); REGISTRY.clear()
    gp = reg("z", "goodpkg"); put_manifest(gp, "main", "goodpkg", "1.0.0", ["m.ki"]); put_file(gp, "main", "m.ki", "var v=1\n")
    kpm("install", "z/goodpkg")
    os.makedirs(pkgdir("brokenpkg"), exist_ok=True)
    with open(os.path.join(pkgdir("brokenpkg"), ".kpm.json"), "w") as bf:
        bf.write("{ this is not valid json ")
    rc, out, err = kpm("list")
    check("list survives a corrupt record", rc == 0 and "goodpkg" in out, out + err)
    rc, out, err = kpm("outdated")
    check("outdated survives a corrupt record", rc == 0, out + err)

finally:
    SERVER.shutdown()
    shutil.rmtree(HOME, ignore_errors=True)

print("\n%d checks passed, %d failed" % (PASS, len(FAILED)))
if FAILED:
    print("FAILURES: " + ", ".join(FAILED))
    sys.exit(1)
print("ALL KPM INTEGRATION TESTS PASSED")
