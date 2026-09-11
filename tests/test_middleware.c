#include <stdio.h>
#include <string.h>

#include "client_ip.h"
#include "ip.h"
#include "middleware.h"
#include "request.h"
#include "response.h"
#include "sv.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// -----------------------------------------------------------------------
// fixture handlers
// -----------------------------------------------------------------------

typedef struct {
    int *ran;
    int *extra;
    Http_Status status;
} Fixture;

static void final_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    Fixture *f = user_data;
    (*f->ran)++;
    res->status = f->status;
}

// records its own letter on the way in (before next) and on the way out
// (after next) so tests can read the exact call order back
static char order[64];
static size_t order_pos;

static void mw_a(Http_Request *req, Http_Response *res, void *user_data,
                 Http_Handler_Fn next, void *next_data)
{
    (void)res;
    (void)user_data;
    order[order_pos++] = 'A';
    next(req, res, next_data);
    order[order_pos++] = 'a';
}

static void mw_b(Http_Request *req, Http_Response *res, void *user_data,
                 Http_Handler_Fn next, void *next_data)
{
    (void)res;
    (void)user_data;
    order[order_pos++] = 'B';
    next(req, res, next_data);
    order[order_pos++] = 'b';
}

static void mw_record(Http_Request *req, Http_Response *res, void *user_data,
                      Http_Handler_Fn next, void *next_data)
{
    (void)res;
    int *extra = user_data;
    (*extra)++;
    next(req, res, next_data);
}

// a middleware that refuses the request without ever calling next: the final
// handler must not run and the response must carry the denial it wrote
static void mw_deny(Http_Request *req, Http_Response *res, void *user_data,
                    Http_Handler_Fn next, void *next_data)
{
    (void)req;
    (void)user_data;
    (void)next;
    (void)next_data;
    res->status = HTTP_403_FORBIDDEN;
}

