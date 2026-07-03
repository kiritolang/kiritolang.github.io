#!/usr/bin/env python3
"""Regular + adversarial test harness for the Kirito SQL server.

Launches the server (ki --lib lib server.ki), connects over TCP, and drives it through:
  - a functional suite asserting correct query results, and
  - an adversarial suite throwing malformed/hostile/edge-case input at it, asserting that the server
    always replies with a well-formed JSON error (and never crashes, hangs, or corrupts state).

The whole point is to stress the Kirito interpreter through a large, real program: every request
round-trips through Kirito's lexer/parser/evaluator, the json module, sockets, classes, exceptions,
collections, and string handling. A server crash (connection reset / non-JSON reply / timeout) is a
test failure and very likely a Kirito bug.

Usage:  python3 test_client.py [--ki PATH] [--port N] [--fuzz N]
Exit code 0 iff every assertion passed and the server stayed healthy throughout.
"""
import argparse
import json
import os
import random
import socket
import string
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


class ServerCrash(Exception):
    pass


class Client:
    def __init__(self, host, port, timeout=20):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def request(self, sql):
        """Send one SQL request, return the parsed JSON reply dict."""
        try:
            self.sock.sendall((json.dumps({"sql": sql}) + "\n").encode())
        except (BrokenPipeError, ConnectionResetError) as e:
            raise ServerCrash("send failed (server died): %r" % e)
        # read until newline
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, ConnectionResetError) as e:
                raise ServerCrash("recv failed/timed out (server hung or crashed): %r" % e)
            if not chunk:
                raise ServerCrash("server closed the connection mid-reply")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        try:
            return json.loads(line.decode())
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            raise ServerCrash("server sent a non-JSON reply %r: %s" % (line[:200], e))

    def request_raw(self, raw_bytes):
        """Send arbitrary bytes (already including any framing) and read one reply line."""
        try:
            self.sock.sendall(raw_bytes)
        except (BrokenPipeError, ConnectionResetError) as e:
            raise ServerCrash("send failed (server died): %r" % e)
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, ConnectionResetError) as e:
                raise ServerCrash("recv failed/timed out: %r" % e)
            if not chunk:
                raise ServerCrash("server closed the connection mid-reply")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Runner:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, cond, msg):
        if cond:
            self.passed += 1
        else:
            self.failed += 1
            print("  FAIL: " + msg)

    def eq(self, got, want, msg):
        self.check(got == want, "%s (got %r, want %r)" % (msg, got, want))


def functional_tests(c, r):
    """Correctness of the SQL engine over the network."""
    print("[functional]")
    # schema + inserts
    r.check(c.request("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")["ok"],
            "create table")
    ins = c.request("INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25), (3, 'Cara', 35), (4, 'Dan', 25)")
    r.eq(ins.get("count"), 4, "insert 4 rows")

    # select all
    res = c.request("SELECT * FROM users")
    r.eq(len(res["rows"]), 4, "select all count")
    r.eq(res["columns"], ["id", "name", "age"], "select columns")

    # where + ordering
    res = c.request("SELECT name FROM users WHERE age >= 30 ORDER BY age DESC")
    r.eq([row["name"] for row in res["rows"]], ["Cara", "Alice"], "where+order")

    # ordering with a tie, then by name implicitly stable
    res = c.request("SELECT name FROM users WHERE age = 25 ORDER BY name ASC")
    r.eq([row["name"] for row in res["rows"]], ["Bob", "Dan"], "tie order by name")

    # count
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age = 25")["count"], 2, "count where")

    # limit
    r.eq(len(c.request("SELECT * FROM users ORDER BY id LIMIT 2")["rows"]), 2, "limit")

    # comparison operators
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age != 25")["count"], 2, "!=")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age < 30")["count"], 2, "<")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age <= 30")["count"], 3, "<=")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE name = 'Alice'")["count"], 1, "string =")

    # AND / OR / NOT / parentheses
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age = 25 AND name = 'Bob'")["count"], 1, "AND")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age = 35 OR age = 25")["count"], 3, "OR")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE NOT age = 25")["count"], 2, "NOT")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE (age = 25 OR age = 35) AND id > 2")["count"], 2, "parens")

    # update
    upd = c.request("UPDATE users SET age = 26 WHERE name = 'Bob'")
    r.eq(upd.get("count"), 1, "update count")
    r.eq(c.request("SELECT age FROM users WHERE id = 2")["rows"][0]["age"], 26, "update applied")

    # NULL handling
    c.request("INSERT INTO users VALUES (5, 'Eve', NULL)")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age IS NULL")["count"], 1, "is null")
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age IS NOT NULL")["count"], 4, "is not null")
    # comparisons with NULL are never true
    r.eq(c.request("SELECT COUNT(*) FROM users WHERE age > 0")["count"], 4, "null excluded from >")

    # delete
    dele = c.request("DELETE FROM users WHERE age = 25")
    r.eq(dele.get("count"), 1, "delete count")
    r.eq(c.request("SELECT COUNT(*) FROM users")["count"], 4, "after delete")

    # explicit-column insert with reordering
    c.request("CREATE TABLE kv (k TEXT PRIMARY KEY, v INTEGER)")
    c.request("INSERT INTO kv (v, k) VALUES (10, 'ten')")
    r.eq(c.request("SELECT v FROM kv WHERE k = 'ten'")["rows"][0]["v"], 10, "reordered insert")

    # drop
    r.check(c.request("DROP TABLE kv")["ok"], "drop table")
    r.check(not c.request("SELECT * FROM kv")["ok"], "select from dropped fails")


