#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "middleware.h"
#include "net.h"
#include "request.h"
#include "request_id.h"
#include "response.h"
#include "server.h"
#include "sv.h"

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

// final handler that echoes the request id into the body, so a test can prove
// the application saw req->request_id and it matches the wire header
static void echo_id_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    http_response_add_body(res, req->request_id);
}

// final handler that writes its own X-Request-Id the middleware must displace
static void strap_header_handler(Http_Request *req, Http_Response *res,
                                 void *user_data)
{
    (void)req;
    (void)user_data;
    http_response_set_header(res, "X-Request-Id", "handler-strap");
}

static void make_req(char *buf, size_t cap, const char *incoming)
{
    if (incoming != NULL) {
        snprintf(buf, cap,
                 "GET / HTTP/1.1\r\n"
                 "Host: x\r\n"
                 "X-Request-Id: %s\r\n"
                 "\r\n", incoming);
    } else {
        snprintf(buf, cap, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    }
}

// drives the request-id middleware over a real chain: parse raw, run its
// trampoline over the final handler, and leave req/res/data for the caller to
// free
static void run_chain(char *raw, Http_RequestId_Opts *opts,
                      Http_Handler_Fn final, void *fud,
                      Http_Request *req, Http_Response *res, void **data_out)
{
    http_request_parse(req, sv_from_cstr(raw));
    http_response_init(res);
    Http_Middleware_Chain c;
    http_middleware_init(&c);
    http_middleware_add(&c, http_request_id_middleware, opts);
    Http_Handler_Fn fn = http_middleware_build(&c, final, fud, data_out);
    fn(req, res, *data_out);
    http_middleware_free(&c);
}

// counts case-insensitive occurrences of the header name in the response text
static size_t count_headers(Http_Response *res, const char *name)
{
    size_t nn = strlen(name);
    size_t occurs = 0;
    size_t at = 0;
    while (at < res->headers.count) {
        bool match = res->headers.count - at >= nn &&
                     res->headers.items[at + nn] == ':';
        if (match) {
            for (size_t i = 0; i < nn; i++) {
                if ((res->headers.items[at + i] | 0x20) != (name[i] | 0x20)) {
                    match = false;
                    break;
                }
            }
        }
        if (match) {
            occurs++;
        }
        at = res->headers.count < at + 1 ? res->headers.count : at + 1;
    }
    return occurs;
}

// reads from client until it holds head_end + tail_len bytes after it
static void read_until(char *buf, size_t cap, Socket_Handle client,
                       const char *head_end, size_t tail_len)
{
    net_set_timeout(client, 3000);
    size_t got = 0;
    size_t he = strlen(head_end);
    for (;;) {
        for (size_t i = 0; i + he + tail_len <= got; i++) {
            if (memcmp(buf + i, head_end, he) == 0) {
                return;
            }
        }
        long n = net_recv(client, buf + got, cap - got - 1);
        if (n <= 0) {
            return;
        }
        got += (size_t)n;
    }
}

static int is_minted_hex(const char *s, size_t n)
{
    if (n != 16) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (strchr("0123456789abcdef", s[i]) == NULL) {
            return 0;
        }
    }
    return 1;
}

// counts case-insensitive occurrences of "X-Request-Id:" before the head end
// of a raw server response
static size_t count_headers_wire(const char *wire, const char *head_end)
{
    static const char tag[] = "X-Request-Id:";
    size_t n = 0;
    const char *p = wire;
    while (p + strlen(tag) <= head_end) {
        if (strncasecmp(p, tag, strlen(tag)) == 0) {
            n++;
        }
        p++;
    }
    return n;
}

// the body is exactly the string when the next byte past it is not part of it
static int nhead_is(const char *s, const char *want)
{
    size_t n = strlen(want);
    return s[n] == '\0' || s[n] == '\r' || s[n] == '\n';
}

// body of a wire response starts right past the head terminator
static int body_is(const char *wire, const char *head_end, const char *s)
{
    (void)wire;
    return strncmp(head_end + 4, s, strlen(s)) == 0 &&
           nhead_is(head_end + 4, s) != 0;
}

