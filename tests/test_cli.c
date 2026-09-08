#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "net.h"

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

    // build the views into C and a server binary, mounting the static dir
    snprintf(wbuf, sizeof wbuf, "%s build %s %s . %s >/dev/null", TOOL, views, out, staticd);
    if (system(wbuf) != 0) {
        fprintf(stderr, "cweb build failed\n");
        return 1;
    }

    // the generated artifacts line up with the views
    char path[2048];
    const char *names[] = {"page_hello.c", "page_index.c", "page_world.c",
                           "page_404.c", "main.c", "server"};
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

    int routes_ok = strstr(main, "router_add(&r, HTTP_GET, \"/hello\", page_hello, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_POST, \"/hello\", page_hello, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/sub/world\", page_world, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/index\", page_index, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"/\", page_index, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_GET, \"*\", cweb_fallback, &cweb_sessions);") != NULL &&
                    strstr(main, "router_add(&r, HTTP_POST, \"*\", cweb_fallback, &cweb_sessions);") != NULL &&
                    strstr(main, "static void cweb_fallback(") != NULL &&
                    strstr(main, "http_serve_static(req, res, (void *)cweb_static_root);") != NULL &&
                    strstr(main, "http_static_mount(&r, \"\", cweb_static_root);") == NULL;
    free(main);
    if (!routes_ok) {
        fprintf(stderr, "generated main.c does not register the expected routes\n");
        return 1;
    }

    // --routes lists them without binding a port
    snprintf(wbuf, sizeof wbuf, "%s/server --routes", out);
    FILE *pr = popen(wbuf, "r");
    int has_hello = 0, has_index = 0, has_world = 0, has_static = 0, has_404 = 0;
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
        }
        pclose(pr);
    }
    if (!has_hello || !has_index || !has_world || !has_static || !has_404) {
        fprintf(stderr, "--routes output incomplete\n");
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
         strstr(body, "<h1>Hello</h1>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /hello failed:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/sub/world");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<p>sub world</p>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /sub/world failed:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<h1>Index</h1>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET / failed:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/index");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 200 OK") != NULL &&
         strstr(body, "<h1>Index</h1>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /index failed:\n%s\n", body ? body : "(connect error)");
    }
    free(body);

    body = fetch(port, "/nope");
    ok = ok && body != NULL && strstr(body, "HTTP/1.1 404") != NULL &&
         strstr(body, "<h1>Not found</h1>") != NULL;
    if (!ok) {
        fprintf(stderr, "GET /nope should render the custom 404 page:\n%s\n", body ? body : "(connect error)");
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