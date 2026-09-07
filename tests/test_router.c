#include <stdio.h>
#include <string.h>

#include "http.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "sv.h"

typedef struct {
    int called;
    char id[64];
    char name[64];
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
    http_response_set_status(res, HTTP_200_OK);
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
    Record a = {0}, b = {0};
    router_add(&router, HTTP_GET, "/users/<id>", user_handler, &a);
    router_add(&router, HTTP_GET, "/pets/<name>/food", pet_handler, &b);
    router_add(&router, HTTP_POST, "/users", user_handler, &a);

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

    // method mismatch must not hit the GET route
    http_request_parse(&req, sv_from_cstr("DELETE /users/99 HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    router_dispatch(&req, &res, &router);
    if (res.status != HTTP_404_NOT_FOUND) {
        fprintf(stderr, "method mismatch should be 404\n");
        return 1;
    }
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

    router_free(&router);
    printf("router ok\n");
    return 0;
}