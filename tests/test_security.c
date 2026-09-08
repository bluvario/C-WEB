#include <stdio.h>
#include <string.h>

#include "response.h"
#include "security.h"
#include "strbuf.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// tail handler for the middleware test: proves control reached the next link
static void recorder(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    (void)user_data;
    http_response_set_status(res, HTTP_200_OK);
}

int main(void)
{
    int fails = 0;

    Http_Response res;
    http_response_init(&res);
    http_response_security_headers(&res, NULL);

    Strbuf wire;
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);

    if (strstr(wire.items, "X-Content-Type-Options: nosniff\r\n") == NULL ||
        strstr(wire.items, "Referrer-Policy: no-referrer\r\n") == NULL ||
        strstr(wire.items, "X-Frame-Options: DENY\r\n") == NULL) {
        fprintf(stderr, "hardening headers missing:\n%s\n", wire.items);
        return 1;
    }
    fails += check("baseline hardening headers emitted", 1);

    const char *def_csp = strstr(wire.items, "Content-Security-Policy: ");
#define CSP_HDR "Content-Security-Policy: "
#define DEF_CSP "default-src 'self'; frame-ancestors 'none'; object-src 'none'"
    if (def_csp == NULL ||
        strncmp(def_csp + sizeof(CSP_HDR) - 1, DEF_CSP, sizeof(DEF_CSP) - 1) != 0) {
        fprintf(stderr, "default CSP wrong or missing:\n%s\n", wire.items);
        return 1;
    }
    fails += check("default CSP emitted", 1);
    http_response_free(&res);
    strbuf_free(&wire);

    // a handler-supplied CSP replaces the baseline default, not adds to it
    http_response_init(&res);
    http_response_security_headers(&res, "default-src https://cdn.example");
    Strbuf w2;
    strbuf_init(&w2);
    http_response_serialize(&res, &w2);
    strbuf_null_terminate(&w2);
    char cspwanted[128];
    snprintf(cspwanted, sizeof(cspwanted), "Content-Security-Policy: %s\r\n",
             "default-src https://cdn.example");
    fails += check("custom CSP used",
        strstr(w2.items, cspwanted) != NULL &&
        strstr(w2.items, "default-src 'self'") == NULL);
    http_response_free(&res);
    strbuf_free(&w2);

    // the middleware stamps the same headers, then hands control onward
    http_response_init(&res);
    http_security_middleware(NULL, &res, NULL, recorder, NULL);
    Strbuf w3;
    strbuf_init(&w3);
    http_response_serialize(&res, &w3);
    strbuf_null_terminate(&w3);
    fails += check("middleware stamps hardening headers",
        res.status == HTTP_200_OK &&
        strstr(w3.items, "X-Content-Type-Options: nosniff\r\n") != NULL &&
        strstr(w3.items, "X-Frame-Options: DENY\r\n") != NULL &&
        strstr(w3.items, "Content-Security-Policy: ") != NULL);
    http_response_free(&res);
    strbuf_free(&w3);

    // a user_data csp override flows through the middleware unchanged
    http_response_init(&res);
    http_security_middleware(NULL, &res, "default-src 'none'; upgrade-insecure-requests",
                             recorder, NULL);
    Strbuf w4;
    strbuf_init(&w4);
    http_response_serialize(&res, &w4);
    strbuf_null_terminate(&w4);
    fails += check("middleware honors the csp override",
        strstr(w4.items, "Content-Security-Policy: default-src 'none'; upgrade-insecure-requests\r\n") != NULL);
    http_response_free(&res);
    strbuf_free(&w4);

    if (fails == 0) {
        printf("security ok\n");
    }
    return fails != 0;
}