def error_tests(c, r):
    """Every malformed/invalid request must yield ok:false with an error message, never a crash."""
    print("[errors]")
    cases = [
        "SELCT * FROM users",                       # misspelled keyword
        "SELECT * FROM",                            # truncated
        "SELECT * FROM nonexistent_table",          # unknown table
        "SELECT nope FROM users",                   # unknown column
        "INSERT INTO users VALUES (1)",             # arity mismatch
        "INSERT INTO users VALUES (1, 'x', 'notanint')",  # type error
        "INSERT INTO users VALUES (1, 'Alice', 30)",      # duplicate PK (id 1 exists)
        "CREATE TABLE users (id INTEGER)",          # table exists
        "CREATE TABLE bad (id INTEGER PRIMARY KEY, id2 INTEGER PRIMARY KEY)",  # two PKs
        "SELECT * FROM users WHERE",                # dangling WHERE
        "SELECT * FROM users WHERE age >",          # dangling operator
        "SELECT * FROM users ORDER BY nope",        # bad order column
        "UPDATE users SET nope = 1",                # unknown column update
        "DELETE FROM users WHERE age = 'x' AND",    # trailing AND
        "'unterminated",                            # lexer: unterminated string
        "SELECT * FROM users; DROP TABLE users",    # trailing tokens after ;
        "",                                         # empty (after strip handled server-side)
        "   ",                                      # whitespace only
        "SELECT * FROM users LIMIT -5",             # negative limit
        "SELECT * FROM users WHERE id = 1 = 1",     # malformed comparison
    ]
    for sql in cases:
        reply = c.request(sql)
        r.check(isinstance(reply, dict) and reply.get("ok") is False and "error" in reply,
                "error-case must report ok:false: %r -> %r" % (sql[:40], reply))
    # the server is still healthy and consistent after all those errors
    r.check(c.request("SELECT COUNT(*) FROM users")["ok"], "server healthy after error storm")


def protocol_tests(c, host, port, r):
    """Hostile/edge-case behavior at the protocol (envelope) level."""
    print("[protocol]")
    # non-object JSON envelopes
    for raw in [b"42\n", b"\"hello\"\n", b"[1,2,3]\n", b"null\n", b"true\n"]:
        reply = c.request_raw(raw)
        r.check(reply.get("ok") is False, "non-object envelope rejected: %r" % raw)
    # object without 'sql'
    r.check(c.request_raw(b'{"notql": "x"}\n').get("ok") is False, "missing sql field")
    # malformed JSON envelope
    r.check(c.request_raw(b'{"sql": \n').get("ok") is False, "truncated JSON envelope")
    r.check(c.request_raw(b'{not json}\n').get("ok") is False, "garbage JSON envelope")
    # a long SQL string (many OR clauses) must be handled in reasonable time, not crash/hang
    big = "SELECT * FROM users WHERE " + " OR ".join(["id = %d" % i for i in range(800)])
    reply = c.request(big)
    r.check(isinstance(reply, dict) and "ok" in reply, "long WHERE handled")
    # deeply parenthesized expression (stress the interpreter's nesting guards): either parses or
    # raises a clean error, but never crashes the server.
    deep = "SELECT * FROM users WHERE " + "(" * 300 + "id = 1" + ")" * 300
    reply = c.request(deep)
    r.check(isinstance(reply, dict) and "ok" in reply, "deeply nested WHERE handled (no crash)")
    # after all the hostile envelopes, the same connection still serves valid requests correctly
    r.check(c.request("SELECT COUNT(*) FROM users")["ok"], "connection healthy after hostile envelopes")


