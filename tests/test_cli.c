#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "net.h"
#include "request_sign.h"
#include "sv.h"

#define TOOL "./build/cweb"

static void nap(void)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
    nanosleep(&ts, NULL);
}

static char wbuf[4096];

static void wfile(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fputs(data, f);
    fclose(f);
}

static int wait_for_port(int port)
{
    for (int i = 0; i < 100; i++) {
        Socket_Handle s = net_connect("127.0.0.1", port);
        if (s != -1) {
            net_close(s);
            return 0;
        }
        nap();
    }
    return -1;
}

// one request per connection; returns a heap copy of the whole response
static char *fetch(int port, const char *path)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char req[1024];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Connection: close\r\n"
             "\r\n",
             path);
    if (net_send_all(s, req, strlen(req)) != (long)strlen(req)) {
        net_close(s);
        return NULL;
    }
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap);
    long got;
    while (n < cap - 1 && (got = net_recv(s, buf + n, cap - n - 1)) > 0) {
        n += (size_t)got;
    }
    buf[n] = '\0';
    net_close(s);
    return buf;
}

// same as fetch but with an extra request header line, e.g. Accept-Encoding
static char *fetch_hdr(int port, const char *path, const char *extra_header)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char req[1200];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "%s"
             "Connection: close\r\n"
             "\r\n",
             path, extra_header != NULL ? extra_header : "");
    if (net_send_all(s, req, strlen(req)) != (long)strlen(req)) {
        net_close(s);
        return NULL;
    }
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap);
    long got;
    while (n < cap - 1 && (got = net_recv(s, buf + n, cap - n - 1)) > 0) {
        n += (size_t)got;
    }
    buf[n] = '\0';
    net_close(s);
    return buf;
}

// full control variant: any method, an optional Cookie header and an
// optional urlencoded body
static char *fetch_req(int port, const char *method, const char *path,
                       const char *cookie, const char *body)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char req[2048];
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Connection: close\r\n",
                     method, path);
    if (cookie != NULL) {
        n += snprintf(req + n, sizeof req - (size_t)n, "Cookie: %s\r\n", cookie);
    }
    if (body != NULL) {
        n += snprintf(req + n, sizeof req - (size_t)n,
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %zu\r\n",
                      strlen(body));
    }
    n += snprintf(req + n, sizeof req - (size_t)n, "\r\n");
    if (body != NULL) {
        memcpy(req + n, body, strlen(body));
        n += (int)strlen(body);
    }
    if (net_send_all(s, req, (long)n) != n) {
        net_close(s);
        return NULL;
    }
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap);
    long r;
    while (got < cap - 1 && (r = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)r;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}

// GET stamped with a fresh X-CWEB-Date / X-CWEB-Signature pair over the
// canonical facts a --signature-secret server checks; taint != 0 flips one
// hex character of the signature so the request must be refused
static char *fetch_signed(int port, const char *path, const char *secret, int taint)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    long long now = (long long)time(NULL);
    char hex[65];
    http_request_signature_compute("GET", path, strlen(path), NULL, 0, now,
                                   secret, hex, sizeof(hex));
    if (taint) {
        hex[10] = hex[10] == 'a' ? 'b' : 'a';
    }
    char req[1024];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "X-CWEB-Date: %lld\r\n"
                      "X-CWEB-Signature: %s\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, now, hex);
    if (net_send_all(s, req, rl) != rl) {
        net_close(s);
        return NULL;
    }
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap);
    long r;
    while (got < cap - 1 && (r = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)r;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}
// returns the whole response
static char *fetch_post_body(int port, const char *path, const char *body, size_t len)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char head[256];
    int hl = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, len);
    char *req = malloc((size_t)hl + len);
    memcpy(req, head, (size_t)hl);
    memcpy(req + hl, body, len);
    long sent = net_send_all(s, req, (long)((size_t)hl + len));
    free(req);
    if (sent != (long)((size_t)hl + len)) {
        net_close(s);
        return NULL;
    }
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap);
    long r;
    while (got < cap - 1 && (r = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)r;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}

// sends a partial request (no final blank line), then waits 400 ms past the
// server's --io-timeout to see whether it answers 408; returns the response
static char *fetch_stall(int port, const char *partial)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    net_send_all(s, partial, strlen(partial));
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 400000000};
    nanosleep(&ts, NULL);
    net_set_timeout(s, 2000);
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap);
    long r;
    while (got < cap - 1 && (r = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)r;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}

// pulls "<name>=<value>" out of a response's Set-Cookie header
static void grab_cookie(const char *resp, const char *name, char *out, size_t out_size)
{
    char mark[96];
    snprintf(mark, sizeof mark, "Set-Cookie: %s=", name);
    const char *at = strstr(resp, mark);
    if (at == NULL) {
        out[0] = '\0';
        return;
    }
    at += strlen(mark);
    size_t i = 0;
    while (at[i] != '\0' && at[i] != ';' && at[i] != '\r' && i + 1 < out_size) {
        out[i] = at[i];
        i++;
    }
    out[i] = '\0';
}

// pulls the value out of a rendered hidden input like
// name="csrf_token" value="<token>"
static void grab_form_token(const char *body, char *out, size_t out_size)
{
    const char *mark = strstr(body, "name=\"csrf_token\" value=\"");
    if (mark == NULL) {
        out[0] = '\0';
        return;
    }
    mark += strlen("name=\"csrf_token\" value=\"");
    size_t i = 0;
    while (mark[i] != '\0' && mark[i] != '"' && i + 1 < out_size) {
        out[i] = mark[i];
        i++;
    }
    out[i] = '\0';
}

