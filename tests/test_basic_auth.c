#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "base64.h"
#include "middleware.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "server.h"
#include "strbuf.h"
#include "sv.h"

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

// the guarded handler: records that it ran and answers 200
static void ok_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    int *ran = user_data;
    if (ran) {
        *ran += 1;
    }
    http_response_set_status(res, HTTP_200_OK);
    http_response_add_body_cstr(res, "open vault\n");
}

static void make_req(char *buf, size_t cap, const char *authorization)
{
    (void)cap; // callers always pass a buffer far larger than any request here
    size_t n = 0;
    const char *head = "GET / HTTP/1.1\r\nHost: x\r\n";
    memcpy(buf, head, strlen(head));
    n += strlen(head);
    if (authorization != NULL) {
        static const char hdr[] = "Authorization: ";
        memcpy(buf + n, hdr, sizeof hdr - 1);
        n += sizeof hdr - 1;
        memcpy(buf + n, authorization, strlen(authorization));
        n += strlen(authorization);
        memcpy(buf + n, "\r\n", 2);
        n += 2;
    }
    memcpy(buf + n, "\r\n", 2);
    n += 2;
    buf[n] = '\0';
}

// base64 of text, into a caller buffer
static void b64(char *out, size_t cap, const char *text)
{
    Strbuf sb;
    strbuf_init(&sb);
    base64_encode_into(&sb, sv_from_cstr(text));
    size_t n = sb.count < cap - 1 ? sb.count : cap - 1;
    memcpy(out, sb.items, n);
    out[n] = '\0';
    strbuf_free(&sb);
}

// case-sensitive substring check over a (maybe unterminated) buffer
static int buf_has(const char *data, size_t count, const char *needle)
{
    size_t nn = strlen(needle);
    for (size_t i = 0; i + nn <= count; i++) {
        if (memcmp(data + i, needle, nn) == 0) {
            return 1;
        }
    }
    return 0;
}

