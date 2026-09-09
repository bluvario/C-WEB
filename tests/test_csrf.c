#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csrf.h"
#include "response.h"
#include "session.h"
#include "strmap.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static int token_shape_ok(const char *tok)
{
    if (tok == NULL || strlen(tok) != 40) {
        return 0;
    }
    for (size_t i = 0; i < 40; i++) {
        char c = tok[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    int fails = 0;

    Http_Session_Store s;
    http_session_store_init(&s, 0);
    char *token = http_session_create(&s, 0);
    fails += check("session created", token != NULL);
    Http_Session *sesh = http_session_open(&s, token);
    fails += check("session opened", sesh != NULL);

    // the token is generated lazily, shaped like the session tokens, and stays
    const char *tok1 = http_csrf_token(sesh);
    fails += check("token is 40 hex chars", token_shape_ok(tok1));
    fails += check("token is stable across calls", http_csrf_token(sesh) == tok1);

    // a second session gets its own, different token
    char *token2 = http_session_create(&s, 0);
    Http_Session *sesh2 = http_session_open(&s, token2);
    const char *tok2 = http_csrf_token(sesh2);
    fails += check("second session has its own token",
                   tok2 != NULL && strcmp(tok2, tok1) != 0);

    // the hidden input carries the session's token verbatim
    Http_Response res;
    http_response_init(&res);
    http_csrf_field(&res, sesh);
    strbuf_null_terminate(&res.body);
    char want[128];
    snprintf(want, sizeof want,
             "<input type=\"hidden\" name=\"csrf_token\" value=\"%s\">", tok1);
    fails += check("field emits a hidden input with the token",
                   strcmp(res.body.items, want) == 0);
    http_response_free(&res);

    // a matching submitted token verifies, everything else fails clean
    Str_Map params;
    strmap_init(&params);
    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr(tok1));
    fails += check("matching token verifies", http_csrf_verify(&params, sesh) == 0);

    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"));
    fails += check("wrong token is refused", http_csrf_verify(&params, sesh) == -1);

    strmap_delete(&params, sv_from_cstr("csrf_token"));
    fails += check("missing token is refused", http_csrf_verify(&params, sesh) == -1);

    fails += check("no session means no verification", http_csrf_verify(&params, NULL) == -1);
    fails += check("token helper is null-safe", http_csrf_token(NULL) == NULL);

    // a stale session (no token ever served) cannot verify even a present field
    char *token3 = http_session_create(&s, 0);
    Http_Session *sesh3 = http_session_open(&s, token3);
    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr("anything"));
    fails += check("session without a served token is refused",
                   http_csrf_verify(&params, sesh3) == -1);

    strmap_free(&params);

    // the reject answer is a 403 with an escaped retry link
    http_response_init(&res);
    http_csrf_reject(&res, "/?x=1&y=<b>");
    strbuf_null_terminate(&res.body);
    fails += check("reject answers 403", res.status == HTTP_403_FORBIDDEN);
    fails += check("reject escapes the retry link",
                   strstr(res.body.items, "href=\"/?x=1&amp;y=&lt;b&gt;\">try again</a>") != NULL);
    fails += check("reject carries html content-type",
                   http_response_has_header(&res, "Content-Type"));
    http_response_free(&res);

    http_session_store_free(&s);

    if (fails == 0) {
        printf("csrf ok\n");
    }
    return fails ? 1 : 0;
}