#!/usr/bin/env python3
"""Concurrency stress for the Kirito SQL server (actor model under the `parallel` module).

Launches the server and hits it with K clients **simultaneously**, each interleaving INSERTs and
SELECTs on a shared table. Because every DB access is serialized through the single owner VM, the
final state must be exactly consistent regardless of timing: assert aggregate invariants (every
insert landed, no lost writes), never an exact interleaving. A hang or crash under load is a failure.

Usage:  python3 test_concurrent.py [--ki PATH] [--port N] [--clients K] [--rows R]
Exit 0 iff all invariants held and the server stayed healthy.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


class Client:
    def __init__(self, host, port, timeout=30):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def request(self, sql):
        self.sock.sendall((json.dumps({"sql": sql}) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("server closed connection mid-reply")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


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
    ap.add_argument("--port", type=int, default=5544)
    ap.add_argument("--clients", type=int, default=8)
    ap.add_argument("--rows", type=int, default=50)
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

        setup = Client(host, port)
        setup.request("CREATE TABLE acct (id INTEGER, who TEXT, amt INTEGER)")
        setup.close()

        errors = []
        K, R = args.clients, args.rows

        def worker(cid):
            try:
                c = Client(host, port)
                for i in range(R):
                    rid = cid * R + i
                    res = c.request("INSERT INTO acct VALUES (%d, 'c%d', %d)" % (rid, cid, i))
                    if not res.get("ok"):
                        errors.append("client %d insert %d failed: %r" % (cid, i, res))
                    # interleave a read so writers and readers contend on the owner
                    res = c.request("SELECT COUNT(*) FROM acct")
                    if not res.get("ok"):
                        errors.append("client %d select failed: %r" % (cid, res))
                c.close()
            except Exception as e:  # noqa: BLE001
                errors.append("client %d raised %r" % (cid, e))

        threads = [threading.Thread(target=worker, args=(c,)) for c in range(K)]
        t0 = time.time()
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=60)
        elapsed = time.time() - t0
        if any(t.is_alive() for t in threads):
            print("FAIL: a client thread hung (possible deadlock)")
            failed += 1

        for e in errors:
            print("FAIL:", e)
            failed += len(errors)

        # Invariant: every insert is present exactly once — K*R rows, no lost writes.
        check = Client(host, port)
        res = check.request("SELECT COUNT(*) FROM acct")
        check.close()
        rows = res["count"] if res.get("ok") else None
        expected = K * R
        if rows != expected:
            print("FAIL: row count %r != expected %d (lost/duplicated writes)" % (rows, expected))
            failed += 1
        else:
            print("OK: %d clients x %d rows = %d rows, consistent (%.2fs)" % (K, R, expected, elapsed))
    except Exception as e:  # noqa: BLE001
        print("FAIL:", e)
        failed += 1
    finally:
        try:
            cc = Client("127.0.0.1", args.port, timeout=3)
            cc.request("__shutdown__")
            cc.close()
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
