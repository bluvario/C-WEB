#define _POSIX_C_SOURCE 200809L

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
                    strstr(main, "cweb_tpl_capture_begin") != NULL &&
                    strstr(main, "static void cweb_wrap_hello(") != NULL &&
                    strstr(main, "static void cweb_wrap_fallback(") != NULL &&
                    strstr(main, "http_rate_limit_middleware") != NULL &&
                    strstr(main, "http_security_middleware") != NULL &&
                    strstr(main, "http_gzip_middleware") != NULL &&
                    strstr(main, "cweb_rate_key") != NULL &&
                    strstr(main, "--secure") != NULL &&
                    strstr(main, "--gzip") != NULL &&
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
         strstr(body, "text/css") != NULL && strstr(body, "#fff") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /style.css should come from the static mount:\n%s\n", body ? body : "(connect error)");
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