def fuzz_tests(c, r, n, seed=1234):
    """Throw random byte/string SQL at the server; it must always answer with valid JSON."""
    print("[fuzz] %d random inputs" % n)
    rng = random.Random(seed)
    alphabet = string.ascii_letters + string.digits + " ,;()='*<>!_." + "\t"
    keywords = ["SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "users", "id", "name",
                "age", "*", "AND", "OR", "NOT", "=", "NULL", "(", ")", ",", "30", "'x'", "ORDER",
                "BY", "LIMIT", "COUNT", "DELETE", "UPDATE", "SET", "CREATE", "TABLE", "DROP"]
    for i in range(n):
        mode = rng.randint(0, 2)
        if mode == 0:
            sql = "".join(rng.choice(alphabet) for _ in range(rng.randint(0, 60)))
        elif mode == 1:
            sql = " ".join(rng.choice(keywords) for _ in range(rng.randint(1, 14)))
        else:
            # mostly-valid with a random mutation
            base = "SELECT * FROM users WHERE age = 30"
            j = rng.randint(0, len(base) - 1)
            sql = base[:j] + rng.choice(alphabet) + base[j + 1:]
        reply = c.request(sql)  # any exception here propagates as a ServerCrash -> failure
        r.check(isinstance(reply, dict) and "ok" in reply,
                "fuzz input must get a structured reply: %r -> %r" % (sql[:40], reply))
    r.check(c.request("SELECT COUNT(*) FROM users")["ok"], "server healthy after fuzzing")


def wait_for_listen(proc, logpath, timeout=20):
    start = time.time()
    while time.time() - start < timeout:
        if proc.poll() is not None:
            raise ServerCrash("server exited early (rc=%s)\n%s" %
                              (proc.returncode, open(logpath).read()))
        try:
            with open(logpath) as f:
                for line in f:
                    if line.startswith("LISTENING"):
                        parts = line.split()
                        return parts[1], int(parts[2])
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    raise ServerCrash("server did not announce LISTENING within %ds" % timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ki", default=os.path.join(HERE, "..", "..", "..", "build", "ki"))
    ap.add_argument("--port", type=int, default=5466)
    ap.add_argument("--fuzz", type=int, default=2000)
    args = ap.parse_args()

    ki = os.path.abspath(args.ki)
    if not os.path.exists(ki):
        sys.exit("ki interpreter not found at %s (build it first, or pass --ki)" % ki)

    logpath = os.path.join(HERE, "_server.log")
    log = open(logpath, "w")
    proc = subprocess.Popen([ki, "--lib", os.path.join(HERE, "lib"),
                             os.path.join(HERE, "server.ki"), "127.0.0.1", str(args.port)],
                            stdout=log, stderr=subprocess.STDOUT)
    r = Runner()
    rc = 1
    try:
        host, port = wait_for_listen(proc, logpath)
        print("server listening on %s:%d (pid %d)" % (host, port, proc.pid))
        c = Client(host, port)
        functional_tests(c, r)
        error_tests(c, r)
        protocol_tests(c, host, port, r)
        fuzz_tests(c, r, args.fuzz)
        c.close()
        # The server is sequential (one connection at a time, no threads), but state persists across
        # connections: reconnect and confirm a prior write is still visible.
        c3 = Client(host, port)
        r.check(c3.request("SELECT COUNT(*) FROM users")["ok"], "reconnect sees persisted state")
        c3.close()
        rc = 0 if r.failed == 0 else 1
    except ServerCrash as e:
        print("SERVER CRASH / PROTOCOL FAILURE: %s" % e)
        r.failed += 1
        rc = 1
    finally:
        # ask the server to shut down cleanly, then ensure it's gone
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

    print("\n%d passed, %d failed" % (r.passed, r.failed))
    if proc.returncode not in (0, None) and rc == 0:
        # server should have exited 0 after __shutdown__; a non-zero exit means a Kirito error/crash
        print("WARNING: server exited with code %s" % proc.returncode)
        print(open(logpath).read())
        rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
