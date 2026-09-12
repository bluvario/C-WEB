# C-WEB

A web framework that keeps the backend in the language you already know: C.
You write the HTML, and inside it, between `<?c ... ?>` tags, you write C.
C-WEB compiles those `.c.html` files into a native server binary — no
interpreter, no runtime, no Node-style dependency tree. Just `cc`, `ar`, and a
static library.

## What you get

- A **template compiler** (`src/template`) that turns `.c.html` into C handlers.
  Raw `<?c ... ?>` blocks run as-is; `<?c= expr ?>` and `<?h= expr ?>` write
  output, the second one HTML-escaped.
- A **HTTP/1.1 + HTTP/2 (h2c) server** (`src/http`) with keep-alive,
  chunked bodies, streaming, WebSocket, and Server-Sent Events.
- A **router** (`src/routing`) with `<name>` captures, a catch-all fallback,
  and per-route timeouts.
- **Sessions, flash messages, and CSRF protection** out of the box.
- A **middleware chain** with security headers, rate limiting, gzip, ETags,
  CORS, Basic auth, request signing, request IDs, and trusted-proxy handling.
- A simple **file-backed key/value store** (`--db`) so pages can persist state
  without standing up a database daemon.
- Optional **HTTPS** when OpenSSL is present; the server is fully functional
  in plaintext without it.
- A **CLI tool** (`build/cweb`) that scaffolds apps, builds your views dir into
  a server binary, and runs it — with a `--watch` mode that rebuilds and
  restarts on every edit.

The library is a single static archive, `build/libcweb.a`, plus one header per
module under `include/`. Apps link against it with nothing more than `cc` and
`-lz`.

## Build

You need a C11 compiler, `make`, `ar`, and zlib. OpenSSL is optional.

```sh
make          # builds build/libcweb.a and the build/cweb CLI
make test     # builds and runs the test suite
make examples # builds the login and notes apps, prints how to run them
make clean
```

## First app

```sh
make
./build/cweb new hello
cd hello
../build/cweb serve views 8080 . static
```

`cweb new` scaffolds a runnable skeleton: a `views/` directory, a layout, a
404 page, a shared partial, and a stylesheet. Editing a `.c.html` file and
refreshing the browser is the whole loop. To build a production binary
instead of running from the source tree:

```sh
cweb build views out . static
./out/server --port 8080 --db app.db --secure
```

Every page answers GET and POST with the same handler, so a form posts right
back to the page that rendered it. Sessions are shared across pages, and a
`--db` path persists them (and anything your pages store) across restarts.

## Where things live

```
src/core       version, memory helpers
src/utils      strbuf, strmap, string views, buffers, threads, JSON, url
src/http       the server: request/response, headers, sessions, middleware,
               static files, HTTP/2, TLS, WebSocket
src/routing    route matching and dispatch
src/security   escaping, path normalization, HMAC
src/template   the .c.html compiler and layout capture
src/db         file-backed key/value store
include        public headers
examples       apps you can steal from (login, notes)
tools          the cweb CLI
docs           this documentation, itself a C-WEB app
tests          one smoke test per module
```

## Documentation

The docs live in `docs/` and are themselves a C-WEB application — this
project eats its own dog food. To read them:

```sh
make docs      # or: ./build/cweb build docs/views build/docs . docs/static
./build/docs/server --port 8080
```

That serves the whole manual, including the CLI reference, the template
language, the request/response API, middleware, sessions, streaming, and the
HTTP/2 details.

## Examples

```sh
make examples
```

- `examples/login` — session-based auth, a form, a logout route, and the
  session cookie dance.
- `examples/notes` — a full CRUD app: a layout wrapper, a `--db` store, a
  form-handling page, and static assets.

Both are compiled through the cweb CLI, so they double as reference for what
the tool generates.

## Why

Web frameworks don't have to be slow, and they don't have to be written in
garbage-collected languages. C-WEB exists because writing the backend in C,
embedded in the pages it serves, turns out to be fast, debuggable, and — once
the scaffolding is right — pleasant. The whole stack is on disk in this
repository; poke at any of it.

## License

MIT. See [LICENSE](LICENSE).