// run the Basic-auth guard over a real chain
static void run(char *raw, Http_BasicAuth_Options *opts,
                Http_Request *req, Http_Response *res, void **data, int *ran)
{
    http_request_parse(req, sv_from_cstr(raw));
    http_response_init(res);
    Http_Middleware_Chain c;
    http_middleware_init(&c);
    http_middleware_add(&c, http_basic_auth_middleware, opts);
    Http_Handler_Fn fn = http_middleware_build(&c, ok_handler, ran, data);
    fn(req, res, *data);
    http_middleware_free(&c);
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

int main(void)
{
    if (net_init() != 0) {
        fprintf(stderr, "net_init failed: %s\n", net_error_string());
        return 1;
    }

    char raw[600];
    char good_b64[128];

    // --- matching credentials let the chain through, user is exposed ---
    {
        b64(good_b64, sizeof good_b64, "alice:secret");
        snprintf(raw, sizeof raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                 good_b64);
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        int ran = 0;
        Http_BasicAuth_Options opts = {"alice", "secret", NULL};
        run(raw, &opts, &req, &res, &data, &ran);

        check("valid credentials ran the handler", ran == 1);
        check("valid credentials answered 200", res.status == HTTP_200_OK);
        check("authenticated user exposed on req",
              req.auth_user.count == 5 &&
                  memcmp(req.auth_user.data, "alice", 5) == 0);
        check("body from the handler is intact",
              buf_has(res.body.items, res.body.count, "open vault"));
        char *wa = http_response_get_header(&res, "WWW-Authenticate");
        check("no WWW-Authenticate on success", wa == NULL);
        free(wa);
        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- wrong password -> 401, chain skipped ---
    {
        char bad_b64[128];
        b64(bad_b64, sizeof bad_b64, "alice:nope");
        snprintf(raw, sizeof raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                 bad_b64);
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        int ran = 0;
        Http_BasicAuth_Options opts = {"alice", "secret", NULL};
        run(raw, &opts, &req, &res, &data, &ran);

        check("wrong password skipped the handler", ran == 0);
        check("wrong password answered 401", res.status == HTTP_401_UNAUTHORIZED);
        char *chal = http_response_get_header(&res, "WWW-Authenticate");
        check("challenge names the Basic scheme",
              chal != NULL && strstr(chal, "Basic realm=") != NULL);
        check("default realm is cweb",
              chal != NULL && strstr(chal, "realm=\"cweb\"") != NULL);
        free(chal);
        check("401 body explains why",
              buf_has(res.body.items, res.body.count, "authentication required"));
        check("failed login leaves auth_user empty", req.auth_user.count == 0);

        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- missing header -> 401 ---
    {
        make_req(raw, sizeof raw, NULL);
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        int ran = 0;
        Http_BasicAuth_Options opts = {"alice", "secret", NULL};
        run(raw, &opts, &req, &res, &data, &ran);

        check("missing header skipped the handler", ran == 0);
        check("missing header answered 401", res.status == HTTP_401_UNAUTHORIZED);
        char *chal = http_response_get_header(&res, "WWW-Authenticate");
        check("missing header still challenges", chal != NULL);
        free(chal);

        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- wrong user -> 401 ---
    {
        b64(good_b64, sizeof good_b64, "mallory:secret");
        snprintf(raw, sizeof raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                 good_b64);
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        int ran = 0;
        Http_BasicAuth_Options opts = {"alice", "secret", NULL};
        run(raw, &opts, &req, &res, &data, &ran);

        check("wrong user skipped the handler", ran == 0);
        check("wrong user answered 401", res.status == HTTP_401_UNAUTHORIZED);

        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- custom realm, and quoted-string escaping of the realm ---
    {
        b64(good_b64, sizeof good_b64, "alice:secret");
        snprintf(raw, sizeof raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                 good_b64);

        Http_Request req;
        Http_Response res;
        void *data = NULL;
        int ran = 0;
        char realm[] = "we\"ird realm";
        Http_BasicAuth_Options opts = {"alice", "wrong", realm};
        run(raw, &opts, &req, &res, &data, &ran);

        char *chal = http_response_get_header(&res, "WWW-Authenticate");
        check("custom realm honored", chal != NULL &&
                                         strstr(chal, "realm=\"we\\\"ird realm\"") != NULL);
        free(chal);

        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- malformed Authorization forms all get 401 ---
    {
        const char *cases[] = {
            "Basic !!!notbase64!!!",              // bad alphabet
            "Digest realm=\"x\"",                  // wrong scheme
            "Basic ",                              // empty payload
            "",                                    // bare scheme-less junk
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            Http_Request req;
            Http_Response res;
            void *data = NULL;
            int ran = 0;
            Http_BasicAuth_Options opts = {"alice", "secret", NULL};
            make_req(raw, sizeof raw, cases[i]);
            run(raw, &opts, &req, &res, &data, &ran);

            check("malformed header skipped the handler", ran == 0);
            check("malformed header answered 401",
                  res.status == HTTP_401_UNAUTHORIZED);
            http_middleware_data_free(data);
            http_request_free(&req);
            http_response_free(&res);
        }

        // base64 of "noseparator" (no colon) and ":x" (empty user)
        char b1[64], b2[64];
        b64(b1, sizeof b1, "noseparator");
        b64(b2, sizeof b2, ":x");
        char junk[600];
        for (size_t i = 0; i < 2; i++) {
            snprintf(junk, sizeof junk, "Basic %s", i == 0 ? b1 : b2);
            make_req(raw, sizeof raw, junk);
            Http_Request req;
            Http_Response res;
            void *data = NULL;
            int ran = 0;
            Http_BasicAuth_Options opts = {"alice", "secret", NULL};
            run(raw, &opts, &req, &res, &data, &ran);
            check("colon-less payload answered 401",
                  ran == 0 && res.status == HTTP_401_UNAUTHORIZED);
            http_middleware_data_free(data);
            http_request_free(&req);
            http_response_free(&res);
        }
    }

    // --- an empty password is legal and matches ---
    {
        char empty_b64[128];
        b64(empty_b64, sizeof empty_b64, "alice:");
        snprintf(raw, sizeof raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                 empty_b64);
        Http_Request req;
        Http_Response res;
        void *data = NULL;
        int ran = 0;
        Http_BasicAuth_Options opts = {"alice", NULL, NULL};
        run(raw, &opts, &req, &res, &data, &ran);

        check("empty supplied password accepted", ran == 1);

        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);
    }

    // --- empty configured user (or NULL opts) disables the guard ---
    {
        make_req(raw, sizeof raw, NULL);

        Http_Request req;
        Http_Response res;
        void *data = NULL;
        int ran = 0;
        Http_BasicAuth_Options opts = {"", NULL, NULL};
        run(raw, &opts, &req, &res, &data, &ran);
        check("empty configured user disables the guard", ran == 1);
        http_middleware_data_free(data);
        http_request_free(&req);
        http_response_free(&res);

        Http_Request req2;
        Http_Response res2;
        void *data2 = NULL;
        int ran2 = 0;
        run(raw, NULL, &req2, &res2, &data2, &ran2);
        check("NULL opts also disables the guard", ran2 == 1);
        http_middleware_data_free(data2);
        http_request_free(&req2);
        http_response_free(&res2);
    }

    // --- full connection: valid credentials get 200 and the body ---
    {
        Socket_Handle srv = net_listen(0);
        int port = net_bound_port(srv);
        Socket_Handle client = net_connect("127.0.0.1", port);
        Socket_Handle peer = net_accept(srv);

        snprintf(raw, sizeof raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                 good_b64);
        net_send_all(client, raw, strlen(raw));

        int ran = 0;
        Http_BasicAuth_Options opts = {"alice", "secret", NULL};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_basic_auth_middleware, &opts);
        void *data = NULL;
        Http_Handler_Fn fn =
            http_middleware_build(&c, ok_handler, &ran, &data);
        Http_Server_Config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.io_timeout_ms = 5000;
        cfg.workers = 0;
        http_serve_connection_config(peer, fn, data, &cfg);
        http_middleware_data_free(data);
        http_middleware_free(&c);

        char wire[4096] = {0};
        read_until(wire, sizeof wire, client, "\r\n\r\n", 12);
        check("wire: valid credentials answer 200",
              strstr(wire, "200 OK") != NULL);
        check("wire: handler body arrived",
              strstr(wire, "open vault") != NULL);

        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    // --- full connection: no credentials get 401 + challenge on the wire ---
    {
        Socket_Handle srv = net_listen(0);
        int port = net_bound_port(srv);
        Socket_Handle client = net_connect("127.0.0.1", port);
        Socket_Handle peer = net_accept(srv);

        make_req(raw, sizeof raw, NULL);
        net_send_all(client, raw, strlen(raw));

        int ran = 0;
        Http_BasicAuth_Options opts = {"alice", "secret", NULL};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_basic_auth_middleware, &opts);
        void *data = NULL;
        Http_Handler_Fn fn =
            http_middleware_build(&c, ok_handler, &ran, &data);
        Http_Server_Config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.io_timeout_ms = 5000;
        cfg.workers = 0;
        http_serve_connection_config(peer, fn, data, &cfg);
        http_middleware_data_free(data);
        http_middleware_free(&c);

        char wire[4096] = {0};
        read_until(wire, sizeof wire, client, "\r\n\r\n", 22);
        check("wire: no credentials answer 401",
              strstr(wire, "401 Unauthorized") != NULL);
        check("wire: WWW-Authenticate challenge present",
              strstr(wire, "WWW-Authenticate: Basic realm=\"cweb\"") != NULL);
        check("wire: handler never ran",
              strstr(wire, "authentication required") != NULL &&
                  strstr(wire, "open vault") == NULL);

        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    if (fails) {
        fprintf(stderr, "%d failures\n", fails);
        return 1;
    }
    printf("basic_auth ok\n");
    return 0;
}