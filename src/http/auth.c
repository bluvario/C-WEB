#include "auth.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "strbuf.h"
#include "sv.h"

int http_basic_auth_parse(String_View header,
                          char *user, size_t user_size,
                          char *pass, size_t pass_size)
{
    header = sv_trim(header);
    // scheme names are case-insensitive, "Basic " is the usual spelling
    if (header.count < 6 ||
        tolower((unsigned char)header.data[0]) != 'b' ||
        tolower((unsigned char)header.data[1]) != 'a' ||
        tolower((unsigned char)header.data[2]) != 's' ||
        tolower((unsigned char)header.data[3]) != 'i' ||
        tolower((unsigned char)header.data[4]) != 'c' ||
        header.data[5] != ' ') {
        return -1;
    }
    header.data += 6;
    header.count -= 6;
    if (header.count == 0) {
        return -1;
    }

    Strbuf decoded;
    strbuf_init(&decoded);
    if (base64_decode_into(&decoded, header) != 0) {
        strbuf_free(&decoded);
        return -1;
    }

    // the wire form is "user:pass" with exactly one colon separating them
    size_t colon = 0;
    while (colon < decoded.count && decoded.items[colon] != ':') {
        colon++;
    }
    if (colon == 0 || colon >= decoded.count) {
        strbuf_free(&decoded);
        return -1;
    }
    size_t user_len = colon;
    size_t pass_len = decoded.count - colon - 1;

    size_t uc = user_len < user_size - 1 ? user_len : user_size - 1;
    memcpy(user, decoded.items, uc);
    user[uc] = '\0';

    size_t pc = pass_len < pass_size - 1 ? pass_len : pass_size - 1;
    memcpy(pass, decoded.items + colon + 1, pc);
    pass[pc] = '\0';

    strbuf_free(&decoded);
    return 0;
}

void http_response_require_basic_auth(Http_Response *res, const char *realm)
{
    // escape the realm for the quoted-string grammar of WWW-Authenticate
    char quoted[256];
    size_t n = 0;
    for (size_t i = 0; realm[i] != '\0' && n + 1 < sizeof(quoted); i++) {
        if (realm[i] == '"' || realm[i] == '\\') {
            quoted[n++] = '\\';
        }
        quoted[n++] = realm[i];
    }
    quoted[n] = '\0';

    char challenge[sizeof(quoted) + 32];
    snprintf(challenge, sizeof(challenge), "Basic realm=\"%s\"", quoted);

    http_response_set_status(res, HTTP_401_UNAUTHORIZED);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    http_response_set_header(res, "WWW-Authenticate", challenge);
    http_response_add_body_cstr(res, "authentication required\n");
}