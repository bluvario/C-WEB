#include <stdio.h>
#include <string.h>

#include "client_ip.h"
#include "ip.h"
#include "middleware.h"
#include "net.h"
#include "request.h"
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

static void handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    res->status = HTTP_200_OK;
    char buf[96];
    snprintf(buf, sizeof buf, "%.*s", (int)req->client_ip.count,
             req->client_ip.data);
    http_response_set_header(res, "X-Client-IP", buf);
}

static void run_ip_middleware(Http_Request *req, const Http_Cidr *trusted,
                              size_t n)
{
    Http_ClientIp_Options opts = {trusted, n};
    Http_Middleware_Chain c;
    http_middleware_init(&c);
    http_middleware_add(&c, http_client_ip_middleware, &opts);
    void *data = NULL;
    Http_Handler_Fn fn = http_middleware_build(&c, handler, NULL, &data);
    Http_Response res;
    http_response_init(&res);
    fn(req, &res, data);
    http_middleware_free(&c);
    http_middleware_data_free(data);
    http_response_free(&res);
}

// reads the client until the response head and tail_bytes after it arrive
static void read_until(char *buf, size_t cap, Socket_Handle client,
                       const char *head_end, size_t tail_bytes)
{
    net_set_timeout(client, 3000);
    size_t got = 0;
    size_t he = strlen(head_end);
    for (;;) {
        for (size_t i = 0; i + he + tail_bytes <= got; i++) {
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

    Http_Cidr trusted[2];
    check("parse trusted list",
          http_cidr_parse(&trusted[0], "10.0.0.0/8") == 0 &&
          http_cidr_parse(&trusted[1], "192.168.1.0/24") == 0);

    // --- resolve(): an untrusted peer's header is ignored outright ---
    {
        String_View out;
        http_client_ip_resolve(sv_from_cstr("198.51.100.7"),
                               sv_from_cstr("203.0.113.9"),
                               trusted, 2, &out);
        check("untrusted peer keeps its own identity",
              sv_equal(out, sv_from_cstr("203.0.113.9")));
    }

    // --- resolve(): trusted peer, no header -> peer ---
    {
        String_View out;
        http_client_ip_resolve((String_View){0},
                               sv_from_cstr("10.0.0.7"),
                               trusted, 2, &out);
        check("trusted peer without a header stays the client",
              sv_equal(out, sv_from_cstr("10.0.0.7")));
    }

    // --- resolve(): rightmost non-trusted entry wins ---
    {
        String_View out;
        http_client_ip_resolve(sv_from_cstr("198.51.100.7, 10.0.0.1, 10.0.0.2"),
                               sv_from_cstr("10.0.0.2"),
                               trusted, 2, &out);
        check("rightmost non-trusted hop is the client",
              sv_equal(out, sv_from_cstr("198.51.100.7")));

        http_client_ip_resolve(sv_from_cstr("198.51.100.8, 10.0.0.1, 10.0.0.2"),
                               sv_from_cstr("10.0.0.2"),
                               trusted, 2, &out);
        check("proxy chain hops are skipped, client still found",
              sv_equal(out, sv_from_cstr("198.51.100.8")));
    }

    // --- resolve(): mix of IPv6 hops and CIDR trimming ---
    {
        String_View out;
        http_client_ip_resolve(sv_from_cstr("2001:db8::5"),
                               sv_from_cstr("10.0.0.3"),
                               trusted, 2, &out);
        check("ipv6 hop survives", sv_equal(out, sv_from_cstr("2001:db8::5")));

        http_client_ip_resolve(sv_from_cstr("10.0.0.1 , 10.0.0.2 ,"),
                               sv_from_cstr("10.0.0.2"),
                               trusted, 2, &out);
        check("all-trusted chain falls back to the peer",
              sv_equal(out, sv_from_cstr("10.0.0.2")));
    }

    // --- resolve(): unparsable header hops cannot name a client ---
    {
        String_View out;
        http_client_ip_resolve(sv_from_cstr("garbage-hop, 10.0.0.1"),
                               sv_from_cstr("10.0.0.1"),
                               trusted, 2, &out);
        check("garbage chain falls back to the peer",
              sv_equal(out, sv_from_cstr("10.0.0.1")));
    }

    // --- resolve(): empty trusted list disables header trust ---
    {
        String_View out;
        http_client_ip_resolve(sv_from_cstr("198.51.100.7"),
                               sv_from_cstr("203.0.113.9"),
                               NULL, 0, &out);
        check("no trusted proxies: header ignored", sv_equal(out, sv_from_cstr("203.0.113.9")));
    }

    // --- middleware: fills client_ip on a real parsed request ---
    {
        Http_Request req;
        check("parse spoofed request",
              http_request_parse(&req, sv_from_cstr(
                  "GET / HTTP/1.1\r\n"
                  "Host: x\r\n"
                  "X-Forwarded-For: 198.51.100.7, 10.0.0.2\r\n\r\n")) == REQ_OK);
        req.remote = sv_from_cstr("10.0.0.2");
        Http_Request pre = req;
        run_ip_middleware(&req, trusted, 2);
        check("middleware records the resolved client",
              sv_equal(req.client_ip, sv_from_cstr("198.51.100.7")));
        (void)pre;
        http_request_free(&req);
    }

    // --- middleware: no trusted list just echoes the peer ---
    {
        Http_Request req;
        check("parse plain request",
              http_request_parse(&req, sv_from_cstr(
                  "GET / HTTP/1.1\r\nHost: x\r\n"
                  "X-Forwarded-For: 198.51.100.7\r\n\r\n")) == REQ_OK);
        req.remote = sv_from_cstr("127.0.0.1");
        run_ip_middleware(&req, NULL, 0);
        check("without a trusted proxy the peer stands in",
              sv_equal(req.client_ip, sv_from_cstr("127.0.0.1")));
        http_request_free(&req);
    }

    // --- full connection: trusted loopback peer + forwarded header ---
    {
        Http_Cidr loopback;
        check("parse loopback trust", http_cidr_parse(&loopback, "127.0.0.1/32") == 0);

        Socket_Handle srv = net_listen(0);
        Socket_Handle client = net_connect("127.0.0.1", net_bound_port(srv));
        Socket_Handle peer = net_accept(srv);

        static const char reqs[] =
            "GET / HTTP/1.1\r\n"
            "Host: x\r\n"
            "X-Forwarded-For: 203.0.113.5\r\n"
            "\r\n";
        net_send_all(client, reqs, strlen(reqs));

        Http_ClientIp_Options opts = {&loopback, 1};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_client_ip_middleware, &opts);
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, handler, NULL, &data);
        Http_Server_Config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.workers = 0;
        http_serve_connection_config(peer, fn, data, &cfg);
        http_middleware_free(&c);
        http_middleware_data_free(data);

        char wire[4096] = {0};
        read_until(wire, sizeof wire, client, "\r\n\r\n", 22);
        check("wire: trusted peer exposes the forwarded client",
              strstr(wire, "X-Client-IP: 203.0.113.5") != NULL);

        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    // --- full connection: untrusted peer's header must not be believed ---
    {
        Socket_Handle srv = net_listen(0);
        Socket_Handle client = net_connect("127.0.0.1", net_bound_port(srv));
        Socket_Handle peer = net_accept(srv);

        static const char reqs[] =
            "GET / HTTP/1.1\r\n"
            "Host: x\r\n"
            "X-Forwarded-For: 203.0.113.5\r\n"
            "\r\n";
        net_send_all(client, reqs, strlen(reqs));

        Http_ClientIp_Options opts = {NULL, 0};
        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_client_ip_middleware, &opts);
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, handler, NULL, &data);
        Http_Server_Config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.workers = 0;
        http_serve_connection_config(peer, fn, data, &cfg);
        http_middleware_free(&c);
        http_middleware_data_free(data);

        char wire[4096] = {0};
        read_until(wire, sizeof wire, client, "\r\n\r\n", 22);
        check("wire: untrusted peer exposes itself, not the spoof",
              strstr(wire, "X-Client-IP: 127.0.0.1") != NULL &&
                  strstr(wire, "X-Client-IP: 203.0.113.5") == NULL);

        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    if (fails == 0) {
        printf("client_ip ok\n");
    }
    return fails != 0;
}