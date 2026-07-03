#!/usr/bin/env python3
"""Drive the Kirito HTTP server over the network: functional, error, and adversarial suites.

Like the sqldb harness, this doubles as a regression/stability test for the interpreter — every
request round-trips through Kirito's lexer/parser/evaluator, classes, string handling, the `net`
sockets, and the `json` module. The harness launches the server, waits for its LISTENING banner,
runs the suites, then asks it to shut down. Exit status is non-zero on any failure or crash.

Usage:  python3 test_client.py [--ki PATH] [--port N]
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
passed = 0
failed = 0


def check(cond, label):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAIL: %s" % label)


def http(method, host, port, path, body=None, headers=None):
    url = "http://%s:%d%s" % (host, port, path)
    data = body.encode() if isinstance(body, str) else body
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(), dict(e.headers)


def raw(host, port, payload, read=True):
    """Send arbitrary bytes and return the raw reply (for adversarial tests)."""
    s = socket.create_connection((host, port), timeout=5)
    try:
        s.sendall(payload if isinstance(payload, bytes) else payload.encode())
        if not read:
            return b""
        s.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            b = s.recv(4096)
            if not b:
                break
            chunks.append(b)
        return b"".join(chunks)
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ki", default=os.path.join(HERE, "..", "..", "..", "build", "ki"))
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8077)
    args = ap.parse_args()
    ki = os.path.abspath(args.ki)
    host, port = args.host, args.port

    proc = subprocess.Popen(
        [ki, "--lib", os.path.join(HERE, "lib"), os.path.join(HERE, "server.ki"),
         host, str(port), os.path.join(HERE, "static")],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    # Wait for the readiness banner.
    ready = False
    for line in proc.stdout:
        if line.startswith("LISTENING"):
            ready = True
            break
    if not ready:
        sys.exit("server did not start")

    try:
        print("[functional]")
        st, body, hdrs = http("GET", host, port, "/")
        check(st == 200 and "Kirito web server" in body, "GET / returns the index HTML")
        check("text/html" in hdrs.get("Content-Type", ""), "index has html content-type")

        st, body, _ = http("GET", host, port, "/hello/Ada")
        check(st == 200 and body == "Hello, Ada!", "path param /hello/:name")
        st, body, _ = http("GET", host, port, "/hello/Ada?loud=1")
        check(body == "HELLO, ADA!", "query flag ?loud=1")
        # unicode path param round-trips
        st, body, _ = http("GET", host, port, "/hello/%C3%A9l%C3%A8ve")
        check(st == 200 and body == "Hello, élève!", "percent-decoded unicode path param")

        st, body, _ = http("GET", host, port, "/api/add?a=2&b=40")
        check(st == 200 and json.loads(body)["sum"] == 42, "json api /api/add")
        st, body, _ = http("GET", host, port, "/api/add?a=2")
        check(st == 400, "missing query param -> 400")

        st, body, _ = http("GET", host, port, "/api/time")
        check(st == 200 and "epoch" in json.loads(body), "json api /api/time")

        payload = json.dumps({"nested": {"a": [1, 2, 3]}, "s": "héllo"})
        st, body, _ = http("POST", host, port, "/echo", payload)
        check(st == 200 and json.loads(body)["you_sent"]["s"] == "héllo", "POST /echo round-trips JSON")

        st, body, hdrs = http("GET", host, port, "/static/about.html")
        check(st == 200 and "About this server" in body, "static file served")

        print("[errors]")
        st, _, _ = http("GET", host, port, "/does/not/exist")
        check(st == 404, "unknown route -> 404")
        st, _, _ = http("POST", host, port, "/api/time")
        check(st == 405, "wrong method -> 405")
        st, _, _ = http("POST", host, port, "/echo", "not valid json")
        check(st == 400, "bad JSON body -> 400")
        # path traversal must be refused
        st = raw(host, port, "GET /static/../server.ki HTTP/1.0\r\n\r\n")
        check(b"403" in st or b"404" in st, "path traversal refused")

        print("[adversarial]  (server must survive, never crash)")
        # malformed request lines / partial input / junk — each on its own connection.
        hostile = [
            "GET\r\n\r\n",                          # too few tokens
            "GET /\r\n\r\n",                         # missing version
            "\r\n\r\n",                              # blank
            "PUT /echo HTTP/1.1\r\nContent-Length: 999\r\n\r\nshort",  # body shorter than declared
            "GET /" + "a" * 5000 + " HTTP/1.1\r\n\r\n",                # very long URI
            "GET / HTTP/1.1\r\n" + ("X-H: y\r\n" * 200) + "\r\n",      # many headers
            "POST /echo HTTP/1.1\r\nContent-Length: -5\r\n\r\n",       # negative length
            "GET /hello/%ZZ HTTP/1.1\r\n\r\n",                         # bad percent-escape
            "garbage bytes \x00\x01\x02 not http\r\n\r\n",            # binary junk
        ]
        for h in hostile:
            try:
                raw(host, port, h)
                check(True, "survived hostile input: %r" % h[:30])
            except Exception as e:
                check(False, "hostile input crashed connection: %r (%s)" % (h[:30], e))
        # after all that abuse, a normal request still works (server is alive)
        st, body, _ = http("GET", host, port, "/hello/again")
        check(st == 200 and body == "Hello, again!", "server still healthy after adversarial inputs")

    finally:
        try:
            http("GET", host, port, "/__shutdown__")
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    print("\n%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
