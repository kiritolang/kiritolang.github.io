#!/usr/bin/env python3
"""Concurrency stress for the Kirito HTTP server (a stateless worker pool under `parallel`).

Fires K clients **simultaneously**, each issuing M `GET /hello/<id>` requests, and asserts every
response is a correct `200 Hello, <id>!`. Then it interleaves a burst of malformed requests with live
traffic and asserts the server keeps serving good requests (a worker fault must not take the pool
down). A hang or a dropped/garbled response is a failure.

Usage:  python3 test_concurrent.py [--ki PATH] [--port N] [--clients K] [--reqs M]
Exit 0 iff every request was served correctly and the server stayed healthy.
"""
import argparse
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def http_get(host, port, path, timeout=20):
    s = socket.create_connection((host, port), timeout=timeout)
    try:
        s.sendall(("GET %s HTTP/1.0\r\nHost: x\r\n\r\n" % path).encode())
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return data.decode("latin-1")
    finally:
        s.close()


def raw(host, port, payload, timeout=20):
    s = socket.create_connection((host, port), timeout=timeout)
    try:
        s.sendall(payload.encode())
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return data.decode("latin-1")
    finally:
        s.close()


def wait_for_listen(proc, logpath, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("server exited early (rc=%s)" % proc.returncode)
        try:
            with open(logpath) as f:
                for line in f:
                    if line.startswith("LISTENING"):
                        parts = line.split()
                        return parts[1], int(parts[2])
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not announce LISTENING within %ds" % timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ki", default=os.environ.get("KI", "ki"))
    ap.add_argument("--port", type=int, default=8155)
    ap.add_argument("--clients", type=int, default=8)
    ap.add_argument("--reqs", type=int, default=25)
    args = ap.parse_args()
    ki = os.path.abspath(args.ki)

    logpath = os.path.join(HERE, "_concurrent.log")
    log = open(logpath, "w")
    proc = subprocess.Popen([ki, "--lib", os.path.join(HERE, "lib"),
                             os.path.join(HERE, "server.ki"), "127.0.0.1", str(args.port)],
                            stdout=log, stderr=subprocess.STDOUT)
    failed = 0
    try:
        host, port = wait_for_listen(proc, logpath)
        print("server listening on %s:%d (pid %d)" % (host, port, proc.pid))
        K, M = args.clients, args.reqs
        errors = []

        def worker(cid):
            try:
                for i in range(M):
                    name = "c%d_%d" % (cid, i)
                    resp = http_get(host, port, "/hello/" + name)
                    if "200" not in resp.split("\r\n", 1)[0] or ("Hello, " + name + "!") not in resp:
                        errors.append("client %d req %d bad response: %r" % (cid, i, resp[:120]))
            except Exception as e:  # noqa: BLE001
                errors.append("client %d raised %r" % (cid, e))

        threads = [threading.Thread(target=worker, args=(c,)) for c in range(K)]
        t0 = time.time()
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=60)
        if any(t.is_alive() for t in threads):
            print("FAIL: a client thread hung (possible deadlock)")
            failed += 1
        for e in errors:
            print("FAIL:", e)
        failed += len(errors)
        if not errors:
            print("OK: %d clients x %d requests = %d served correctly (%.2fs)"
                  % (K, M, K * M, time.time() - t0))

        # Adversarial burst interleaved with good traffic: the server must survive and keep serving.
        bad = ["GET\r\n\r\n", "POST /echo HTTP/1.0\r\nContent-Length: 999\r\n\r\nx",
               "\r\n\r\n", "GARBAGE !@#$", "GET /static/../server.ki HTTP/1.0\r\n\r\n"]
        for b in bad:
            try:
                raw(host, port, b, timeout=5)
            except OSError:
                pass  # the server may close on a malformed request; it must not die
        ok = http_get(host, port, "/hello/after")
        if "200" not in ok.split("\r\n", 1)[0] or "Hello, after!" not in ok:
            print("FAIL: server unhealthy after adversarial burst: %r" % ok[:120])
            failed += 1
        else:
            print("OK: server healthy after adversarial burst")
    except Exception as e:  # noqa: BLE001
        print("FAIL:", e)
        failed += 1
    finally:
        try:
            http_get("127.0.0.1", args.port, "/__shutdown__", timeout=3)
        except OSError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()

    if proc.returncode not in (0, None):
        print("WARNING: server exited with code %s" % proc.returncode)
        failed += 1
    print("PASSED" if failed == 0 else "FAILED (%d)" % failed)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
