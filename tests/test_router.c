#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "static.h"
#include "strbuf.h"
#include "sv.h"

typedef struct {
    int called;
    char id[64];
    char name[64];
    char flag[64];
    char k[64];
    char user[64];
} Record;

static void user_handler(Http_Request *req, Http_Response *res, Str_Map *params, void *ud)
{
    (void)req;
    Record *rec = ud;
    rec->called = 1;
    const char *id = strmap_get_cstr(params, "id");
    if (id) {
        strncpy(rec->id, id, sizeof(rec->id) - 1);
    }
    const char *flag = strmap_get_cstr(params, "flag");
    if (flag) {
        strncpy(rec->flag, flag, sizeof(rec->flag) - 1);
    }
    http_response_set_status(res, HTTP_200_OK);
}

static void pick_handler(Http_Request *req, Http_Response *res, Str_Map *params, void *ud)
{
    (void)req;
    Record *rec = ud;
    rec->called = 1;
    const char *k = strmap_get_cstr(params, "k");
    if (k) {
        strncpy(rec->k, k, sizeof(rec->k) - 1);
    }
    http_response_set_status(res, HTTP_200_OK);
}

static void form_handler(Http_Request *req, Http_Response *res, Str_Map *params, void *ud)
{
    (void)req;
    Record *rec = ud;
    rec->called = 1;
    const char *user = strmap_get_cstr(params, "user");
    if (user) {
        strncpy(rec->user, user, sizeof(rec->user) - 1);
    }
    http_response_set_status(res, HTTP_200_OK);
}

static void echo_handler(Http_Request *req, Http_Response *res, Str_Map *params, void *ud)
{
    (void)req;
    (void)params;
    Record *rec = ud;
    rec->called = 1;
    http_response_set_status(res, HTTP_200_OK);
    http_response_add_body_cstr(res, "hello");
}

static void pet_handler(Http_Request *req, Http_Response *res, Str_Map *params, void *ud)
{
    (void)req;
    Record *rec = ud;
    rec->called = 1;
    const char *name = strmap_get_cstr(params, "name");
    if (name) {
        strncpy(rec->name, name, sizeof(rec->name) - 1);
    }
    http_response_set_status(res, HTTP_200_OK);
}

