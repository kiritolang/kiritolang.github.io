#!/usr/bin/env python3
"""Minimal TLS/HTTPS server for spec_net_tls.ki. Serves a fixed `200 OK` (body "tls-ok") over TLS
using a cert/key pair passed on the command line, so the Kirito `net` client can drive a real HTTPS
handshake against a known endpoint. Bound to 127.0.0.1 only. Per-connection handshake/IO errors are
swallowed (so the TCP-readiness probe the test uses — a plain connect that never completes the TLS
handshake — doesn't take the server down).

    Usage:  python3 net_tls_server.py <port> <certfile> <keyfile>
"""
import socket
import ssl
import sys
import threading


def handle(conn, ctx):
    try:
        tls = ctx.wrap_socket(conn, server_side=True)
    except (ssl.SSLError, OSError):
        try:
            conn.close()
        except OSError:
            pass
        return
    try:
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = tls.recv(4096)
            if not chunk:
                break
            data += chunk
        body = b"tls-ok"
        resp = (b"HTTP/1.1 200 OK\r\nContent-Length: " + str(len(body)).encode() +
                b"\r\nConnection: close\r\n\r\n" + body)
        tls.sendall(resp)
    except (ssl.SSLError, OSError):
        pass
    finally:
        try:
            tls.close()
        except OSError:
            pass


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: net_tls_server.py <port> <certfile> <keyfile>")
    port = int(sys.argv[1])
    certfile, keyfile = sys.argv[2], sys.argv[3]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=handle, args=(conn, ctx), daemon=True).start()


if __name__ == "__main__":
    main()
