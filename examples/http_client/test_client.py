#!/usr/bin/env python3
"""Test harness for the http_client example: launch server.ki, run client.ki against it, and assert
the client's full HTTP exchange (GET/POST/JSON/redirect/auth/params/session) succeeds.

Doubles as an integration test of the net library and the interpreter: every request round-trips
through Kirito's sockets, HTTP client, json module, classes, and string handling on both ends.

Usage:  python3 test_client.py [--ki PATH]
Exit code 0 iff the client printed "ALL OK" and exited cleanly.
"""
import argparse
import os
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ki", default=os.path.join(HERE, "..", "..", "build", "ki"))
    args = ap.parse_args()
    ki = os.path.abspath(args.ki)
    port = str(free_port())

    server = subprocess.Popen([ki, os.path.join(HERE, "server.ki"), port],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        # wait for the LISTENING banner
        ready = False
        for _ in range(200):
            line = server.stdout.readline()
            if not line:
                break
            if line.startswith("LISTENING"):
                ready = True
                break
        if not ready:
            print("server failed to start"); return 1

        client = subprocess.run([ki, os.path.join(HERE, "client.ki"), port],
                                capture_output=True, text=True, timeout=60)
        out = client.stdout
        sys.stdout.write(out)
        if client.returncode != 0:
            sys.stderr.write(client.stderr)
            print("FAIL: client exited %d" % client.returncode); return 1
        if "ALL OK" not in out:
            print("FAIL: client did not report ALL OK"); return 1
        n = out.count("\nok ") + (1 if out.startswith("ok ") else 0)
        print("PASS: %d checks, client clean" % n)
        return 0
    finally:
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.terminate()


if __name__ == "__main__":
    sys.exit(main())