int main(void)
{
    int fails = 0;

    // an empty chain still runs through the uniform trampoline convention
    {
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        int ran = 0;
        Fixture f = {&ran, NULL, HTTP_200_OK};
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);
        Http_Request req;
        memset(&req, 0, sizeof req);
        Http_Response res;
        http_response_init(&res);
        fn(&req, &res, data);
        fails += check("empty chain runs the final handler", ran == 1);
        fails += check("empty chain keeps the handler data", data != NULL);
        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_response_free(&res);
    }

    // one middleware wraps the handler and gets its own user_data
    {
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        int ran = 0, extra = 0;
        Fixture f = {&ran, &extra, HTTP_201_CREATED};
        http_middleware_add(&c, mw_record, &extra);
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);
        Http_Request req;
        memset(&req, 0, sizeof req);
        Http_Response res;
        http_response_init(&res);
        fn(&req, &res, data);
        // middleware user_data and the handler's landed on their own targets
        fails += check("middleware ran", extra == 1);
        fails += check("final handler ran", ran == 1);
        fails += check("middleware sees the response after the call",
            res.status == HTTP_201_CREATED);
        fails += check("build handed back a real trampoline", data != NULL);
        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_response_free(&res);
    }

    // middlewares stack FIFO and unwind the other way: A B H b a
    {
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, mw_a, NULL);
        http_middleware_add(&c, mw_b, NULL);
        int ran = 0;
        Fixture f = {&ran, NULL, HTTP_200_OK};
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);
        Http_Request req;
        memset(&req, 0, sizeof req);
        Http_Response res;
        http_response_init(&res);
        order_pos = 0;
        fn(&req, &res, data);
        order[order_pos] = '\0';
        fails += check("middleware order is onion-shaped",
            strcmp(order, "ABba") == 0);
        fails += check("final handler ran once", ran == 1);
        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_response_free(&res);
    }

    // a middleware may short-circuit the chain by never calling next
    {
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, mw_deny, NULL);
        int ran = 0;
        Fixture f = {&ran, NULL, HTTP_200_OK};
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);
        Http_Request req;
        memset(&req, 0, sizeof req);
        Http_Response res;
        http_response_init(&res);
        fn(&req, &res, data);
        fails += check("shorted chain skips the final handler", ran == 0);
        fails += check("short-circuit keeps the written response",
            res.status == HTTP_403_FORBIDDEN);
        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_response_free(&res);
    }

    // the access logger emits one CLF-style line this test can diff byte-wise
    {
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        FILE *logf = tmpfile();
        Http_AccessLog_Opts opts = {logf, 0};
        http_middleware_add(&c, http_access_log_middleware, &opts);
        int ran = 0;
        Fixture f = {&ran, NULL, HTTP_200_OK};
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);

        Http_Request req;
        memset(&req, 0, sizeof req);
        req.method = HTTP_POST;
        req.path = sv_from_cstr("/submit");
        req.version = sv_from_cstr("HTTP/1.1");
        Http_Response res;
        http_response_init(&res);
        http_response_add_body_cstr(&res, "x");
        fn(&req, &res, data);
        fails += check("access logger lets the request through", ran == 1);

        fflush(logf);
        rewind(logf);
        char line[256] = {0};
        size_t got = fread(line, 1, sizeof(line) - 1, logf);
        line[got] = '\0';
        fclose(logf);

        // "0.0.0.0 - [<timestamp>] \"POST /submit HTTP/1.1\" 200 1 <ms>"
        // (no client-ip/remote recorded on the zeroed request -> placeholder)
        fails += check("log starts with the host placeholder",
            strncmp(line, "0.0.0.0 - [", 11) == 0);
        fails += check("log carries the request line",
            strstr(line, "\"POST /submit HTTP/1.1\"") != NULL);
        fails += check("log carries the status", strstr(line, " 200 ") != NULL);
        fails += check("log carries the body size", strstr(line, " 1 ") != NULL);

        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_response_free(&res);
    }

    // the real caller is named, never dash: the peer address stand-in
    {
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        FILE *logf = tmpfile();
        Http_AccessLog_Opts opts = {logf, 0};
        http_middleware_add(&c, http_access_log_middleware, &opts);
        int ran = 0;
        Fixture f = {&ran, NULL, HTTP_200_OK};
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);

        Http_Request req;
        memset(&req, 0, sizeof req);
        req.method = HTTP_GET;
        req.path = sv_from_cstr("/");
        req.version = sv_from_cstr("HTTP/1.1");
        req.remote = sv_from_cstr("203.0.113.7");
        Http_Response res;
        http_response_init(&res);
        fn(&req, &res, data);

        fflush(logf);
        rewind(logf);
        char line[256] = {0};
        size_t got = fread(line, 1, sizeof line - 1, logf);
        line[got] = '\0';
        fclose(logf);

        fails += check("peer address fills the host field",
            strncmp(line, "203.0.113.7 - [", 15) == 0);
        fails += check("resolved-client log still keeps the request line",
            strstr(line, "\"GET / HTTP/1.1\"") != NULL);

        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_response_free(&res);
    }

    // chained after the client-ip middleware, the trusted-proxy-resolved
    // client (not the proxy) becomes the host field of the log line
    {
        Http_Cidr loopback;
        check("test premise: loopback trust parses",
            http_cidr_parse(&loopback, "127.0.0.1/32") == 0);
        Http_ClientIp_Options ip_opts = {&loopback, 1};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        FILE *logf = tmpfile();
        Http_AccessLog_Opts log_opts = {logf, 0};
        http_middleware_add(&c, http_client_ip_middleware, &ip_opts);
        http_middleware_add(&c, http_access_log_middleware, &log_opts);
        int ran = 0;
        Fixture f = {&ran, NULL, HTTP_200_OK};
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);

        Http_Request req;
        check("spoofed request parses",
            http_request_parse(&req, sv_from_cstr(
                "GET /admin HTTP/1.1\r\n"
                "Host: x\r\n"
                "X-Forwarded-For: 198.51.100.9\r\n\r\n")) == REQ_OK);
        req.remote = sv_from_cstr("127.0.0.1");
        Http_Response res;
        http_response_init(&res);
        fn(&req, &res, data);

        fflush(logf);
        rewind(logf);
        char line[256] = {0};
        size_t got = fread(line, 1, sizeof line - 1, logf);
        line[got] = '\0';
        fclose(logf);

        fails += check("resolved client fills the host field, not the proxy",
            strncmp(line, "198.51.100.9 - [", 16) == 0);
        fails += check("host field never names the proxy",
            strncmp(line, "127.0.0.1 - [", 13) != 0);

        http_request_free(&req);
        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_response_free(&res);
    }

    if (fails == 0) {
        printf("middleware ok\n");
    }
    return fails != 0;
}