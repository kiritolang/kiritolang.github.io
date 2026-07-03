# SQLDB — a networked SQL database in pure Kirito

> **Keyword-argument edition.** This is a copy of `sqldb` refactored so that *every* call site
> passes its arguments by name (`f(param = value)`) — user functions, class constructors and
> methods, built-in type methods (`xs.sort(reverse = True)`), native-object methods, and stdlib
> functions alike. It exists as an end-to-end test that Kirito's keyword arguments work uniformly
> across every kind of callable; it passes the same `test_client.py` suite as the original.


A complete, if compact, SQL database server written entirely in Kirito (`.ki`), plus a Python test
harness that drives it over TCP with functional, error, protocol, and fuzz suites. It exists both as
a substantial example program and as a **stress test for the Kirito interpreter** — every request
round-trips through Kirito's lexer/parser/evaluator, classes, collections, exceptions, string
handling, sockets, and the `json` module, so bugs and performance cliffs surface quickly. (Several
real Kirito fixes came directly out of building it — see the repo history.)

## Layout

```
lib/sql_lexer.ki    tokenizer (keywords, idents, numbers, '...' strings with '' escaping, operators)
lib/sql_parser.ki   recursive-descent parser -> statement AST Dicts
lib/sql_engine.ki   storage (Database/Table classes) + execution + type/constraint checking
server.ki           TCP server: newline-framed JSON request/response protocol over one shared DB
test_client.py      Python harness: launches the server, tests it over the network
```

## The SQL subset

- `CREATE TABLE t (col INTEGER|TEXT [PRIMARY KEY], ...)` and `DROP TABLE t`
- `INSERT INTO t [(cols)] VALUES (...), (...)` — column reordering, NULL defaults, type coercion
- `SELECT */cols/COUNT(*) FROM t [WHERE ...] [ORDER BY col ASC|DESC] [LIMIT n]`
- `UPDATE t SET col = val, ... [WHERE ...]` and `DELETE FROM t [WHERE ...]`
- `WHERE`: `= != <> < <= > >=`, `IS [NOT] NULL`, `AND`/`OR`/`NOT`, parentheses
- Types: `INTEGER` (int64) and `TEXT`; `PRIMARY KEY` uniqueness + NOT NULL enforced

## Protocol

Newline-framed. The client sends one JSON object per line `{"sql": "..."}`; the server replies with
one JSON object per line: `{"ok": true, "type": "...", ...}` on success, or
`{"ok": false, "error": "..."}` on any lex/parse/engine error. The special request
`{"sql": "__shutdown__"}` stops the server. One in-memory `Database` is shared for the server's
lifetime; the server is single-threaded and serves one connection at a time (matching Kirito's
no-threads model), but state persists across sequential connections.

## Running

```sh
# build the interpreter first
cmake --build build

# run the test harness (it launches and stops the server itself)
python3 examples/big_projects/sqldb/test_client.py            # 2000 fuzz inputs (default)
python3 examples/big_projects/sqldb/test_client.py --fuzz 200 # quicker

# or run the server by hand and talk to it yourself
ki --lib examples/big_projects/sqldb/lib examples/big_projects/sqldb/server.ki 127.0.0.1 5433
```

The harness exits non-zero if any assertion fails or the server crashes/hangs/sends a malformed
reply — so it doubles as a regression test for the interpreter.