int main(void)
{
    if (net_init() != 0) {
        return 1;
    }

    char tmp[256] = "/tmp/cweb_cli_test_XXXXXX";
    if (mkdtemp(tmp) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    char views[512], out[1024], sub[1024], staticd[1024];
    snprintf(views, sizeof views, "%s/views", tmp);
    snprintf(out, sizeof out, "%s/out", tmp);
    snprintf(sub, sizeof sub, "%s/sub", views);
    snprintf(staticd, sizeof staticd, "%s/static", tmp);
    mkdir(views, 0755);
    mkdir(sub, 0755);
    mkdir(staticd, 0755);

    snprintf(wbuf, sizeof wbuf, "%s/hello.c.html", views);
    wfile(wbuf, "<h1>Hello</h1>");
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", views);
    wfile(wbuf, "<h1>Index</h1>");
    snprintf(wbuf, sizeof wbuf, "%s/world.c.html", sub);
    wfile(wbuf, "<p>sub world</p>");
    snprintf(wbuf, sizeof wbuf, "%s/style.css", staticd);
    wfile(wbuf, "body { background: #fff; }");
    snprintf(wbuf, sizeof wbuf, "%s/404.c.html", views);
    wfile(wbuf, "<h1>Not found</h1>");
    char partiald[1024];
    snprintf(partiald, sizeof partiald, "%s/partials", views);
    mkdir(partiald, 0755);
    snprintf(wbuf, sizeof wbuf, "%s/partials/shout.c.html", views);
    wfile(wbuf, "<strong>reusable!</strong>");
    snprintf(wbuf, sizeof wbuf, "%s/mix.c.html", views);
    wfile(wbuf, "<?c page_shout(req, res, params, user_data); ?><span>mixed page</span>");
    snprintf(wbuf, sizeof wbuf, "%s/layout.c.html", views);
    wfile(wbuf,
          "<!DOCTYPE html><html><head><title>L</title></head>"
          "<body><nav>top</nav>"
          "<?c cweb_tpl_layout_emit(res); ?>"
          "<footer>bottom</footer></body></html>");

    // build the views into C and a server binary, mounting the static dir
    snprintf(wbuf, sizeof wbuf, "%s build %s %s . %s >/dev/null", TOOL, views, out, staticd);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed\n");
        return 1;
    }

    // the generated artifacts line up with the views
    char path[2048];
    const char *names[] = {"page_hello.c", "page_index.c", "page_world.c",
                           "page_404.c", "page_shout.c", "page_mix.c",
                           "page_layout.c", "pages.h", "main.c", "server"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", out, names[i]);
        if (access(path, F_OK) != 0) {
            fprintf(stderr, "missing %s\n", path);
            return 1;
        }
    }

    snprintf(path, sizeof path, "%s/main.c", out);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 1;
    }
    size_t cap = 16384, n = 0;
    char *main = malloc(cap);
    n = fread(main, 1, cap - 1, f);
    main[n] = '\0';
    fclose(f);

    int routes_ok = strstr(main, "#include \"pages.h\"") != NULL &&
                    strstr(main, "#include \"rate_limit.h\"") != NULL &&
                    strstr(main, "#include \"security.h\"") != NULL &&
                    strstr(main, "#include \"template.h\"") != NULL &&
                    strstr(main, "#include \"db.h\"") != NULL &&
                    strstr(main, "#include \"gzip.h\"") != NULL &&
                    strstr(main, "#include \"request_sign.h\"") != NULL &&
                    strstr(main, "#include \"log.h\"") != NULL &&
                    strstr(main, "cweb_tpl_capture_begin") != NULL &&
                    strstr(main, "static void cweb_wrap_hello(") != NULL &&
                    strstr(main, "static void cweb_wrap_fallback(") != NULL &&
                    strstr(main, "http_rate_limit_middleware") != NULL &&
                    strstr(main, "http_security_middleware") != NULL &&
                    strstr(main, "http_gzip_middleware") != NULL &&
                    strstr(main, "http_signature_middleware") != NULL &&
                    strstr(main, "cweb_rate_key") != NULL &&
                    strstr(main, "--secure") != NULL &&
                    strstr(main, "--gzip") != NULL &&
                    strstr(main, "--log") != NULL &&
                    strstr(main, "--static-cache") != NULL &&
                    strstr(main, "http_static_set_cache(cweb_static_cache)") != NULL &&
                    strstr(main, "--max-body") != NULL &&
                    strstr(main, "--io-timeout") != NULL &&
                    strstr(main, "--workers") != NULL &&
                    strstr(main, "--signature-secret") != NULL &&
                    strstr(main, "Http_Signature_Options sig_opts = {0};") != NULL &&
                    strstr(main, "cweb_size_arg") != NULL &&
                    strstr(main, "http_serve_config(listener, router_dispatch, &r, &cfg)") != NULL &&
                    strstr(main, "cfg.max_body = cweb_max_body;") != NULL &&
                    strstr(main, "log_set_clf(clf_file)") != NULL &&
                    strstr(main, "--db") != NULL &&
                    strstr(main, "cweb_db_open(&cweb_database_impl, db_path)") != NULL &&
                    strstr(main, "cweb_db_close(&cweb_database_impl)") != NULL &&
                    strstr(main, "Cweb_Db *cweb_database(void)") != NULL &&
                    strstr(main, "void page_hello(Http_Request") == NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/hello\", cweb_wrap_hello, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_POST, \"/hello\", cweb_wrap_hello, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/sub/world\", cweb_wrap_world, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/mix\", cweb_wrap_mix, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/index\", cweb_wrap_index, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/\", cweb_wrap_index, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"*\", cweb_wrap_fallback, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_POST, \"*\", cweb_wrap_fallback, &cweb_sessions);") != NULL &&
                    strstr(main, "static void cweb_fallback(") != NULL &&
                    strstr(main, "http_serve_static(req, res, (void *)cweb_static_root);") != NULL &&
                    strstr(main, "http_static_mount(&r, \"\", cweb_static_root);") == NULL &&
                    strstr(main, "page_shout, &cweb_sessions);") == NULL;
    free(main);
    if (!routes_ok) {
        fprintf(stderr, "generated main.c does not register the expected routes\n");
        return 1;
    }

    // pages.h forwards handlers for pages and partials alike
    snprintf(path, sizeof path, "%s/pages.h", out);
    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "missing pages.h\n");
        return 1;
    }
    main = malloc(cap);
    size_t pn = fread(main, 1, cap - 1, f);
    main[pn] = '\0';
    fclose(f);
    int headers_ok = strstr(main, "#ifndef CWEB_PAGES_H") != NULL &&
                     strstr(main, "#include \"db.h\"") != NULL &&
                     strstr(main, "void page_hello(Http_Request") != NULL &&
                     strstr(main, "void page_shout(Http_Request") != NULL &&
                     strstr(main, "void page_layout(Http_Request") != NULL &&
                     strstr(main, "Cweb_Db *cweb_database(void);") != NULL;
    free(main);
    if (!headers_ok) {
        fprintf(stderr, "generated pages.h is incomplete\n");
        return 1;
    }

    // --routes lists them without binding a port
    snprintf(wbuf, sizeof wbuf, "%s/server --routes", out);
    FILE *pr = popen(wbuf, "r");
    int has_hello = 0, has_index = 0, has_world = 0, has_static = 0, has_404 = 0, has_mix = 0, has_shout = 0, has_layout = 0;
    if (pr != NULL) {
        char line[512];
        while (fgets(line, sizeof line, pr) != NULL) {
            if (strstr(line, "/hello") != NULL) {
                has_hello = 1;
            }
            if (strstr(line, "/index") != NULL) {
                has_index = 1;
            }
            if (strstr(line, "/sub/world") != NULL) {
                has_world = 1;
            }
            if (strstr(line, "static: /*") != NULL) {
                has_static = 1;
            }
            if (strstr(line, "404: *") != NULL) {
                has_404 = 1;
            }
            if (strstr(line, "/mix") != NULL) {
                has_mix = 1;
            }
            if (strstr(line, "/shout") != NULL) {
                has_shout = 1;
            }
            if (strstr(line, "/layout") != NULL) {
                has_layout = 1;
            }
        }
        pclose(pr);
    }
    if (!has_hello || !has_index || !has_world || !has_static || !has_404 || !has_mix || has_shout || has_layout) {
        fprintf(stderr, "--routes output incomplete (layout must not be routed)\n");
        return 1;
    }

    // live server on a free port: reserve, release, hand to the child
    Socket_Handle probe = net_listen(0);
    int port = net_bound_port(probe);
    net_close(probe);

    snprintf(path, sizeof path, "%s/server", out);
    char port_arg[32];
    snprintf(port_arg, sizeof port_arg, "%d", port);
    pid_t child = fork();
    if (child == 0) {
        execl(path, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(port) != 0) {
        fprintf(stderr, "server did not come up on port %d\n", port);
        return 1;
    }

    int ok = 1;
    char *body;
    body = fetch(port, "/hello");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<body><nav>top</nav><h1>Hello</h1>") != NULL &&
         strstr(body, "<footer>bottom</footer></body></html>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /hello should be wrapped by the layout:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/sub/world");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<body><nav>top</nav><p>sub world</p>") != NULL &&
         strstr(body, "<footer>bottom</footer></body></html>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /sub/world should be wrapped by the layout:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<body><nav>top</nav><h1>Index</h1>") != NULL &&
         strstr(body, "<footer>bottom</footer></body></html>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET / should be wrapped by the layout:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/index");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<body><nav>top</nav><h1>Index</h1>") != NULL &&
         strstr(body, "<footer>bottom</footer></body></html>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /index should be wrapped by the layout:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/mix");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<body><nav>top</nav>") != NULL &&
         strstr(body, "<strong>reusable!</strong>") != NULL &&
         strstr(body, "mixed page") != NULL &&
         strstr(body, "<footer>bottom</footer></body></html>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /mix should render the partial inside the layout:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/nope");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 404") != NULL &&
         strstr(body, "<body><nav>top</nav><h1>Not found</h1>") != NULL &&
         strstr(body, "<footer>bottom</footer></body></html>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /nope should render the wrapped 404 page:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/style.css");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "text/css") != NULL && strstr(body, "#fff") != NULL &&
         strstr(body, "Cache-Control:") == NULL;
    if (!ok) {
        fprintf(stderr, "GET /style.css should come from the static mount (uncached):\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    // --- cweb new scaffolds a runnable app into a fresh dir ---
    int app_ok = 1;
    char app[512], app_out[1024];
    snprintf(app, sizeof app, "%s/app", tmp);
    snprintf(wbuf, sizeof wbuf, "%s new %s >/dev/null", TOOL, app);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb new failed\n");
        return 1;
    }
    const char *app_names[] = {"views/index.c.html", "views/404.c.html",
                               "views/layout.c.html", "views/partials/footer.c.html",
                               "static/style.css"};
    for (size_t i = 0; i < sizeof app_names / sizeof app_names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", app, app_names[i]);
        if (access(path, F_OK) != 0) {
            fprintf(stderr, "missing scaffolded %s\n", path);
            return 1;
        }
    }

    // a non-empty dir must be refused
    char taken[512];
    snprintf(taken, sizeof taken, "%s/taken", tmp);
    mkdir(taken, 0755);
    snprintf(wbuf, sizeof wbuf, "%s/keep.txt", taken);
    wfile(wbuf, "x");
    snprintf(wbuf, sizeof wbuf, "%s new %s >/dev/null 2>&1", TOOL, taken);
    if (system(wbuf) == 0) {
        fprintf(stderr, "cweb new should refuse a non-empty dir\n");
        return 1;
    }

    // the scaffold builds and serves its page, stylesheet and 404 page
    snprintf(app_out, sizeof app_out, "%s/out", app);
    snprintf(wbuf, sizeof wbuf, "%s build %s/views %s . %s/static >/dev/null",
             TOOL, app, app_out, app);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed for the scaffolded app\n");
        return 1;
    }
    probe = net_listen(0);
    int app_port = net_bound_port(probe);
    net_close(probe);
    snprintf(path, sizeof path, "%s/server", app_out);
    snprintf(port_arg, sizeof port_arg, "%d", app_port);
    child = fork();
    if (child == 0) {
        execl(path, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(app_port) != 0) {
        fprintf(stderr, "scaffolded server did not come up on port %d\n", app_port);
        return 1;
    }
    char *app_body = fetch(app_port, "/");
    app_ok = app_ok && app_body != NULL && strstr(app_body, "HTTP/1.1 200 OK") != NULL &&
             strstr(app_body, "<!DOCTYPE html>") != NULL &&
             strstr(app_body, "<main>") != NULL &&
             strstr(app_body, "cweb app") != NULL &&
             strstr(app_body, "Powered by C-WEB") != NULL;
    if (!app_ok) {
        fprintf(stderr, "scaffolded home page failed:\n%s\n", app_body ? app_body : "(connect error)");
    }
    free(app_body);
    app_body = fetch(app_port, "/style.css");
    app_ok = app_ok && app_body != NULL && strstr(app_body, "HTTP/1.1 200 OK") != NULL &&
             strstr(app_body, "text/css") != NULL;
    if (!app_ok) {
        fprintf(stderr, "scaffolded stylesheet failed:\n%s\n", app_body ? app_body : "(connect error)");
    }
    free(app_body);
    app_body = fetch(app_port, "/nope");
    app_ok = app_ok && app_body != NULL && strstr(app_body, "HTTP/1.1 404") != NULL &&
             strstr(app_body, "no such page") != NULL;
    if (!app_ok) {
        fprintf(stderr, "scaffolded 404 page failed:\n%s\n", app_body ? app_body : "(connect error)");
    }
    free(app_body);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    // --- --db PATH hands every page a persistent key-value store. a counter
    //     page bumps a value it read from the store, which must survive both
    //     the request boundary and a server restart ---
    char dbviews[1024], dbvout[1024], dbbin[2048], dbfile[1024];
    snprintf(dbviews, sizeof dbviews, "%s/dbviews", tmp);
    mkdir(dbviews, 0755);
    snprintf(wbuf, sizeof wbuf, "%s/db.c.html", dbviews);
    wfile(wbuf,
          "<?c\n"
          "Cweb_Db *db = cweb_database();\n"
          "if (db == NULL) {\n"
          "    cweb_tpl_out(res, \"<p>no database configured</p>\");\n"
          "    return;\n"
          "}\n"
          "String_View hits = cweb_db_get(db, sv_from_cstr(\"hits\"));\n"
          "long long n = 0;\n"
          "if (hits.data) {\n"
          "    sv_to_i64(hits, &n);\n"
          "}\n"
          "n++;\n"
          "char buf[32];\n"
          "snprintf(buf, sizeof buf, \"%lld\", n);\n"
          "cweb_db_put(db, sv_from_cstr(\"hits\"), sv_from_cstr(buf));\n"
          "?><p>hits=<?c= buf ?></p>\n");
    snprintf(dbvout, sizeof dbvout, "%s/dbvout", tmp);
    snprintf(wbuf, sizeof wbuf, "%s build %s %s . \"\" >/dev/null",
             TOOL, dbviews, dbvout);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed for the db app\n");
        return 1;
    }
    snprintf(dbfile, sizeof dbfile, "%s/dbstore", tmp);
    snprintf(dbbin, sizeof dbbin, "%s/server", dbvout);
    probe = net_listen(0);
    int db_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", db_port);
    child = fork();
    if (child == 0) {
        execl(dbbin, "server", "--port", port_arg, "--db", dbfile, NULL);
        _exit(127);
    }
    if (wait_for_port(db_port) != 0) {
        fprintf(stderr, "db server did not come up on port %d\n", db_port);
        return 1;
    }
    char *dbbody = fetch(db_port, "/db");
    int db_ok = dbbody != NULL && strstr(dbbody, "HTTP/1.1 200 OK") != NULL &&
                strstr(dbbody, "<p>hits=1</p>") != NULL;
    if (!db_ok) {
        fprintf(stderr, "db counter start failed:\n%s\n", dbbody ? dbbody : "(connect error)");
    }
    free(dbbody);
    dbbody = fetch(db_port, "/db");
    db_ok = db_ok && dbbody != NULL &&
            strstr(dbbody, "<p>hits=2</p>") != NULL;
    if (!db_ok) {
        fprintf(stderr, "db counter did not advance:\n%s\n", dbbody ? dbbody : "(connect error)");
    }
    free(dbbody);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    // the value was written to the file, not just held in memory
    Cweb_Db reopen;
    if (cweb_db_open(&reopen, dbfile) != 0) {
        fprintf(stderr, "db file did not persist\n");
        return 1;
    }
    String_View persisted = cweb_db_get(&reopen, sv_from_cstr("hits"));
    db_ok = db_ok && sv_equal(persisted, sv_from_cstr("2"));
    if (!db_ok) {
        fprintf(stderr, "db file contents wrong\n");
    }
    cweb_db_close(&reopen);

    // restart against the same file continues from where it stopped
    child = fork();
    if (child == 0) {
        execl(dbbin, "server", "--port", port_arg, "--db", dbfile, NULL);
        _exit(127);
    }
    if (wait_for_port(db_port) != 0) {
        fprintf(stderr, "db server did not come back up on port %d\n", db_port);
        return 1;
    }
    dbbody = fetch(db_port, "/db");
    db_ok = db_ok && dbbody != NULL && strstr(dbbody, "<p>hits=3</p>") != NULL;
    if (!db_ok) {
        fprintf(stderr, "db counter did not survive a restart:\n%s\n", dbbody ? dbbody : "(connect error)");
    }
    free(dbbody);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    // without --db the accessor stays NULL and the page's guard branch runs
    child = fork();
    if (child == 0) {
        execl(dbbin, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(db_port) != 0) {
        fprintf(stderr, "db-less server did not come up on port %d\n", db_port);
        return 1;
    }
    dbbody = fetch(db_port, "/db");
    db_ok = db_ok && dbbody != NULL && strstr(dbbody, "HTTP/1.1 200 OK") != NULL &&
            strstr(dbbody, "no database configured") != NULL;
    if (!db_ok) {
        fprintf(stderr, "db-less server should render the guard branch:\n%s\n", dbbody ? dbbody : "(connect error)");
    }
    free(dbbody);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    if (!db_ok) {
        return 1;
    }

    // --- --rate caps each client at N requests a minute, --secure stamps
    //     hardening headers on every response including the 429s ---
    probe = net_listen(0);
    int rl_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", rl_port);
    child = fork();
    if (child == 0) {
        execl(path, "server", "--port", port_arg, "--rate", "2", "--secure", NULL);
        _exit(127);
    }
    if (wait_for_port(rl_port) != 0) {
        fprintf(stderr, "rate-limited server did not come up on port %d\n", rl_port);
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        return 1;
    }
    int rl_ok = 0, rl_ok_secure = 0, rl_429 = 0, rl_429_secure = 0;
    for (int i = 0; i < 10; i++) {
        char *rl_body = fetch(rl_port, "/");
        if (rl_body == NULL) {
            continue;
        }
        int hard = strstr(rl_body, "X-Content-Type-Options: nosniff") != NULL &&
                   strstr(rl_body, "X-Frame-Options: DENY") != NULL &&
                   strstr(rl_body, "Content-Security-Policy: ") != NULL;
        if (strstr(rl_body, "HTTP/1.1 200 OK") != NULL) {
            rl_ok++;
            rl_ok_secure += hard;
        } else if (strstr(rl_body, "429") != NULL) {
            rl_429++;
            rl_429_secure += hard;
        }
        free(rl_body);
    }
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    if (rl_ok_secure < 1 || rl_429_secure < 1 || rl_429 < 5) {
        fprintf(stderr,
                "--rate/--secure should pass a few requests (with hardening "
                "headers), then answer 429 (also hardened); got %d ok (%d "
                "hardened), %d rate-limited (%d hardened)\n",
                rl_ok, rl_ok_secure, rl_429, rl_429_secure);
        return 1;
    }

    // --- cweb serve forwards FLAGS... to the generated server ---
    // a non-watch serve forks the CLI which execs the server; served with
    // --rate 2 --secure the first budget of requests must pass hardened and
    // anything beyond must come back 429, also hardened
    char sf[1024], ss[1024];
    snprintf(sf, sizeof sf, "%s/sf", tmp);
    mkdir(sf, 0755);
    snprintf(ss, sizeof ss, "%s/ss", tmp);
    mkdir(ss, 0755);
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", sf);
    wfile(wbuf, "<h1>served</h1>");
    probe = net_listen(0);
    int s_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", s_port);
    pid_t s_child = fork();
    if (s_child == 0) {
        execl(TOOL, "cweb", "serve", sf, port_arg, ".", ss, "--rate", "2",
              "--secure", NULL);
        _exit(127);
    }
    if (wait_for_port(s_port) != 0) {
        fprintf(stderr, "serve --rate --secure did not come up on port %d\n", s_port);
        kill(s_child, SIGTERM);
        waitpid(s_child, NULL, 0);
        return 1;
    }
    int s_ok = 0, s_ok_secure = 0, s_429 = 0, s_429_secure = 0;
    for (int i = 0; i < 10; i++) {
        char *s_body = fetch(s_port, "/");
        if (s_body == NULL) {
            continue;
        }
        int hard = strstr(s_body, "X-Content-Type-Options: nosniff") != NULL &&
                   strstr(s_body, "X-Frame-Options: DENY") != NULL;
        if (strstr(s_body, "HTTP/1.1 200 OK") != NULL &&
            strstr(s_body, "<h1>served</h1>") != NULL) {
            s_ok++;
            s_ok_secure += hard;
        } else if (strstr(s_body, "429") != NULL) {
            s_429++;
            s_429_secure += hard;
        }
        free(s_body);
    }
    kill(s_child, SIGTERM);
    waitpid(s_child, NULL, 0);
    if (s_ok_secure < 1 || s_429_secure < 1 || s_429 < 5) {
        fprintf(stderr,
                "cweb serve must pass FLAGS through (got %d ok/%d hardened, "
                "%d rate-limited/%d hardened)\n",
                s_ok, s_ok_secure, s_429, s_429_secure);
        return 1;
    }

    // --- --gzip compresses compressible bodies for gzip clients ---
    char gzv[1024], gzo[1024];
    snprintf(gzv, sizeof gzv, "%s/gzv", tmp);
    mkdir(gzv, 0755);
    snprintf(gzo, sizeof gzo, "%s/gzo", tmp);
    // static HTML comfortably above the 1 KiB gzip threshold
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", gzv);
    FILE *gzf = fopen(wbuf, "w");
    if (gzf != NULL) {
        for (int i = 0; i < 80; i++) {
            fputs("<p>lorem ipsum dolor sit amet, consectetur adipiscing elit</p>\n", gzf);
        }
        fclose(gzf);
    }
    snprintf(wbuf, sizeof wbuf, "%s build %s %s >/dev/null", TOOL, gzv, gzo);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed for the gzip fixture\n");
        return 1;
    }
    probe = net_listen(0);
    int gz_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", gz_port);
    snprintf(wbuf, sizeof wbuf, "%s/server", gzo);
    pid_t gz_child = fork();
    if (gz_child == 0) {
        execl(wbuf, "server", "--port", port_arg, "--gzip", NULL);
        _exit(127);
    }
    if (wait_for_port(gz_port) != 0) {
        fprintf(stderr, "gzip server did not come up on port %d\n", gz_port);
        return 1;
    }
    char *gz_raw = fetch(gz_port, "/");
    int gz_plain_ok = gz_raw != NULL && strstr(gz_raw, "HTTP/1.1 200 OK") != NULL &&
                      strstr(gz_raw, "Content-Encoding: gzip") == NULL;
    size_t raw_len = gz_raw != NULL ? strlen(gz_raw) : 0;
    char *gz_wire = fetch_hdr(gz_port, "/", "Accept-Encoding: gzip\r\n");
    int gz_ok = gz_wire != NULL && strstr(gz_wire, "HTTP/1.1 200 OK") != NULL &&
                strstr(gz_wire, "Content-Encoding: gzip") != NULL &&
                raw_len > 0 && strlen(gz_wire) < raw_len;
    if (!gz_plain_ok) {
        fprintf(stderr, "plain request must stay ungzipped:\n%.80s\n", gz_raw ? gz_raw : "(connect error)");
    }
    if (!gz_ok) {
        fprintf(stderr, "gzip request must compress (raw %zu, wire %zu):\n%.120s\n",
                raw_len, gz_wire ? strlen(gz_wire) : 0, gz_wire ? gz_wire : "(connect error)");
    }
    free(gz_raw);
    free(gz_wire);
    kill(gz_child, SIGTERM);
    waitpid(gz_child, NULL, 0);
    ok = ok && gz_plain_ok && gz_ok;

    // --- --static-cache stamps Cache-Control on files the static root serves ---
    probe = net_listen(0);
    int sc_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", sc_port);
    snprintf(wbuf, sizeof wbuf, "%s/server", out);
    pid_t sc_child = fork();
    if (sc_child == 0) {
        execl(wbuf, "server", "--port", port_arg, "--static-cache", "300", NULL);
        _exit(127);
    }
    if (wait_for_port(sc_port) != 0) {
        fprintf(stderr, "static-cache server did not come up on port %d\n", sc_port);
        return 1;
    }
    char *sc_wire = fetch(sc_port, "/style.css");
    int sc_ok = sc_wire != NULL && strstr(sc_wire, "HTTP/1.1 200 OK") != NULL &&
                strstr(sc_wire, "Cache-Control: public, max-age=300") != NULL;
    if (!sc_ok) {
        fprintf(stderr, "--static-cache must set Cache-Control on static files:\n%.120s\n",
                sc_wire ? sc_wire : "(connect error)");
    }
    free(sc_wire);
    kill(sc_child, SIGTERM);
    waitpid(sc_child, NULL, 0);
    ok = ok && sc_ok;

    // --- session flash messages: queued on POST, shown once on the next GET ---
    char flv[1024], flo[1024];
    snprintf(flv, sizeof flv, "%s/flv", tmp);
    mkdir(flv, 0755);
    snprintf(flo, sizeof flo, "%s/flo", tmp);
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", flv);
    wfile(wbuf,
          "<?c\n"
          "  Http_Session_Store *sfl = (Http_Session_Store *)user_data;\n"
          "  Http_Session *sesh = http_session_from_cookie(sfl, req, \"cweb_session\");\n"
          "  if (req->method == HTTP_POST) {\n"
          "      if (sesh == NULL) {\n"
          "          char *tok = http_session_create(sfl, -1);\n"
          "          if (tok != NULL) {\n"
          "              sesh = http_session_open(sfl, tok);\n"
          "              http_session_issue_cookie(res, \"cweb_session\", tok, NULL);\n"
          "          }\n"
          "      }\n"
          "      if (sesh != NULL) {\n"
          "          http_flash_set(sesh, \"saved\", \"note pinned!\");\n"
          "      }\n"
          "      http_response_redirect(res, HTTP_303_SEE_OTHER, \"/\");\n"
          "      return;\n"
          "  }\n"
          "  http_flash_render(res, sesh);\n"
          "?>\n"
          "<h1>Flash board</h1>");
    snprintf(wbuf, sizeof wbuf, "%s build %s %s >/dev/null", TOOL, flv, flo);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed for the flash fixture\n");
        return 1;
    }
    probe = net_listen(0);
    int fl_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", fl_port);
    snprintf(wbuf, sizeof wbuf, "%s/server", flo);
    pid_t fl_child = fork();
    if (fl_child == 0) {
        execl(wbuf, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(fl_port) != 0) {
        fprintf(stderr, "flash server did not come up on port %d\n", fl_port);
        return 1;
    }
    char *fl_body = fetch_req(fl_port, "GET", "/", NULL, NULL);
    int fl_ok = fl_body != NULL && strstr(fl_body, "HTTP/1.1 200 OK") != NULL &&
                strstr(fl_body, "<h1>Flash board</h1>") != NULL &&
                strstr(fl_body, "<div class=\"flash\">") == NULL;
    if (!fl_ok) {
        fprintf(stderr, "anonymous first visit should have no flash pending:\n%.120s\n", fl_body ? fl_body : "(connect error)");
    }
    free(fl_body);

    fl_body = fetch_req(fl_port, "POST", "/", NULL, "");
    char fl_cookie[160];
    grab_cookie(fl_body, "cweb_session", fl_cookie, sizeof fl_cookie);
    fl_ok = fl_ok && fl_body != NULL && strstr(fl_body, "HTTP/1.1 303") != NULL &&
            strstr(fl_body, "Location: /") != NULL && fl_cookie[0] != '\0';
    if (!fl_ok) {
        fprintf(stderr, "flash POST should redirect home and open a session:\n%.140s\n", fl_body ? fl_body : "(connect error)");
    }
    free(fl_body);

    char fl_hdr[192];
    snprintf(fl_hdr, sizeof fl_hdr, "cweb_session=%s", fl_cookie);
    fl_body = fetch_req(fl_port, "GET", "/", fl_hdr, NULL);
    fl_ok = fl_ok && fl_body != NULL && strstr(fl_body, "HTTP/1.1 200 OK") != NULL &&
            strstr(fl_body, "<h1>Flash board</h1>") != NULL &&
            strstr(fl_body, "<div class=\"flash\">note pinned!</div>") != NULL;
    if (!fl_ok) {
        fprintf(stderr, "the queued flash should appear on the redirected GET:\n%.160s\n", fl_body ? fl_body : "(connect error)");
    }
    free(fl_body);

    fl_body = fetch_req(fl_port, "GET", "/", fl_hdr, NULL);
    fl_ok = fl_ok && fl_body != NULL && strstr(fl_body, "HTTP/1.1 200 OK") != NULL &&
            strstr(fl_body, "<div class=\"flash\">") == NULL;
    if (!fl_ok) {
        fprintf(stderr, "a shown flash must be consumed by the render:\n%.160s\n", fl_body ? fl_body : "(connect error)");
    }
    free(fl_body);
    kill(fl_child, SIGTERM);
    waitpid(fl_child, NULL, 0);
    ok = ok && fl_ok;

    // --- CSRF: the form carries a per-session token, POSTs must match it ---
    char csv[1024], cso[1024];
    snprintf(csv, sizeof csv, "%s/csv", tmp);
    mkdir(csv, 0755);
    snprintf(cso, sizeof cso, "%s/cso", tmp);
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", csv);
    wfile(wbuf,
          "<?c\n"
          "  Http_Session_Store *cs = (Http_Session_Store *)user_data;\n"
          "  Http_Session *sesh = http_session_from_cookie(cs, req, \"cweb_session\");\n"
          "  if (sesh == NULL) {\n"
          "      char *tok = http_session_create(cs, -1);\n"
          "      if (tok != NULL) {\n"
          "          sesh = http_session_open(cs, tok);\n"
          "          http_session_issue_cookie(res, \"cweb_session\", tok, NULL);\n"
          "      }\n"
          "  }\n"
          "  if (req->method == HTTP_POST) {\n"
          "      if (http_csrf_verify(params, sesh) != 0) {\n"
          "          http_csrf_reject(res, \"/\");\n"
          "          return;\n"
          "      }\n"
          "      http_response_redirect(res, HTTP_303_SEE_OTHER, \"/?done=1\");\n"
          "      return;\n"
          "  }\n"
          "?>\n"
          "<form method=\"post\" action=\"/\">\n"
          "  <?c http_csrf_field(res, sesh); ?>\n"
          "  <button>Go</button>\n"
          "</form>");
    snprintf(wbuf, sizeof wbuf, "%s build %s %s >/dev/null", TOOL, csv, cso);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed for the csrf fixture\n");
        return 1;
    }
    probe = net_listen(0);
    int cs_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", cs_port);
    snprintf(wbuf, sizeof wbuf, "%s/server", cso);
    pid_t cs_child = fork();
    if (cs_child == 0) {
        execl(wbuf, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(cs_port) != 0) {
        fprintf(stderr, "csrf server did not come up on port %d\n", cs_port);
        return 1;
    }
    char *cs_body = fetch_req(cs_port, "GET", "/", NULL, NULL);
    char cs_cookie[160], cs_tok[64];
    grab_cookie(cs_body, "cweb_session", cs_cookie, sizeof cs_cookie);
    grab_form_token(cs_body, cs_tok, sizeof cs_tok);
    int cs_ok = cs_body != NULL && strstr(cs_body, "HTTP/1.1 200 OK") != NULL &&
                strstr(cs_body, "name=\"csrf_token\" value=\"") != NULL &&
                cs_cookie[0] != '\0' && strlen(cs_tok) == 40;
    if (!cs_ok) {
        fprintf(stderr, "the form should carry a per-session csrf token:\n%.160s\n", cs_body ? cs_body : "(connect error)");
    }
    free(cs_body);

    char cs_hdr[192];
    snprintf(cs_hdr, sizeof cs_hdr, "cweb_session=%s", cs_cookie);
    cs_body = fetch_req(cs_port, "POST", "/", cs_hdr, "");
    cs_ok = cs_ok && cs_body != NULL && strstr(cs_body, "HTTP/1.1 403") != NULL;
    if (!cs_ok) {
        fprintf(stderr, "a POST without the token must be refused:\n%.140s\n", cs_body ? cs_body : "(connect error)");
    }
    free(cs_body);

    cs_body = fetch_req(cs_port, "POST", "/", cs_hdr, "csrf_token=deadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    cs_ok = cs_ok && cs_body != NULL && strstr(cs_body, "HTTP/1.1 403") != NULL;
    if (!cs_ok) {
        fprintf(stderr, "a POST with the wrong token must be refused:\n%.140s\n", cs_body ? cs_body : "(connect error)");
    }
    free(cs_body);

    char cs_form[96];
    snprintf(cs_form, sizeof cs_form, "csrf_token=%s", cs_tok);
    cs_body = fetch_req(cs_port, "POST", "/", cs_hdr, cs_form);
    cs_ok = cs_ok && cs_body != NULL && strstr(cs_body, "HTTP/1.1 303") != NULL &&
            strstr(cs_body, "Location: /?done=1") != NULL;
    if (!cs_ok) {
        fprintf(stderr, "a POST carrying the real token must be accepted:\n%.140s\n", cs_body ? cs_body : "(connect error)");
    }
    free(cs_body);
    kill(cs_child, SIGTERM);
    waitpid(cs_child, NULL, 0);
    ok = ok && cs_ok;

    // --- validation: a POST is checked, one readable error, values kept ---
    char vv[1024], vo[1024];
    snprintf(vv, sizeof vv, "%s/vv", tmp);
    mkdir(vv, 0755);
    snprintf(vo, sizeof vo, "%s/vo", tmp);
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", vv);
    wfile(wbuf,
          "<?c\n"
          "  Http_Session_Store *vs = (Http_Session_Store *)user_data;\n"
          "  Http_Session *sesh = http_session_from_cookie(vs, req, \"v_session\");\n"
          "  if (sesh == NULL) {\n"
          "      char *tok = http_session_create(vs, -1);\n"
          "      if (tok != NULL) {\n"
          "          sesh = http_session_open(vs, tok);\n"
          "          http_session_issue_cookie(res, \"v_session\", tok, NULL);\n"
          "      }\n"
          "  }\n"
          "  const char *error = NULL;\n"
          "  if (req->method == HTTP_POST) {\n"
          "      Http_Form form;\n"
          "      http_form_init(&form);\n"
          "      long age = 0;\n"
          "      http_form_required(&form, params, \"username\", \"username\");\n"
          "      http_form_email(&form, params, \"email\", \"email\");\n"
          "      http_form_int(&form, params, \"age\", \"age\", 13, 150, &age);\n"
          "      if (!http_form_ok(&form)) {\n"
          "          error = http_form_error(&form);\n"
          "      } else {\n"
          "          http_response_redirect(res, HTTP_303_SEE_OTHER, \"/?saved=1\");\n"
          "          return;\n"
          "      }\n"
          "  }\n"
          "?>\n"
          "<h1>Join</h1>\n"
          "<?c if (error) { ?><p class=\"err\"><?h= sv_from_cstr(error) ?></p><?c } ?>\n"
          "<form method=\"post\" action=\"/\">\n"
          "  <input name=\"username\" <?c http_form_value(res, params, \"username\"); ?>>\n"
          "  <input name=\"email\" <?c http_form_value(res, params, \"email\"); ?>>\n"
          "  <input name=\"age\" <?c http_form_value(res, params, \"age\"); ?>>\n"
          "  <button>Join</button>\n"
          "</form>");
    snprintf(wbuf, sizeof wbuf, "%s build %s %s >/dev/null", TOOL, vv, vo);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed for the validation fixture\n");
        return 1;
    }
    probe = net_listen(0);
    int vv_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", vv_port);
    snprintf(wbuf, sizeof wbuf, "%s/server", vo);
    pid_t vv_child = fork();
    if (vv_child == 0) {
        execl(wbuf, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(vv_port) != 0) {
        fprintf(stderr, "validation server did not come up on port %d\n", vv_port);
        return 1;
    }
    char *vv_body = fetch_req(vv_port, "GET", "/", NULL, NULL);
    char vv_cookie[160];
    grab_cookie(vv_body, "v_session", vv_cookie, sizeof vv_cookie);
    int vv_ok = vv_body != NULL && strstr(vv_body, "HTTP/1.1 200 OK") != NULL &&
                strstr(vv_body, "<form method=\"post\" action=\"/\">") != NULL &&
                vv_cookie[0] != '\0';
    if (!vv_ok) {
        fprintf(stderr, "the form should be served with a session cookie:\n%.140s\n", vv_body ? vv_body : "(connect error)");
    }
    free(vv_body);

    snprintf(wbuf, sizeof wbuf, "v_session=%s", vv_cookie);
    vv_body = fetch_req(vv_port, "POST", "/", wbuf, "");
    vv_ok = vv_ok && vv_body != NULL &&
            strstr(vv_body, "<p class=\"err\">username is required.</p>") != NULL;
    if (!vv_ok) {
        fprintf(stderr, "a POST with a missing username should show the required error:\n%.160s\n", vv_body ? vv_body : "(connect error)");
    }
    free(vv_body);

    vv_body = fetch_req(vv_port, "POST", "/", wbuf,
                        "username=alice&email=b%40c.co&age=12");
    vv_ok = vv_ok && vv_body != NULL &&
            strstr(vv_body, "<p class=\"err\">age must be between 13 and 150.</p>") != NULL &&
            strstr(vv_body, "value=\"alice\"") != NULL &&
            strstr(vv_body, "value=\"b@c.co\"") != NULL &&
            strstr(vv_body, "value=\"12\"") != NULL;
    if (!vv_ok) {
        fprintf(stderr, "a POST with an out-of-range age should keep the typed values:\n%.180s\n", vv_body ? vv_body : "(connect error)");
    }
    free(vv_body);

    vv_body = fetch_req(vv_port, "POST", "/", wbuf,
                        "username=alice&email=b%40c.co&age=25");
    vv_ok = vv_ok && vv_body != NULL && strstr(vv_body, "HTTP/1.1 303") != NULL &&
            strstr(vv_body, "Location: /?saved=1") != NULL;
    if (!vv_ok) {
        fprintf(stderr, "a valid POST should redirect:\n%.140s\n", vv_body ? vv_body : "(connect error)");
    }
    free(vv_body);
    kill(vv_child, SIGTERM);
    waitpid(vv_child, NULL, 0);
    ok = ok && vv_ok;

    // --- hardening: --max-body caps request size (413), --io-timeout trips a
    // 408 on stalled clients, --workers sizes the pool, --routes shows all ---
    char hv[1024], ho[1024];
    snprintf(hv, sizeof hv, "%s/hv", tmp);
    mkdir(hv, 0755);
    snprintf(ho, sizeof ho, "%s/ho", tmp);
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", hv);
    wfile(wbuf, "<?c ?><p>hardening fixture</p>");
    snprintf(wbuf, sizeof wbuf, "%s build %s %s >/dev/null", TOOL, hv, ho);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed for the hardening fixture\n");
        return 1;
    }
    snprintf(wbuf, sizeof wbuf, "%s/server", ho);
    int h_ok = 1;
    char routes_file[600];

    // --routes echoes the hardening knobs, and only when they are set
    snprintf(routes_file, sizeof routes_file, "%s/routes.txt", tmp);
    pid_t rp = fork();
    if (rp == 0) {
        int fd = open(routes_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        execl(wbuf, "server", "--max-body", "4k", "--io-timeout", "250",
              "--workers", "3", "--routes", NULL);
        _exit(127);
    }
    waitpid(rp, NULL, 0);
    FILE *rf = fopen(routes_file, "r");
    char *rc = malloc(4096);
    size_t rn = fread(rc, 1, 4095, rf);
    rc[rn] = '\0';
    fclose(rf);
    h_ok = h_ok && strstr(rc, "max-body: 4096 bytes") != NULL &&
           strstr(rc, "io-timeout: 250ms") != NULL &&
           strstr(rc, "workers: 3") != NULL;
    if (!h_ok) {
        fprintf(stderr, "--routes should echo the hardening knobs when set:\n%s\n", rc);
    }
    free(rc);

    snprintf(routes_file, sizeof routes_file, "%s/routes2.txt", tmp);
    rp = fork();
    if (rp == 0) {
        int fd = open(routes_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        execl(wbuf, "server", "--routes", NULL);
        _exit(127);
    }
    waitpid(rp, NULL, 0);
    rf = fopen(routes_file, "r");
    rc = malloc(4096);
    rn = fread(rc, 1, 4095, rf);
    rc[rn] = '\0';
    fclose(rf);
    h_ok = h_ok && strstr(rc, "max-body:") == NULL &&
           strstr(rc, "io-timeout:") == NULL &&
           strstr(rc, "workers:") == NULL;
    if (!h_ok) {
        fprintf(stderr, "--routes without knobs should not mention them:\n%s\n", rc);
    }
    free(rc);

    // a body under the cap is served, a body over it gets 413
    probe = net_listen(0);
    int hb_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", hb_port);
    pid_t hb_child = fork();
    if (hb_child == 0) {
        execl(wbuf, "server", "--port", port_arg, "--max-body", "4096", NULL);
        _exit(127);
    }
    if (wait_for_port(hb_port) != 0) {
        fprintf(stderr, "hardening server did not come up on port %d\n", hb_port);
        return 1;
    }
    char big[70000];
    memset(big, 'x', sizeof big);
    char *hb_body = fetch_post_body(hb_port, "/", big, 2000);
    h_ok = h_ok && hb_body != NULL && strstr(hb_body, "HTTP/1.1 200 OK") != NULL;
    if (!h_ok) {
        fprintf(stderr, "a body under --max-body should be served:\n%.120s\n",
                hb_body ? hb_body : "(connect error)");
    }
    free(hb_body);

    hb_body = fetch_post_body(hb_port, "/", big, 9000);
    h_ok = h_ok && hb_body != NULL && strstr(hb_body, "HTTP/1.1 413") != NULL;
    if (!h_ok) {
        fprintf(stderr, "a body over --max-body should be refused with 413:\n%.120s\n",
                hb_body ? hb_body : "(connect error)");
    }
    free(hb_body);
    kill(hb_child, SIGTERM);
    waitpid(hb_child, NULL, 0);

    // the library default cap (64 KiB) still applies without a flag
    probe = net_listen(0);
    int hd_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", hd_port);
    pid_t hd_child = fork();
    if (hd_child == 0) {
        execl(wbuf, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(hd_port) != 0) {
        fprintf(stderr, "default server did not come up on port %d\n", hd_port);
        return 1;
    }
    hb_body = fetch_post_body(hd_port, "/", big, 70000);
    h_ok = h_ok && hb_body != NULL && strstr(hb_body, "HTTP/1.1 413") != NULL;
    if (!h_ok) {
        fprintf(stderr, "the default 64 KiB cap should 413 a 70 KiB body:\n%.120s\n",
                hb_body ? hb_body : "(connect error)");
    }
    free(hb_body);
    kill(hd_child, SIGTERM);
    waitpid(hd_child, NULL, 0);

    // a client that stalls past --io-timeout gets a 408
    probe = net_listen(0);
    int ht_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", ht_port);
    pid_t ht_child = fork();
    if (ht_child == 0) {
        execl(wbuf, "server", "--port", port_arg, "--io-timeout", "150", NULL);
        _exit(127);
    }
    if (wait_for_port(ht_port) != 0) {
        fprintf(stderr, "timeout server did not come up on port %d\n", ht_port);
        return 1;
    }
    char *ht_body = fetch_stall(ht_port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n");
    h_ok = h_ok && ht_body != NULL && strstr(ht_body, "HTTP/1.1 408") != NULL;
    if (!h_ok) {
        fprintf(stderr, "a client stalling past --io-timeout should get 408:\n%.120s\n",
                ht_body ? ht_body : "(connect error)");
    }
    free(ht_body);
    kill(ht_child, SIGTERM);
    waitpid(ht_child, NULL, 0);
    ok = ok && h_ok;

    // --- --signature-secret: only requests stamped with the shared secret ---
    // --- reach a page; everything else is refused with a 403 ---------------
    int sg_ok = 1;

    // --routes should surface the gate only when a secret is configured
    snprintf(routes_file, sizeof routes_file, "%s/sroutes.txt", tmp);
    pid_t srp = fork();
    if (srp == 0) {
        int fd = open(routes_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        dup2(fd, STDOUT_FILENO);
        close(fd);
        execl(wbuf, "server", "--signature-secret", "s3cr3t", "--routes", NULL);
        _exit(127);
    }
    waitpid(srp, NULL, 0);
    rf = fopen(routes_file, "r");
    rc = malloc(4096);
    rn = fread(rc, 1, 4095, rf);
    rc[rn] = '\0';
    fclose(rf);
    sg_ok = sg_ok && strstr(rc, "signature: on") != NULL;
    if (!sg_ok) {
        fprintf(stderr, "--routes should echo signature when --signature-secret is set:\n%s\n", rc);
    }
    free(rc);

    probe = net_listen(0);
    int sg_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", sg_port);
    pid_t sg_child = fork();
    if (sg_child == 0) {
        execl(wbuf, "server", "--port", port_arg, "--signature-secret",
              "s3cr3t", NULL);
        _exit(127);
    }
    if (wait_for_port(sg_port) != 0) {
        fprintf(stderr, "signature server did not come up on port %d\n", sg_port);
        return 1;
    }
    char *sg_body = fetch_signed(sg_port, "/", "s3cr3t", 0);
    sg_ok = sg_ok && sg_body != NULL && strstr(sg_body, "HTTP/1.1 200 OK") != NULL;
    if (!sg_ok) {
        fprintf(stderr, "a request with a valid signature should be served:\n%.120s\n",
                sg_body ? sg_body : "(connect error)");
    }
    free(sg_body);

    sg_body = fetch_signed(sg_port, "/", "s3cr3t", 1);
    sg_ok = sg_ok && sg_body != NULL && strstr(sg_body, "HTTP/1.1 403") != NULL;
    if (!sg_ok) {
        fprintf(stderr, "a request with a tampered signature should be refused:\n%.120s\n",
                sg_body ? sg_body : "(connect error)");
    }
    free(sg_body);

    sg_body = fetch_signed(sg_port, "/", "wrong-secret", 0);
    sg_ok = sg_ok && sg_body != NULL && strstr(sg_body, "HTTP/1.1 403") != NULL;
    if (!sg_ok) {
        fprintf(stderr, "a request signed with the wrong secret should be refused:\n%.120s\n",
                sg_body ? sg_body : "(connect error)");
    }
    free(sg_body);
    kill(sg_child, SIGTERM);
    waitpid(sg_child, NULL, 0);
    ok = ok && sg_ok;

    // --- --log appends a Common Log Format line per completed request ---
    char logpath[1024];
    snprintf(logpath, sizeof logpath, "%s/access.log", tmp);
    probe = net_listen(0);
    int lg_port = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", lg_port);
    snprintf(wbuf, sizeof wbuf, "%s/server", out);
    pid_t lg_child = fork();
    if (lg_child == 0) {
        execl(wbuf, "server", "--port", port_arg, "--log", logpath, NULL);
        _exit(127);
    }
    if (wait_for_port(lg_port) != 0) {
        fprintf(stderr, "log server did not come up on port %d\n", lg_port);
        return 1;
    }
    char *lg_body = fetch(lg_port, "/");
    int lg_served = lg_body != NULL && strstr(lg_body, "HTTP/1.1 200 OK") != NULL &&
                    strstr(lg_body, "<h1>Index</h1>") != NULL;
    // keep-alive this time: two requests on one connection must log two lines
    Socket_Handle k = net_connect("127.0.0.1", lg_port);
    if (k != -1) {
        char two[1200];
        snprintf(two, sizeof two,
                 "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
                 "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                 "Connection: close\r\n\r\n");
        net_send_all(k, two, strlen(two));
        char sink[4096];
        while (net_recv(k, sink, sizeof sink) > 0) {
        }
        net_close(k);
    }
    free(lg_body);
    kill(lg_child, SIGTERM);
    waitpid(lg_child, NULL, 0);
    FILE *lgf = fopen(logpath, "r");
    if (lgf == NULL) {
        fprintf(stderr, "no access log written at %s\n", logpath);
        return 1;
    }
    char lgline[512];
    int lg_lines = 0, lg_root = 0, lg_hello = 0, lg_shape = 1;
    while (fgets(lgline, sizeof lgline, lgf) != NULL) {
        long line_len = strlen(lgline);
        char *bytes = strstr(lgline, " 200 ");
        if (line_len == 0 || lgline[line_len - 1] != '\n' ||
            strstr(lgline, "127.0.0.1 - - [") == NULL ||
            strstr(lgline, "] \"") == NULL || bytes == NULL ||
            bytes[5] == '-' || bytes[5] < '0' || bytes[5] > '9') {
            lg_shape = 0;
        }
        lg_root += strstr(lgline, "\"GET / HTTP/1.1\" 200 ") != NULL ? 1 : 0;
        lg_hello += strstr(lgline, "\"GET /hello HTTP/1.1\" 200 ") != NULL ? 1 : 0;
        lg_lines++;
    }
    fclose(lgf);
    if (!lg_served || !lg_shape || lg_lines < 2 || lg_root < 1 || lg_hello < 1) {
        fprintf(stderr,
                "access log wrong (served %d, shape %d, lines %d, root %d, hello %d)\n",
                lg_served, lg_shape, lg_lines, lg_root, lg_hello);
        return 1;
    }
    ok = ok && lg_served && lg_shape && lg_hello >= 1;

    // --- cweb serve --watch rebuilds and restarts on change ---
    char wv[1024], ws[1024];
    snprintf(wv, sizeof wv, "%s/wv", tmp);
    mkdir(wv, 0755);
    snprintf(ws, sizeof ws, "%s/ws", tmp);
    mkdir(ws, 0755);
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", wv);
    wfile(wbuf, "<h1>v1</h1>");
    probe = net_listen(0);
    int wport = net_bound_port(probe);
    net_close(probe);
    snprintf(port_arg, sizeof port_arg, "%d", wport);
    pid_t wchild = fork();
    if (wchild == 0) {
        execl(TOOL, "cweb", "serve", "--watch", wv, port_arg, ".", ws, NULL);
        _exit(127);
    }
    if (wait_for_port(wport) != 0) {
        fprintf(stderr, "watch server did not come up on port %d\n", wport);
        kill(wchild, SIGTERM);
        waitpid(wchild, NULL, 0);
        return 1;
    }
    char *wb = fetch(wport, "/");
    int watch_ok = wb != NULL && strstr(wb, "HTTP/1.1 200 OK") != NULL &&
                   strstr(wb, "<h1>v1</h1>") != NULL;
    if (!watch_ok) {
        fprintf(stderr, "watch server should serve the first version:\n%s\n", wb ? wb : "(connect error)");
    }
    free(wb);

    // touch the template: the server must rebuild and restart, same port
    snprintf(wbuf, sizeof wbuf, "%s/index.c.html", wv);
    wfile(wbuf, "<h1>v2</h1>");
    int got_v2 = 0;
    for (int i = 0; i < 150 && !got_v2; i++) {
        nap();
        wb = fetch(wport, "/");
        if (wb != NULL && strstr(wb, "HTTP/1.1 200 OK") != NULL &&
            strstr(wb, "<h1>v2</h1>") != NULL) {
            got_v2 = 1;
        }
        free(wb);
    }
    kill(wchild, SIGTERM);
    waitpid(wchild, NULL, 0);
    if (!got_v2) {
        fprintf(stderr, "watch server did not serve the edited template\n");
        return 1;
    }

    ok = ok && app_ok && watch_ok;
    net_cleanup();

    // tidy up the fixture tree
    char cleanup[4096];
    snprintf(cleanup, sizeof cleanup,
             "rm -rf %s", tmp);
    system(cleanup);

    if (!ok) {
        return 1;
    }
    printf("cli ok\n");
    return 0;
}