int main(void)
{
    if (net_init() != 0) {
        fprintf(stderr, "net_init failed: %s\n", net_error_string());
        return 1;
    }

    char raw[600];

    // --- minted id: stamped on the response, visible to the handler ---
    {
        make_req(raw, sizeof raw, NULL);
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        Http_RequestId_Opts opts = {0};
        run_chain(raw, &opts, echo_id_handler, NULL, &req, &res, &data);

        char *got = http_response_get_header(&res, "X-Request-Id");
        check("response carries X-Request-Id", got != NULL);
        check("minted id is 16 hex digits",
              got != NULL && is_minted_hex(got, strlen(got)));
        check("handler saw the same id as the header",
              got != NULL && req.request_id.count == strlen(got) &&
                  memcmp(req.request_id.data, got, req.request_id.count) == 0);
        check("exactly one X-Request-Id line", count_headers(&res, "X-Request-Id") == 1);
        free(got);
        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- default (opts NULL) behaves like honor_incoming off ---
    {
        make_req(raw, sizeof raw, "forged-proxy-id");
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        run_chain(raw, NULL, echo_id_handler, NULL, &req, &res, &data);

        char *got = http_response_get_header(&res, "X-Request-Id");
        check("NULL opts still stamp an id", got != NULL);
        check("NULL opts ignore the inbound id",
              got != NULL && strcmp(got, "forged-proxy-id") != 0);
        free(got);
        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- two requests never share an id ---
    {
        make_req(raw, sizeof raw, NULL);
        Http_Request req1, req2;
        Http_Response res1, res2;
        void *d1 = NULL, *d2 = NULL;
        Http_RequestId_Opts opts = {0};
        run_chain(raw, &opts, echo_id_handler, NULL, &req1, &res1, &d1);
        run_chain(raw, &opts, echo_id_handler, NULL, &req2, &res2, &d2);
        check("successive ids differ",
              req1.request_id.count == req2.request_id.count &&
                  memcmp(req1.request_id.data, req2.request_id.data,
                         req2.request_id.count) != 0);
        http_middleware_data_free(d1);
        http_middleware_data_free(d2);
        http_request_free(&req1);
        http_request_free(&req2);
        http_response_free(&res1);
        http_response_free(&res2);
    }

    // --- honor_incoming echoes a clean inbound id ---
    {
        make_req(raw, sizeof raw, "trace-42");
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        Http_RequestId_Opts opts = {1};
        run_chain(raw, &opts, echo_id_handler, NULL, &req, &res, &data);

        char *got = http_response_get_header(&res, "X-Request-Id");
        check("inbound id honored", got != NULL && strcmp(got, "trace-42") == 0);
        check("handler saw the inbound id",
              got != NULL && req.request_id.count == 8 &&
                  memcmp(req.request_id.data, "trace-42", 8) == 0);
        free(got);
        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- honor_incoming normalizes OWS, filters the truly dirty ---
    {
        make_req(raw, sizeof raw, "   trace-42   ");
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        Http_RequestId_Opts opts = {1};
        run_chain(raw, &opts, echo_id_handler, NULL, &req, &res, &data);
        char *got = http_response_get_header(&res, "X-Request-Id");
        check("inbound id is trimmed of OWS",
              got != NULL && strcmp(got, "trace-42") == 0);
        free(got);
        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);

        Http_RequestId_Opts opts2 = {1};
        const char *dirty[] = {
            "with space inside",
            "ctrl\x01byte",
        };
        for (size_t i = 0; i < sizeof dirty / sizeof dirty[0]; i++) {
            make_req(raw, sizeof raw, dirty[i]);
            Http_Request req;
            Http_Response res;
            void *data = NULL;
            run_chain(raw, &opts2, echo_id_handler, NULL, &req, &res, &data);
            char *got = http_response_get_header(&res, "X-Request-Id");
            check("dirty inbound id was replaced with a minted one",
                  got != NULL && is_minted_hex(got, strlen(got)));
            free(got);
            http_middleware_data_free(data);
            http_request_free(&req);
            http_response_free(&res);
        }

        char longid[200];
        memset(longid, 'a', sizeof longid);
        longid[sizeof longid - 1] = '\0';
        make_req(raw, sizeof raw, longid);
        Http_Request req2;
        Http_Response res2;
        void *d2 = NULL;
        run_chain(raw, &opts2, echo_id_handler, NULL, &req2, &res2, &d2);
        char *got2 = http_response_get_header(&res2, "X-Request-Id");
        check("overlong inbound id was replaced",
              got2 != NULL && is_minted_hex(got2, strlen(got2)));
        free(got2);
        http_middleware_data_free(d2);
        http_request_free(&req2);
        http_response_free(&res2);
    }

    // --- a handler-set X-Request-Id is displaced exactly once ---
    {
        make_req(raw, sizeof raw, NULL);
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        Http_RequestId_Opts opts = {0};
        run_chain(raw, &opts, strap_header_handler, NULL, &req, &res, &data);

        char *got = http_response_get_header(&res, "X-Request-Id");
        check("handler strap replaced by the middleware id",
              got != NULL && strcmp(got, "handler-strap") != 0 &&
                  req.request_id.count == strlen(got) &&
                  memcmp(req.request_id.data, got, req.request_id.count) == 0);
        check("only one X-Request-Id line survives",
              count_headers(&res, "X-Request-Id") == 1);
        free(got);
        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- the access logger appends the id only when asked ---
    {
        Http_RequestId_Opts rid = {0};
        Http_AccessLog_Opts logopts = {NULL, 1};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_request_id_middleware, &rid);
        http_middleware_add(&c, http_access_log_middleware, &logopts);
        FILE *logf = tmpfile();
        logopts.file = logf;
        void *data = NULL;
        Http_Handler_Fn fn =
            http_middleware_build(&c, echo_id_handler, NULL, &data);

        make_req(raw, sizeof raw, NULL);
        Http_Request req;
        http_request_parse(&req, sv_from_cstr(raw));
        Http_Response res;
        http_response_init(&res);
        fn(&req, &res, data);

        fflush(logf);
        rewind(logf);
        char line[512] = {0};
        size_t gotl = fread(line, 1, sizeof line - 1, logf);
        line[gotl] = '\0';
        fclose(logf);

        // the request id is the last whitespace-delimited field of the line
        char *last_token = line;
        for (char *p = line; *p != '\0'; p++) {
            if (*p == ' ') {
                last_token = p + 1;
            }
        }
        check("access line ends with the request id",
              req.request_id.count == 16 &&
                  strncmp(last_token, (char *)req.request_id.data, 16) == 0 &&
                  last_token[16] == '\n');

        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- full connection: inbound id honored end to end ---
    {
        Socket_Handle srv = net_listen(0);
        int port = net_bound_port(srv);
        Socket_Handle client = net_connect("127.0.0.1", port);
        Socket_Handle peer = net_accept(srv);

        make_req(raw, sizeof raw, "trace-42");
        net_send_all(client, raw, strlen(raw));

        Http_RequestId_Opts opts = {1};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_request_id_middleware, &opts);
        void *data = NULL;
        Http_Handler_Fn fn =
            http_middleware_build(&c, echo_id_handler, NULL, &data);
        Http_Server_Config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.io_timeout_ms = 5000;
        cfg.workers = 0;
        http_serve_connection_config(peer, fn, data, &cfg);
        http_middleware_data_free(data);
        http_middleware_free(&c);

        char wire[4096] = {0};
        read_until(wire, sizeof wire, client, "\r\n\r\n", 8);
        char *head_end = strstr(wire, "\r\n\r\n");
        check("server answered 200", head_end != NULL &&
                                         strstr(wire, "200 OK") != NULL);
        char *rid = head_end != NULL ? strstr(wire, "X-Request-Id: trace-42") : NULL;
        check("wire echoes the inbound id on the response",
              rid != NULL && rid < head_end);
check("exactly one X-Request-Id line",
              head_end != NULL && count_headers_wire(wire, head_end) == 1);
        int he_ok = head_end != NULL;
        int bd_ok = body_is(wire, head_end, "trace-42");
        check("handler saw the same id, echoed in the body", he_ok && bd_ok);

        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    // --- full connection: minted id, handler body matches the header ---
    {
        Socket_Handle srv = net_listen(0);
        int port = net_bound_port(srv);
        Socket_Handle client = net_connect("127.0.0.1", port);
        Socket_Handle peer = net_accept(srv);

        make_req(raw, sizeof raw, NULL);
        net_send_all(client, raw, strlen(raw));

        Http_RequestId_Opts opts = {0};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_request_id_middleware, &opts);
        void *data = NULL;
        Http_Handler_Fn fn =
            http_middleware_build(&c, echo_id_handler, NULL, &data);
        Http_Server_Config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.io_timeout_ms = 5000;
        cfg.workers = 0;
        http_serve_connection_config(peer, fn, data, &cfg);
        http_middleware_data_free(data);
        http_middleware_free(&c);

        char wire[4096] = {0};
        read_until(wire, sizeof wire, client, "\r\n\r\n", 17);
        char *head_end = strstr(wire, "\r\n\r\n");
        check("server answered 200 (minted case)", head_end != NULL &&
                                          strstr(wire, "200 OK") != NULL);
        char *rid = head_end != NULL ? strstr(wire, "X-Request-Id: ") : NULL;
        check("wire carries a minted X-Request-Id",
              rid != NULL && rid < head_end &&
                  is_minted_hex(rid + strlen("X-Request-Id: "), 16));
        check("body matches the minted header id",
              rid != NULL && head_end != NULL &&
                  strncmp(head_end + 4, rid + strlen("X-Request-Id: "), 16) == 0);

        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    if (fails) {
        fprintf(stderr, "%d failures\n", fails);
        return 1;
    }
    printf("request_id ok\n");
    return 0;
}