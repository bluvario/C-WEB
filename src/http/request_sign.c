#include "request_sign.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hmac.h"
#include "xmem.h"

#define DEFAULT_MAX_AGE_SECONDS 300UL
#define MAC_HEX_LEN 64

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

static long long now_unix(void)
{
    return (long long)time(NULL);
}

void http_request_signature_compute(const char *method, const char *path,
                                    size_t path_len, const char *body,
                                    size_t body_len, long long unix_seconds,
                                    const char *secret, char *hex,
                                    size_t hex_size)
{
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", unix_seconds);

    size_t method_len = strlen(method);
    size_t ts_len = strlen(ts);
    size_t msg_len = method_len + 1 + path_len + 1 + ts_len + 1 + body_len;
    char *msg = xmalloc(msg_len);
    size_t n = 0;
    memcpy(msg + n, method, method_len);
    n += method_len;
    msg[n++] = '\n';
    if (path_len > 0) {
        memcpy(msg + n, path, path_len);
    }
    n += path_len;
    msg[n++] = '\n';
    memcpy(msg + n, ts, ts_len);
    n += ts_len;
    msg[n++] = '\n';
    if (body_len > 0) {
        memcpy(msg + n, body, body_len);
    }
    n += body_len;

    hmac_sha256_hex((const unsigned char *)secret, strlen(secret),
                    (const unsigned char *)msg, n, hex, hex_size);
    xfree(msg);
}

int http_request_signature_verify(Http_Request *req, const char *secret,
                                  unsigned long max_age_seconds)
{
    if (req == NULL || secret == NULL) {
        return -1;
    }

    const String_View *sig = http_request_get_header(req, CWEB_SIGN_MAC_HEADER);
    const String_View *date = http_request_get_header(req, CWEB_SIGN_DATE_HEADER);
    if (sig == NULL || sig->count != MAC_HEX_LEN || date == NULL) {
        return -1;
    }

    for (size_t i = 0; i < MAC_HEX_LEN; i++) {
        if (hex_digit(sig->data[i]) < 0) {
            return -1; // hex, lowercase only
        }
    }

    char dateraw[64];
    size_t date_n = date->count < sizeof(dateraw) - 1 ? date->count
                                                      : sizeof(dateraw) - 1;
    memcpy(dateraw, date->data, date_n);
    dateraw[date_n] = '\0';

    errno = 0;
    char *end = NULL;
    long long ts = strtoll(dateraw, &end, 10);
    if (errno != 0 || end == dateraw || *end != '\0') {
        return -1; // not a plain integer
    }

    if (max_age_seconds == 0) {
        max_age_seconds = DEFAULT_MAX_AGE_SECONDS;
    }
    long long now = now_unix();
    long long diff = now > ts ? now - ts : ts - now;
    if (diff < 0 || (unsigned long long)diff > max_age_seconds) {
        return -1; // replayed or stale
    }

    const char *method = http_method_name(req->method);
    if (method == NULL) {
        method = "???";
    }

    char want[MAC_HEX_LEN + 1];
    http_request_signature_compute(method, req->path.data, req->path.count,
                                   req->body.data, req->body.count, ts, secret,
                                   want, sizeof(want));

    return secure_byte_equal((const unsigned char *)sig->data,
                             (const unsigned char *)want, MAC_HEX_LEN)
               ? 0
               : -1;
}

void http_signature_middleware(Http_Request *req, Http_Response *res,
                               void *user_data,
                               Http_Handler_Fn next, void *next_data)
{
    const Http_Signature_Options *opts = user_data;
    const char *secret = opts != NULL ? opts->secret : NULL;
    unsigned long age = opts != NULL ? opts->max_age_seconds : 0;

    if (secret == NULL || secret[0] == '\0' ||
        http_request_signature_verify(req, secret, age) != 0) {
        http_response_set_status(res, HTTP_403_FORBIDDEN);
        http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
        http_response_add_body_cstr(res, "missing or invalid request signature\r\n");
        return; // refuse before a page runs
    }

    next(req, res, next_data);
}