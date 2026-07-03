# http_client — a real HTTP app on Kirito's `net` library

A companion **server** and **client** that together exercise the full `net` HTTP client end to end,
plus a Python harness that runs them as an integration test.

- `server.ki` — a small HTTP/1.1 server (raw `net.Socket`) with endpoints for text, JSON, an echo,
  a redirect, cookies, basic auth, query params, and `/shutdown`. Prints `LISTENING <port>`.
- `client.ki` — a real client using the unified HTTP API: `net.get`/`post`/`request`, the `Response`
  object (`.status`/`.ok`/`.text`/`.json()`/`.header()`/`.raiseforstatus()`), redirect following,
  basic `auth`, query `params`, JSON and form bodies, and a `net.Session()` that persists cookies.
- `test_client.py` — launches the server, runs the client against it, and asserts the client reports
  `ALL OK` and exits cleanly.

## Running

```sh
cmake --build build --target ki
python3 examples/http_client/test_client.py --ki build/ki
```

It launches both processes on an ephemeral port and prints `PASS: <n> checks, client clean`. Every
request round-trips through Kirito's sockets, HTTP client, `json` module, classes and string
handling on both ends — so it doubles as a stability test of the interpreter and the `net` library.
