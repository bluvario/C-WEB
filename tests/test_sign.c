#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "middleware.h"
#include "request.h"
#include "request_sign.h"
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

static void uppercase(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'f') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

// builds a raw HTTP request with a freshly computed signature for these facts
static size_t build_request(char *buf, size_t buf_size, const char *method,
                            const char *path, const char *body, size_t body_len,
                            long long ts, const char *secret)
{
    char hex[65];
    http_request_signature_compute(method, path, strlen(path), body, body_len,
                                   ts, secret, hex, sizeof(hex));
    size_t n = snprintf(buf, buf_size,
                        "%s %s HTTP/1.1\r\n"
                        "Host: example.test\r\n"
                        "X-CWEB-Date: %lld\r\n"
                        "X-CWEB-Signature: %s\r\n"
                        "\r\n",
                        method, path, ts, hex);
    if (body != NULL && body_len > 0) {
        n += snprintf(buf + n, buf_size - n, "%s", body);
    }
    return n;
}

static void ok_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    (void)user_data;
    res->status = HTTP_200_OK;
}

static long dispatch_chain(Http_Request *req, const char *secret)
{
    Http_Signature_Options opts = {0};
    opts.secret = secret;
    Http_Middleware_Chain chain;
    http_middleware_init(&chain);
    http_middleware_add(&chain, http_signature_middleware, &opts);
    void *data = NULL;
    Http_Handler_Fn dispatch = http_middleware_build(&chain, ok_handler, NULL, &data);
    http_middleware_free(&chain);

    Http_Response res;
    http_response_init(&res);
    dispatch(req, &res, data);
    http_middleware_data_free(data);

    long status = res.status;
    http_response_free(&res);
    return status;
}

int main(void)
{
    int fails = 0;
    const char *secret = "s3cr3t-shared-key";
    const char *wrong = "another-key";
    char raw[4096];

    long long now = (long long)time(NULL);

    // genuine GET, no body
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    Http_Request req;
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("valid GET verifies", http_request_signature_verify(&req, secret, 0) == 0);
    fails += check("valid GET fails under a different secret",
                   http_request_signature_verify(&req, wrong, 0) == -1);
    http_request_free(&req);

    // POST with a body: the canonical form covers raw body bytes
    build_request(raw, sizeof(raw), "POST", "/login", "user=bob&pass=123",
                  strlen("user=bob&pass=123"), now, secret);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("valid POST with body verifies",
                   http_request_signature_verify(&req, secret, 0) == 0);
    http_request_free(&req);

    // tampered signature byte
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    char *sigline = strstr(raw, "X-CWEB-Signature: ");
    sigline[37] = sigline[37] == 'a' ? 'b' : 'a';
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("tampered signature rejected",
                   http_request_signature_verify(&req, secret, 0) == -1);
    http_request_free(&req);

    // uppercase hex is rejected (must be lowercase, exactly 64 chars)
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    sigline = strstr(raw, "X-CWEB-Signature: ");
    uppercase(sigline + 18);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("uppercase signature rejected",
                   http_request_signature_verify(&req, secret, 0) == -1);
    http_request_free(&req);

    // missing headers
    snprintf(raw, sizeof(raw),
             "GET /login HTTP/1.1\r\nHost: example.test\r\n\r\n");
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("missing signature headers rejected",
                   http_request_signature_verify(&req, secret, 0) == -1);
    http_request_free(&req);

    // malformed timestamp
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    char *dateline = strstr(raw, "X-CWEB-Date: ");
    memcpy(dateline + 13, "not-a-number", 12);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("malformed date rejected",
                   http_request_signature_verify(&req, secret, 0) == -1);
    http_request_free(&req);

    // stale timestamp (older than the 300s default window)
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now - 1000, secret);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("stale date rejected",
                   http_request_signature_verify(&req, secret, 0) == -1);
    fails += check("stale date accepted with wider window",
                   http_request_signature_verify(&req, secret, 2000) == 0);
    http_request_free(&req);

    // future timestamp is a replay attack too
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now + 1000, secret);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("future date rejected",
                   http_request_signature_verify(&req, secret, 0) == -1);
    http_request_free(&req);

    // NULL secret never verifies
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("null secret rejected",
                   http_request_signature_verify(&req, NULL, 0) == -1);
    http_request_free(&req);

    // middleware: genuine request reaches the page, forged one gets a 403
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("middleware passes a valid signature",
                   dispatch_chain(&req, secret) == 200);
    http_request_free(&req);

    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    sigline = strstr(raw, "X-CWEB-Signature: ");
    sigline[37] = sigline[37] == 'a' ? 'b' : 'a';
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("middleware 403s a forged signature",
                   dispatch_chain(&req, secret) == 403);
    http_request_free(&req);

    // middleware with no secret refuses everything
    build_request(raw, sizeof(raw), "GET", "/login", NULL, 0, now, secret);
    http_request_parse(&req, (String_View){raw, strlen(raw)});
    fails += check("middleware without secret 403s",
                   dispatch_chain(&req, NULL) == 403);
    http_request_free(&req);

    if (fails == 0) {
        printf("sign ok\n");
    }
    return fails ? 1 : 0;
}