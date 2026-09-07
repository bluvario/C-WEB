# C-WEB

**THIS SOFTWARE IS UNFINISHED!!! Don't have any high expectations.**

C-WEB is a web framework where you write your backend in C, embedded directly
inside your HTML files (`.c.html`). The framework compiles those files into a
native server with a built-in HTTP stack, template engine, router and
(one day) sessions, middleware and a database layer.

Right now it is a glorified version string. I wanted to see whether a web
framework written in C could actually be pleasant to use instead of the usual
GCC-and-socket-foreplay ritual. Remains to be seen.

## Building

```sh
make            # builds build/libcweb.a
make test       # builds and runs the smoke test
make clean
```

That's it. You have `cc`, `ar` and a pulse. Good.

## Layout

```
src/core       core engine
src/http       HTTP server
src/template   template parsing/rendering
src/routing    routes
src/security   memory safety, escaping, auth
src/db         database abstractions
src/utils      sundry helpers
include        public headers (cweb.h)
examples       apps you can steal from
tests          smoke tests
docs           shameful confessions
tools          dev scripts
```

## TODO

- [ ] Everything past the version string
- [ ] Params, forms, sessions, middleware
- [ ] An HTTP server that speaks more than TCP