int main(void)
{
    Http_Router router;
    router_init(&router);
    Record a = {0}, b = {0}, c = {0}, d = {0}, e = {0};
    router_add(&router, HTTP_GET, "/users/<id>", user_handler, &a);
    router_add(&router, HTTP_GET, "/pets/<name>/food", pet_handler, &b);
    router_add(&router, HTTP_GET, "/pick/<k>", pick_handler, &c);
    router_add(&router, HTTP_GET, "/echo", echo_handler, &e);
    router_add(&router, HTTP_POST, "/users", user_handler, &a);
    router_add(&router, HTTP_POST, "/form", form_handler, &d);

    Http_Request req;
    Http_Response res;

    http_request_parse(&req, sv_from_cstr("GET /users/99 HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (!a.called || strcmp(a.id, "99") != 0) {
        fprintf(stderr, "user route did not capture id\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    http_request_parse(&req, sv_from_cstr("GET /pets/whiskers/food HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (!b.called || strcmp(b.name, "whiskers") != 0) {
        fprintf(stderr, "pet route did not capture name\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // query params ride along with route captures
    http_request_parse(&req, sv_from_cstr("GET /users/12?flag=on HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (!a.called || strcmp(a.id, "12") != 0 || strcmp(a.flag, "on") != 0) {
        fprintf(stderr, "query param did not reach the handler\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // a captured segment wins over a query key with the same name
    http_request_parse(&req, sv_from_cstr("GET /pick/route?k=query HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (!c.called || strcmp(c.k, "route") != 0) {
        fprintf(stderr, "route capture should override query (got %s)\n", c.k);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // urlencoded body fields become params
    http_request_parse(&req, sv_from_cstr(
        "POST /form HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "user=alice&pw=secret"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (!d.called || strcmp(d.user, "alice") != 0) {
        fprintf(stderr, "form field did not reach the handler (user=%s)\n", d.user);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // malformed query data is a 400
    http_request_parse(&req, sv_from_cstr("GET /users/1?x=%zz HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (res.status != HTTP_400_BAD_REQUEST) {
        fprintf(stderr, "malformed query should be 400\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // method mismatch must not hit the GET route: 405 with an Allow header
    http_request_parse(&req, sv_from_cstr("DELETE /users/99 HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (res.status != HTTP_405_METHOD_NOT_ALLOWED) {
        fprintf(stderr, "method mismatch should be 405, got %d\n", (int)res.status);
        return 1;
    }
    Strbuf wire;
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Allow: GET\r\n") == NULL) {
        fprintf(stderr, "405 missing Allow header:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // /users is served by POST, so DELETE there lists only POST
    http_request_parse(&req, sv_from_cstr("DELETE /users HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (res.status != HTTP_405_METHOD_NOT_ALLOWED) {
        fprintf(stderr, "/users DELETE should be 405, got %d\n", (int)res.status);
        return 1;
    }
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Allow: POST\r\n") == NULL) {
        fprintf(stderr, "/users 405 should list POST only:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // HEAD runs the GET handler but suppresses the body, keeping the headers
    http_request_parse(&req, sv_from_cstr("HEAD /echo HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (!e.called || res.status != HTTP_200_OK || res.body.count != 5) {
        fprintf(stderr, "HEAD should run GET handler with body kept for length\n");
        return 1;
    }
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Content-Length: 5\r\n") == NULL) {
        fprintf(stderr, "HEAD should keep the GET Content-Length:\n%s\n", wire.items);
        return 1;
    }
    if (strstr(wire.items, "hello") != NULL) {
        fprintf(stderr, "HEAD must not carry the body:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // unknown path
    http_request_parse(&req, sv_from_cstr("GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (res.status != HTTP_404_NOT_FOUND) {
        fprintf(stderr, "unknown path should be 404\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // OPTIONS advertises what a path supports without running anything
    e.called = 0;
    http_request_parse(&req, sv_from_cstr("OPTIONS /echo HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (res.status != HTTP_200_OK) {
        fprintf(stderr, "OPTIONS should be 200, got %d\n", (int)res.status);
        return 1;
    }
    if (e.called) {
        fprintf(stderr, "OPTIONS must not run the GET handler\n");
        return 1;
    }
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Allow: GET\r\n") == NULL) {
        fprintf(stderr, "OPTIONS missing Allow header:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // --- static mount ----------------------------------------------------
    // a real file tree on disk, served from "/assets" by a prefix mount
    (void)mkdir("build/mnt", 0755);
    (void)mkdir("build/mnt/css", 0755);
    FILE *idx = fopen("build/mnt/index.html", "w");
    fprintf(idx, "<h1>Home</h1>\n");
    fclose(idx);
    FILE *app = fopen("build/mnt/css/app.css", "w");
    fprintf(app, "body{color:red}\n");
    fclose(app);

    Http_Router site;
    router_init(&site);
    if (http_static_mount(&site, "/assets", "build/mnt") != 0) {
        fprintf(stderr, "static mount failed\n");
        return 1;
    }

    // mount root serves the directory index
    http_request_parse(&req, sv_from_cstr("GET /assets HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &site);
    if (res.status != HTTP_200_OK || strstr(res.body.items, "<h1>Home</h1>") == NULL) {
        fprintf(stderr, "mount root should serve index.html\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // trailing slash reaches the same index
    http_request_parse(&req, sv_from_cstr("GET /assets/ HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &site);
    if (res.status != HTTP_200_OK || strstr(res.body.items, "<h1>Home</h1>") == NULL) {
        fprintf(stderr, "/assets/ should serve index.html\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // the prefix is stripped: /assets/css/app.css reads css/app.css
    http_request_parse(&req, sv_from_cstr("GET /assets/css/app.css HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &site);
    if (res.status != HTTP_200_OK ||
        strstr(res.body.items, "body{color:red}") == NULL ||
        strstr(res.headers.items, "text/css") == NULL) {
        fprintf(stderr, "nested asset under the mount should be served\n");
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // a traversal attempt on a mount is an error, not a file read outside
    http_request_parse(&req, sv_from_cstr("GET /assets/../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &site);
    if (res.status != HTTP_400_BAD_REQUEST) {
        fprintf(stderr, "traversal should be 400, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // a missing file under the mount is a 404, not a fallthrough
    http_request_parse(&req, sv_from_cstr("GET /assets/nope.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &site);
    if (res.status != HTTP_404_NOT_FOUND) {
        fprintf(stderr, "missing asset should be 404, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // paths outside the mount still 404: the catch-all didn't grab them
    http_request_parse(&req, sv_from_cstr("GET /assetsx/index.html HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &site);
    if (res.status != HTTP_404_NOT_FOUND) {
        fprintf(stderr, "similar-but-outside path should 404, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    router_free(&site);
    unlink("build/mnt/css/app.css");
    unlink("build/mnt/index.html");
    rmdir("build/mnt/css");
    rmdir("build/mnt");

    printf("router ok\n");
    return 0;
}