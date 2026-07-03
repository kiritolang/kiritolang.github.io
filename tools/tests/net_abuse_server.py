#!/usr/bin/env python3
"""Adversarial HTTP/1.1 server for probe_http_client_abuse.ki. Serves a small set of misbehaving
endpoints so the Kirito `net` client can be tested against them:

    /notfound       — 404, valid response
    /bad-chunked    — chunked encoding with a malformed length prefix
    /slow-body      — drips 1 byte / 200ms, ignoring Content-Length correctness
    /short-body     — declares Content-Length: 1000000 but only sends 10 bytes then closes
    /loop           — 302 Location: <self>, forever (redirect loop)
    /garbage        — non-HTTP bytes on the wire

Bound to 127.0.0.1 only. Usage:  python3 net_abuse_server.py <port>
"""
import os
import socket
import sys
import threading
import time


def read_request(conn):
    """Read the request line + headers; return (path, headers) or (None, None) on EOF."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return None, None
        buf += chunk
    head, _ = buf.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    request_line = lines[0].decode(errors="replace")
    parts = request_line.split(" ")
    if len(parts) < 2:
        return None, None
    path = parts[1]
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.decode().strip().lower()] = v.decode().strip()
    return path, headers


def handle(conn, addr):
    try:
        path, headers = read_request(conn)
        if path is None:
            return
        if path == "/notfound":
            conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            return
        if path == "/bad-chunked":
            body = (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    b"NOTAHEX\r\nfoo\r\n0\r\n\r\n")
            conn.sendall(body)
            return
        if path == "/slow-body":
            # Send a modest number of bytes with an 80ms delay each. Total ≈ 1.6s — long enough to
            # trigger a 1s per-op timeout, short enough that a client which ignores the timeout
            # (streams the whole body) also finishes quickly and lets the test move on.
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n")
            i = 0
            while i < 20:
                conn.sendall(b"a")
                time.sleep(0.08)
                i += 1
            return
        if path == "/short-body":
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n")
            conn.sendall(b"only ten\r\n")
            time.sleep(0.05)
            return
        if path == "/loop":
            conn.sendall(b"HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n")
            return
        if path == "/garbage":
            conn.sendall(b"NOT AN HTTP RESPONSE AT ALL\n")
            return
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 20080
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    srv.settimeout(30.0)     # exit after 30s idle so a leaked instance dies eventually
    while True:
        try:
            conn, addr = srv.accept()
        except socket.timeout:
            break
        except OSError:
            break
        t = threading.Thread(target=handle, args=(conn, addr), daemon=True)
        t.start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
