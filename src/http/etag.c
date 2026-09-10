#include "etag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hmac.h"
#include "http.h"
#include "request.h"
#include "response.h"
#include "sv.h"

// does the comma-separated If-None-Match header list match our etag? per
// RFC 9110 each entry is a quoted string (a weak and a strong etag of the
// same value compare equal for validation) or "*", which matches whenever the
// representation exists.
static int inm_matches(String_View header, String_View ours)
{
    // strip the surrounding quotes (and a W/ prefix) off our own tag once
    if (ours.count >= 2 && ours.data[0] == 'W' && ours.data[1] == '/' &&
        ours.count >= 4 && ours.data[2] == '"' &&
        ours.data[ours.count - 1] == '"') {
        ours.data += 3;
        ours.count -= 4;
    } else if (ours.count >= 2 && ours.data[0] == '"' &&
               ours.data[ours.count - 1] == '"') {
        ours.data += 1;
        ours.count -= 2;
    }

    String_View rest = header;
    while (rest.count > 0) {
        String_View entry = sv_trim(sv_chop_by_delim(&rest, ','));
        if (entry.count == 0) {
            continue;
        }
        if (sv_equal(entry, sv_from_cstr("*"))) {
            return 1;
        }
        if (entry.count >= 2 && entry.data[0] == '"' &&
            entry.data[entry.count - 1] == '"') {
            entry.data += 1;
            entry.count -= 2;
            if (entry.count >= 2 && entry.data[0] == 'W' && entry.data[1] == '/') {
                entry.data += 2;
                entry.count -= 2;
            }
            if (sv_equal(entry, ours)) {
                return 1;
            }
        }
    }
    return 0;
}

static void hex64(const unsigned char *in, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[64] = '\0';
}

void http_etag_middleware(Http_Request *req, Http_Response *res,
                          void *user_data,
                          Http_Handler_Fn next, void *next_data)
{
    // run the chain first so the body is fully assembled
    next(req, res, next_data);

    if (res->status < 200 || res->status >= 300) {
        return; // only cacheable successful responses are validated
    }
    if (res->stream_fn != NULL || res->stream_chunk_count > 0) {
        return; // a streamed entity has no single body to hash
    }
    if (res->body.count == 0) {
        return; // empty bodies carry nothing to validate
    }

    char *owned = http_response_get_header(res, "ETag");
    String_View tag;
    if (owned == NULL) {
        // the handler did not stamp a validator: synthesize a strong etag
        // from the body bytes unless the options ask for a weak one
        unsigned char digest[32];
        sha256((const unsigned char *)res->body.items, res->body.count, digest);
        char hex[65];
        hex64(digest, hex);
        char tag_buf[96];
        const Http_Etag_Options *opts = user_data;
        snprintf(tag_buf, sizeof tag_buf, opts != NULL && opts->weak
                                            ? "W/\"%s\"" : "\"%s\"", hex);
        http_response_set_header_replace(res, "ETag", tag_buf);
        tag = sv_from_cstr(tag_buf);
    } else {
        tag = sv_from_cstr(owned);
    }

    // a matching If-None-Match short-circuits: 304, body dropped (serialize
    // omits both the body and Content-Length for 304)
    const String_View *inm = http_request_get_header(req, "If-None-Match");
    if (inm != NULL && inm->count > 0 && inm_matches(*inm, tag)) {
        res->status = HTTP_304_NOT_MODIFIED;
        res->body.count = 0;
    }
    free(owned);
}