# Web server — an HTTP/1.1 server and tiny web framework in pure Kirito

A complete (if compact) HTTP server and a small Sinatra/Flask-style routing framework, written
entirely in Kirito (`.ki`), plus a Python harness that drives it over the network. Like the
[sqldb](../sqldb) example it doubles as a **stress test for the interpreter**: every request
round-trips through Kirito's lexer/parser/evaluator, classes, string handling, the `net` sockets,
and the `json` module — so bugs and rough edges surface quickly. (Several were found and fixed while
building it — see below.)

## Layout

```
lib/http.ki        buffered connection Reader, request parsing, the Response builder
lib/framework.ki   App: method+path routing (with :name params), middleware, static files
lib/app.ki         the demo application — create(static_dir) returns a configured App
server.ki          entry point: bind/listen/accept loop, dispatch, Connection: close
static/about.html  a static file to serve
test_client.py     Python harness: launches the server, tests it over HTTP
```

## What it does

- **Routing** by method + path, with `:name` path parameters (`/hello/:name`) and query strings
  (`?loud=1`), exposed to handlers as `req.params` and `req.query`.
- **Responses**: `Response().text(...)`, `.html(...)`, `.json(value, status)`, custom headers.
- **Middleware** run before every handler (the demo logs each request to stderr).
- **Static files** under a URL prefix, with content-type by extension and path-traversal refused.
- **Robust errors**: a missing route is `404`, a wrong method `405`, a bad JSON body `400`, and any
  handler exception becomes a `500` — the accept loop never dies on a bad request.

The server is **concurrent**: a stateless pool of connection-worker VMs (via the `parallel` module)
serves many clients at once, each worker owning its own read-only `App` (the same `parallel`
foundation as the sqldb server, minus the shared-state owner since handlers are pure).

## Running

```sh
cmake --build build                                  # build the interpreter first

# run the harness (launches and stops the server itself)
python3 examples/big_projects/webserver/test_client.py

# or run it by hand and browse to http://127.0.0.1:8080/
ki --lib examples/big_projects/webserver/lib \
   examples/big_projects/webserver/server.ki 127.0.0.1 8080 \
   examples/big_projects/webserver/static
```

Demo routes: `GET /`, `GET /hello/:name[?loud=1]`, `GET /api/time`, `GET /api/add?a=&b=`,
`POST /echo` (echoes a JSON body), and `GET /static/...`. `GET /__shutdown__` stops the server.

## Interpreter issues this surfaced (all fixed)

- **`Content-Length` must be a byte count, not a code-point count.** `len(s)` counts Unicode code
  points, so any response containing multibyte characters (e.g. `é`) was truncated. Fixed in
  `http.ki` by measuring the UTF-8 byte length via `BytesIO` (`byte_len`). This points at a missing
  language facility: a direct "byte length of a String".
- **Block-bodied inline functions can't be call arguments.** Only single-expression
  `Function(x): expr` works inline; a multi-line handler must be bound to a name first (the
  block-bodied-vs-inline function split). `app.ki` defines each handler as a named function — see its header note.

## Known limitations

- One request per connection (`Connection: close`); no keep-alive, chunked encoding, or HTTPS.
- Static files are read in text mode, so binary assets (images) aren't byte-faithful yet.
- The worker pool is fixed-size (`parallel.cpucount()` workers); there is no per-request thread.
